/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_session_mgr.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_session_mgr";

#define CAP_SESSION_MGR_MAP_DIRNAME  "chat_map"
#define CAP_SESSION_MGR_PATH_SIZE    256
#define CAP_SESSION_MGR_KEY_SIZE     128
#define CAP_SESSION_MGR_ID_SIZE      128
#define CAP_SESSION_MGR_ALIAS_SIZE   33
#define CAP_SESSION_MGR_ALIAS_MAX    32
#define CAP_SESSION_MGR_MAX_SESSIONS 32
#define CAP_SESSION_MGR_FILE_MAX     4096
#define CAP_SESSION_MGR_DEFAULT_BASE "default_"

typedef struct {
    bool configured;
    char session_root_dir[160];
    char mapping_root_dir[192];
    SemaphoreHandle_t mutex;
} cap_session_mgr_state_t;

typedef struct {
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    char current_alias[CAP_SESSION_MGR_ALIAS_SIZE];
    uint32_t next_default_index;
    size_t session_count;
    char sessions[CAP_SESSION_MGR_MAX_SESSIONS][CAP_SESSION_MGR_ALIAS_SIZE];
} cap_session_mgr_alias_map_t;

static cap_session_mgr_state_t s_session_mgr = {0};

static bool cap_session_mgr_is_ascii_space(char ch);

static bool cap_session_mgr_is_chat_event(const claw_event_t *event)
{
    return event &&
           strcmp(event->event_type, "message") == 0 &&
           event->source_channel[0] != '\0' &&
           event->chat_id[0] != '\0';
}

static const char *cap_session_mgr_skip_ascii_space(const char *text)
{
    if (!text) {
        return "";
    }
    while (*text && cap_session_mgr_is_ascii_space(*text)) {
        text++;
    }

    return text;
}

static bool cap_session_mgr_text_is_command(const char *text, const char *command)
{
    size_t command_len;

    if (!text || !command || !command[0]) {
        return false;
    }

    command_len = strlen(command);
    if (strncmp(text, command, command_len) != 0) {
        return false;
    }

    return text[command_len] == '\0' || cap_session_mgr_is_ascii_space(text[command_len]);
}

static bool cap_session_mgr_is_session_command_event(const claw_event_t *event)
{
    const char *text = NULL;

    if (!event || !event->text) {
        return false;
    }

    text = cap_session_mgr_skip_ascii_space(event->text);
    return cap_session_mgr_text_is_command(text, "/new") ||
           cap_session_mgr_text_is_command(text, "/switch");
}

static uint32_t cap_session_mgr_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *ptr = (const unsigned char *)text;

    while (ptr && *ptr) {
        hash ^= *ptr++;
        hash *= 16777619u;
    }

    return hash;
}

static void cap_session_mgr_sanitize(const char *src, char *dst, size_t dst_size)
{
    size_t off = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size) {
        char ch = *src++;

        if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
            dst[off++] = ch;
        } else if (off == 0 || dst[off - 1] != '_') {
            dst[off++] = '_';
        }
    }
    if (off > 0 && dst[off - 1] == '_') {
        off--;
    }
    dst[off] = '\0';
}

static bool cap_session_mgr_is_ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static bool cap_session_mgr_is_alias_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' ||
           ch == '-';
}

static bool cap_session_mgr_is_valid_alias(const char *alias)
{
    size_t len;

    if (!alias) {
        return false;
    }
    len = strlen(alias);
    if (len == 0 || len > CAP_SESSION_MGR_ALIAS_MAX) {
        return false;
    }
    while (*alias) {
        if (!cap_session_mgr_is_alias_char(*alias++)) {
            return false;
        }
    }

    return true;
}

static esp_err_t cap_session_mgr_normalize_alias(const char *input,
                                                 char *alias,
                                                 size_t alias_size,
                                                 bool *out_has_alias)
{
    const char *start = input;
    const char *end = NULL;
    size_t len;

    if (!alias || alias_size == 0 || !out_has_alias) {
        return ESP_ERR_INVALID_ARG;
    }
    alias[0] = '\0';
    *out_has_alias = false;
    if (!input) {
        return ESP_OK;
    }

    while (*start && cap_session_mgr_is_ascii_space(*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && cap_session_mgr_is_ascii_space(*(end - 1))) {
        end--;
    }

    len = (size_t)(end - start);
    if (len == 0) {
        return ESP_OK;
    }
    if (len > CAP_SESSION_MGR_ALIAS_MAX || len >= alias_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        if (!cap_session_mgr_is_alias_char(start[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        alias[i] = start[i];
    }
    alias[len] = '\0';
    *out_has_alias = true;

    return ESP_OK;
}

static bool cap_session_mgr_alias_exists(const cap_session_mgr_alias_map_t *map, const char *alias)
{
    if (!map || !alias || !alias[0]) {
        return false;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        if (strcmp(map->sessions[i], alias) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t cap_session_mgr_validate_alias_map(const cap_session_mgr_alias_map_t *map)
{
    if (!map || !map->chat_key[0] ||
            !cap_session_mgr_is_valid_alias(map->current_alias) ||
            map->next_default_index == 0 ||
            map->session_count == 0 ||
            map->session_count > CAP_SESSION_MGR_MAX_SESSIONS) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        if (!cap_session_mgr_is_valid_alias(map->sessions[i])) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (size_t j = i + 1; j < map->session_count; j++) {
            if (strcmp(map->sessions[i], map->sessions[j]) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (!cap_session_mgr_alias_exists(map, map->current_alias)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t cap_session_mgr_build_chat_key(const char *source_channel, const char *chat_id, char *buf, size_t buf_size)
{
    int written;

    if (!source_channel || !source_channel[0] || !chat_id || !chat_id[0] || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(buf, buf_size, "%s:%s", source_channel, chat_id);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_build_mapping_path(const char *chat_key, char *path, size_t path_size)
{
    char safe_key[40];
    uint32_t hash;
    int written;

    if (!chat_key || !chat_key[0] || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_session_mgr_sanitize(chat_key, safe_key, sizeof(safe_key));
    if (strlen(safe_key) > 24) {
        safe_key[24] = '\0';
    }
    hash = cap_session_mgr_hash(chat_key);
    written = snprintf(path,
                       path_size,
                       "%s/chat_%s_%08" PRIx32 ".json",
                       s_session_mgr.mapping_root_dir,
                       safe_key[0] ? safe_key : "default",
                       hash);
    if (written < 0 || (size_t)written >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_read_file_limited(const char *path, char **out_text)
{
    FILE *file = NULL;
    long size;
    char *text = NULL;
    size_t read_bytes;

    if (!path || !path[0] || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || size > CAP_SESSION_MGR_FILE_MAX) {
        fclose(file);
        return size < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    if (read_bytes != (size_t)size && ferror(file)) {
        fclose(file);
        free(text);
        return ESP_FAIL;
    }
    fclose(file);
    text[read_bytes] = '\0';
    *out_text = text;

    return ESP_OK;
}

static esp_err_t cap_session_mgr_write_mapping_locked(const cap_session_mgr_alias_map_t *map)
{
    char path[CAP_SESSION_MGR_PATH_SIZE];
    cJSON *root = NULL;
    cJSON *sessions = NULL;
    char *json = NULL;
    FILE *file = NULL;
    esp_err_t err;

    err = cap_session_mgr_validate_alias_map(map);
    if (err != ESP_OK) {
        return err;
    }
    err = cap_session_mgr_build_mapping_path(map->chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    sessions = cJSON_CreateArray();
    if (!root || !sessions) {
        cJSON_Delete(root);
        cJSON_Delete(sessions);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "chat_key", map->chat_key) ||
            !cJSON_AddStringToObject(root, "current_alias", map->current_alias) ||
            !cJSON_AddNumberToObject(root, "next_default_index", map->next_default_index)) {
        cJSON_Delete(root);
        cJSON_Delete(sessions);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        cJSON *item = cJSON_CreateString(map->sessions[i]);

        if (!item) {
            cJSON_Delete(root);
            cJSON_Delete(sessions);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(sessions, item);
    }
    cJSON_AddItemToObject(root, "sessions", sessions);
    sessions = NULL;

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    file = fopen(path, "wb");
    if (!file) {
        free(json);
        return ESP_FAIL;
    }
    if (fputs(json, file) < 0) {
        fclose(file);
        free(json);
        return ESP_FAIL;
    }
    fclose(file);
    free(json);
    return ESP_OK;
}

static esp_err_t cap_session_mgr_load_mapping_locked(const char *chat_key,
                                                     cap_session_mgr_alias_map_t *out_map)
{
    char path[CAP_SESSION_MGR_PATH_SIZE];
    char *text = NULL;
    cJSON *root = NULL;
    cJSON *item = NULL;
    cJSON *sessions = NULL;
    esp_err_t err;

    if (!chat_key || !chat_key[0] || !out_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_map, 0, sizeof(*out_map));
    err = cap_session_mgr_build_mapping_path(chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    err = cap_session_mgr_read_file_limited(path, &text);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "chat_key");
    if (!cJSON_IsString(item) || !item->valuestring || strcmp(item->valuestring, chat_key) != 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(out_map->chat_key, item->valuestring, sizeof(out_map->chat_key));

    item = cJSON_GetObjectItemCaseSensitive(root, "current_alias");
    if (!cJSON_IsString(item) || !cap_session_mgr_is_valid_alias(item->valuestring)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(out_map->current_alias, item->valuestring, sizeof(out_map->current_alias));

    item = cJSON_GetObjectItemCaseSensitive(root, "next_default_index");
    if (!cJSON_IsNumber(item) || item->valueint < 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_map->next_default_index = (uint32_t)item->valueint;

    sessions = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    if (!cJSON_IsArray(sessions)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_map->session_count = (size_t)cJSON_GetArraySize(sessions);
    if (out_map->session_count == 0 || out_map->session_count > CAP_SESSION_MGR_MAX_SESSIONS) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < out_map->session_count; i++) {
        item = cJSON_GetArrayItem(sessions, (int)i);
        if (!cJSON_IsString(item) || !cap_session_mgr_is_valid_alias(item->valuestring)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        strlcpy(out_map->sessions[i], item->valuestring, sizeof(out_map->sessions[i]));
    }
    cJSON_Delete(root);

    return cap_session_mgr_validate_alias_map(out_map);
}

static esp_err_t cap_session_mgr_init_mapping_locked(const char *chat_key,
                                                     cap_session_mgr_alias_map_t *out_map)
{
    if (!chat_key || !chat_key[0] || !out_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_map, 0, sizeof(*out_map));
    strlcpy(out_map->chat_key, chat_key, sizeof(out_map->chat_key));
    strlcpy(out_map->current_alias, "default_01", sizeof(out_map->current_alias));
    out_map->next_default_index = 2;
    out_map->session_count = 1;
    strlcpy(out_map->sessions[0], "default_01", sizeof(out_map->sessions[0]));

    return cap_session_mgr_write_mapping_locked(out_map);
}

static esp_err_t cap_session_mgr_load_or_init_mapping_locked(const char *chat_key,
                                                             cap_session_mgr_alias_map_t *out_map)
{
    esp_err_t err;

    err = cap_session_mgr_load_mapping_locked(chat_key, out_map);
    if (err == ESP_ERR_NOT_FOUND) {
        return cap_session_mgr_init_mapping_locked(chat_key, out_map);
    }

    return err;
}

static esp_err_t cap_session_mgr_build_default_alias(const cap_session_mgr_alias_map_t *map,
                                                     char *alias,
                                                     size_t alias_size,
                                                     uint32_t *out_next_index)
{
    uint32_t index;
    int written;

    if (!map || !alias || alias_size == 0 || !out_next_index) {
        return ESP_ERR_INVALID_ARG;
    }

    index = map->next_default_index ? map->next_default_index : 1;
    while (index < UINT32_MAX) {
        written = snprintf(alias, alias_size, "%s%02" PRIu32, CAP_SESSION_MGR_DEFAULT_BASE, index);
        if (written < 0 || (size_t)written >= alias_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!cap_session_mgr_alias_exists(map, alias)) {
            *out_next_index = index + 1;
            return ESP_OK;
        }
        index++;
    }

    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t cap_session_mgr_build_current_session_id_locked(const char *source_channel,
                                                                 const char *chat_id,
                                                                 char *buf,
                                                                 size_t buf_size)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    cap_session_mgr_alias_map_t map;
    int written;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_load_or_init_mapping_locked(chat_key, &map);
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(buf, buf_size, "%s:%s", chat_key, map.current_alias);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_new_session_locked(const char *source_channel,
                                                    const char *chat_id,
                                                    const char *requested_alias,
                                                    bool has_requested_alias,
                                                    char *out_alias,
                                                    size_t out_alias_size,
                                                    char *out_session_id,
                                                    size_t out_session_id_size)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    cap_session_mgr_alias_map_t map;
    char new_alias[CAP_SESSION_MGR_ALIAS_SIZE];
    uint32_t next_index = 0;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_load_mapping_locked(chat_key, &map);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(&map, 0, sizeof(map));
        strlcpy(map.chat_key, chat_key, sizeof(map.chat_key));
        map.next_default_index = 1;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (map.session_count >= CAP_SESSION_MGR_MAX_SESSIONS) {
        return ESP_ERR_NO_MEM;
    }

    if (has_requested_alias) {
        if (cap_session_mgr_alias_exists(&map, requested_alias)) {
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(new_alias, requested_alias, sizeof(new_alias));
        next_index = map.next_default_index;
    } else {
        err = cap_session_mgr_build_default_alias(&map, new_alias, sizeof(new_alias), &next_index);
        if (err != ESP_OK) {
            return err;
        }
    }

    strlcpy(map.sessions[map.session_count], new_alias, sizeof(map.sessions[map.session_count]));
    map.session_count++;
    strlcpy(map.current_alias, new_alias, sizeof(map.current_alias));
    map.next_default_index = next_index;

    err = cap_session_mgr_write_mapping_locked(&map);
    if (err != ESP_OK) {
        return err;
    }

    if (out_alias && out_alias_size > 0) {
        strlcpy(out_alias, new_alias, out_alias_size);
    }
    if (out_session_id && out_session_id_size > 0) {
        return cap_session_mgr_build_current_session_id_locked(source_channel,
                                                              chat_id,
                                                              out_session_id,
                                                              out_session_id_size);
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_switch_session_locked(const char *source_channel,
                                                       const char *chat_id,
                                                       const char *alias,
                                                       char *out_alias,
                                                       size_t out_alias_size)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    cap_session_mgr_alias_map_t map;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_load_or_init_mapping_locked(chat_key, &map);
    if (err != ESP_OK) {
        return err;
    }
    if (!cap_session_mgr_alias_exists(&map, alias)) {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(map.current_alias, alias, sizeof(map.current_alias));
    err = cap_session_mgr_write_mapping_locked(&map);
    if (err != ESP_OK) {
        return err;
    }
    if (out_alias && out_alias_size > 0) {
        strlcpy(out_alias, alias, out_alias_size);
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_append_output(char *output,
                                               size_t output_size,
                                               size_t *offset,
                                               const char *fmt,
                                               ...)
{
    va_list args;
    int written;

    if (!output || output_size == 0 || !offset || *offset >= output_size) {
        return ESP_ERR_INVALID_ARG;
    }

    va_start(args, fmt);
    written = vsnprintf(output + *offset, output_size - *offset, fmt, args);
    va_end(args);
    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= output_size - *offset) {
        *offset = output_size - 1;
        return ESP_ERR_INVALID_SIZE;
    }
    *offset += (size_t)written;

    return ESP_OK;
}

static esp_err_t cap_session_mgr_list_sessions_locked(const char *source_channel,
                                                      const char *chat_id,
                                                      char *output,
                                                      size_t output_size)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    cap_session_mgr_alias_map_t map;
    size_t offset = 0;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_load_or_init_mapping_locked(chat_key, &map);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_append_output(output, output_size, &offset, "Sessions:");
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < map.session_count; i++) {
        err = cap_session_mgr_append_output(output,
                                            output_size,
                                            &offset,
                                            "\n* %s%s",
                                            map.sessions[i],
                                            strcmp(map.sessions[i], map.current_alias) == 0 ? " (current)" : "");
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_read_alias_input(const char *input_json,
                                                  char *alias,
                                                  size_t alias_size,
                                                  bool *out_has_alias)
{
    cJSON *root = NULL;
    cJSON *item = NULL;
    esp_err_t err;

    if (!alias || alias_size == 0 || !out_has_alias) {
        return ESP_ERR_INVALID_ARG;
    }
    alias[0] = '\0';
    *out_has_alias = false;

    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }
    root = cJSON_Parse(input_json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "alias");
    if (!item || cJSON_IsNull(item)) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (!cJSON_IsString(item)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_session_mgr_normalize_alias(item->valuestring, alias, alias_size, out_has_alias);
    cJSON_Delete(root);

    return err;
}

static void cap_session_mgr_write_message(char *output, size_t output_size, const char *message)
{
    if (output && output_size > 0) {
        snprintf(output, output_size, "%s", message ? message : "");
    }
}

static void cap_session_mgr_write_format(char *output, size_t output_size, const char *fmt, ...)
{
    va_list args;

    if (!output || output_size == 0 || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(output, output_size, fmt, args);
    va_end(args);
}

static bool cap_session_mgr_context_ready(const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    if (!ctx || !ctx->channel || !ctx->channel[0] || !ctx->chat_id || !ctx->chat_id[0]) {
        cap_session_mgr_write_message(output, output_size, "Session command failed: missing chat context.");
        ESP_LOGW(TAG, "Session command missing channel or chat_id");
        return false;
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        cap_session_mgr_write_message(output, output_size, "Session command failed: session manager is not configured.");
        ESP_LOGW(TAG, "Session manager command called before configuration");
        return false;
    }

    return true;
}

static esp_err_t cap_session_mgr_new_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    char alias[CAP_SESSION_MGR_ALIAS_SIZE];
    char created_alias[CAP_SESSION_MGR_ALIAS_SIZE];
    bool has_alias = false;
    esp_err_t err;

    if (!cap_session_mgr_context_ready(ctx, output, output_size)) {
        return ESP_OK;
    }

    err = cap_session_mgr_read_alias_input(input_json, alias, sizeof(alias), &has_alias);
    if (err == ESP_ERR_INVALID_SIZE) {
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Invalid session name: use 1-32 characters from A-Z, a-z, 0-9, _ or -.");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Invalid session name: use 1-32 characters from A-Z, a-z, 0-9, _ or -.");
        return ESP_OK;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = cap_session_mgr_new_session_locked(ctx->channel,
                                             ctx->chat_id,
                                             alias,
                                             has_alias,
                                             created_alias,
                                             sizeof(created_alias),
                                             NULL,
                                             0);
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err == ESP_ERR_INVALID_STATE) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot create session \"%s\": a session with that name already exists.",
                                     alias);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Create chat session failed for %s:%s: %s",
                 ctx->channel,
                 ctx->chat_id,
                 esp_err_to_name(err));
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Session command failed: unable to access session storage.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Created chat session %s:%s alias=%s", ctx->channel, ctx->chat_id, created_alias);
    cap_session_mgr_write_format(output, output_size, "Started a new session: %s", created_alias);
    return ESP_OK;
}

static esp_err_t cap_session_mgr_switch_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    char alias[CAP_SESSION_MGR_ALIAS_SIZE];
    char switched_alias[CAP_SESSION_MGR_ALIAS_SIZE];
    bool has_alias = false;
    esp_err_t err;

    if (!cap_session_mgr_context_ready(ctx, output, output_size)) {
        return ESP_OK;
    }

    err = cap_session_mgr_read_alias_input(input_json, alias, sizeof(alias), &has_alias);
    if (err == ESP_ERR_INVALID_SIZE) {
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Invalid session name: use 1-32 characters from A-Z, a-z, 0-9, _ or -.");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Invalid session name: use 1-32 characters from A-Z, a-z, 0-9, _ or -.");
        return ESP_OK;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    if (!has_alias) {
        err = cap_session_mgr_list_sessions_locked(ctx->channel, ctx->chat_id, output, output_size);
    } else {
        err = cap_session_mgr_switch_session_locked(ctx->channel,
                                                    ctx->chat_id,
                                                    alias,
                                                    switched_alias,
                                                    sizeof(switched_alias));
    }
    xSemaphoreGiveRecursive(s_session_mgr.mutex);

    if (err == ESP_ERR_NOT_FOUND) {
        cap_session_mgr_write_format(output,
                                     output_size,
                                     "Cannot switch to session \"%s\": no such session. Send /switch to list sessions.",
                                     alias);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Switch/list chat session failed for %s:%s: %s",
                 ctx->channel,
                 ctx->chat_id,
                 esp_err_to_name(err));
        cap_session_mgr_write_message(output,
                                      output_size,
                                      "Session command failed: unable to access session storage.");
        return ESP_OK;
    }
    if (has_alias) {
        ESP_LOGI(TAG, "Switched chat session %s:%s alias=%s", ctx->channel, ctx->chat_id, switched_alias);
        cap_session_mgr_write_format(output, output_size, "Switched to session: %s", switched_alias);
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_roll_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    char alias[CAP_SESSION_MGR_ALIAS_SIZE];
    char session_id[CAP_SESSION_MGR_ID_SIZE];
    esp_err_t err;

    (void)input_json;

    if (!ctx || !ctx->channel || !ctx->channel[0] || !ctx->chat_id || !ctx->chat_id[0]) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"channel and chat_id are required\"}");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"session manager not configured\"}");
        }
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = cap_session_mgr_new_session_locked(ctx->channel,
                                             ctx->chat_id,
                                             NULL,
                                             false,
                                             alias,
                                             sizeof(alias),
                                             session_id,
                                             sizeof(session_id));
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Roll chat session failed for %s:%s: %s",
                 ctx->channel,
                 ctx->chat_id,
                 esp_err_to_name(err));
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "Rolled chat session %s:%s to alias %s", ctx->channel, ctx->chat_id, alias);
    if (output && output_size > 0) {
        snprintf(output, output_size, "{\"ok\":true,\"session_id\":\"%s\",\"alias\":\"%s\"}", session_id, alias);
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_session_mgr_caps[] = {
    {
        .id = "new_chat_session",
        .name = "new_chat_session",
        .family = "system",
        .description = "Create and switch to a new aliased persistent chat session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"alias\":{\"type\":\"string\"}}}",
        .execute = cap_session_mgr_new_execute,
    },
    {
        .id = "switch_chat_session",
        .name = "switch_chat_session",
        .family = "system",
        .description = "List chat sessions or switch to an existing aliased chat session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"alias\":{\"type\":\"string\"}}}",
        .execute = cap_session_mgr_switch_execute,
    },
    {
        .id = "roll_chat_session",
        .name = "roll_chat_session",
        .family = "system",
        .description = "Advance the current chat to a fresh persistent session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_session_mgr_roll_execute,
    },
};

static const claw_cap_group_t s_session_mgr_group = {
    .group_id = "cap_session_mgr",
    .plugin_name = "cap_session_mgr",
    .version = "1.0.0",
    .descriptors = s_session_mgr_caps,
    .descriptor_count = sizeof(s_session_mgr_caps) / sizeof(s_session_mgr_caps[0]),
};

esp_err_t cap_session_mgr_register_group(void)
{
    return claw_cap_register_group(&s_session_mgr_group);
}

esp_err_t cap_session_mgr_set_session_root_dir(const char *session_root_dir)
{
    int written;
    SemaphoreHandle_t mutex = s_session_mgr.mutex;

    if (!session_root_dir || !session_root_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_session_mgr, 0, sizeof(s_session_mgr));
    s_session_mgr.mutex = mutex;
    strlcpy(s_session_mgr.session_root_dir, session_root_dir, sizeof(s_session_mgr.session_root_dir));
    written = snprintf(s_session_mgr.mapping_root_dir,
                       sizeof(s_session_mgr.mapping_root_dir),
                       "%s/%s",
                       session_root_dir,
                       CAP_SESSION_MGR_MAP_DIRNAME);
    if (written < 0 || (size_t)written >= sizeof(s_session_mgr.mapping_root_dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_session_mgr.mutex) {
        s_session_mgr.mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_session_mgr.mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (cap_session_mgr_ensure_dir(s_session_mgr.session_root_dir) != ESP_OK ||
            cap_session_mgr_ensure_dir(s_session_mgr.mapping_root_dir) != ESP_OK) {
        return ESP_FAIL;
    }

    s_session_mgr.configured = true;
    return ESP_OK;
}

size_t cap_session_mgr_build_session_id(const claw_event_t *event, char *buf, size_t buf_size, void *user_ctx)
{
    esp_err_t err;

    (void)user_ctx;

    if (!buf || buf_size == 0 || !event) {
        return 0;
    }
    if (!cap_session_mgr_is_chat_event(event) ||
            cap_session_mgr_is_session_command_event(event) ||
            !s_session_mgr.configured ||
            !s_session_mgr.mutex) {
        return claw_event_router_build_session_id(event, buf, buf_size);
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = cap_session_mgr_build_current_session_id_locked(event->source_channel, event->chat_id, buf, buf_size);
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Falling back to default session id for %s:%s: %s",
                 event->source_channel,
                 event->chat_id,
                 esp_err_to_name(err));
        return claw_event_router_build_session_id(event, buf, buf_size);
    }

    return strlen(buf);
}
