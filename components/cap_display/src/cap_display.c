/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The screen, as tools the agent can call.
 *
 * Two decisions worth explaining.
 *
 * Emotions are a drawn face, not bitmaps and not an emoji font. Both of those
 * cost flash for something whose every frame is two rounded rectangles; see
 * cap_display_face.c, which owns the geometry and the animation. What matters
 * here is that the face is this robot's home screen, so the tools below move
 * it rather than take the panel over: showing a message borrows the screen and
 * gives it back, and clearing returns to the face rather than to a blank
 * panel that looks like a crash.
 *
 * The session is SHARED_LVGL, not EXCLUSIVE. Exclusive would fight the system
 * UI and any Lua skill that wants to draw; shared means the last writer wins
 * and nothing gets locked out. A tool the model calls casually should not be
 * able to take the screen away from a running skill.
 */

#include "cap_display.h"
#include "cap_display_face.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "driver/ledc.h"
#include "esp_board_device.h"
#include "esp_log.h"
#include "lvgl.h"
#include "periph_ledc.h"

#include "display_service.h"

static const char *TAG = "cap_display";

#define CAP_DISPLAY_TEXT_MAX 128
#define CAP_DISPLAY_BRIGHTNESS_DEVICE "lcd_brightness"

#define CAP_DISPLAY_TEXT_HOLD_MS 8000

static display_service_session_handle_t s_session;
static lv_obj_t   *s_text_screen;
static lv_obj_t   *s_label;
static lv_timer_t *s_text_timer;

/* ── The screen ────────────────────────────────────────────────────────── */

static esp_err_t ensure_session(void)
{
    display_service_session_config_t cfg = {
        .owner_name = "cap_display",
        .mode = DISPLAY_SERVICE_MODE_SHARED_LVGL,
        .flags = DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE,
    };
    esp_err_t err;

    if (!display_service_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session != NULL && display_service_session_is_valid(s_session)) {
        return ESP_OK;
    }

    err = display_service_open(&cfg, &s_session);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_service_open failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* Put the face back on the panel. Caller holds the LVGL lock. */
static esp_err_t show_face_locked(void)
{
    lv_obj_t *face = cap_display_face_screen_locked();

    if (!face) {
        return ESP_ERR_NO_MEM;
    }
    return display_service_session_load_screen_locked(s_session, face);
}

static void text_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Already inside LVGL, so the lock is held. */
    (void)show_face_locked();
    s_text_timer = NULL;
}

/* Show `text` scaled to fill the panel, then hand the screen back to the face.
 *
 * The hand-back is the point. The face is the robot's resting state, so a
 * message is something it says and stops saying -- without the timer, one
 * display_show_text call would leave a wall of words up until the next tool
 * call, which on a quiet robot could be hours.
 *
 * Caller must NOT hold the LVGL lock: this takes it. */
static esp_err_t paint_text(const char *text)
{
    esp_err_t err = ensure_session();

    if (err != ESP_OK) {
        return err;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_text_screen == NULL) {
        s_text_screen = lv_obj_create(NULL);
        if (s_text_screen == NULL) {
            display_service_unlock();
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_color(s_text_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_text_screen, LV_OPA_COVER, LV_PART_MAIN);

        s_label = lv_label_create(s_text_screen);
        lv_obj_set_style_text_color(s_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_label, lv_pct(90));
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(s_label);
    }

    lv_label_set_text(s_label, text);
    err = display_service_session_load_screen_locked(s_session, s_text_screen);

    if (s_text_timer) {
        lv_timer_reset(s_text_timer);
    } else {
        s_text_timer = lv_timer_create(text_expired_cb, CAP_DISPLAY_TEXT_HOLD_MS, NULL);
        if (s_text_timer) {
            lv_timer_set_repeat_count(s_text_timer, 1);
        }
    }
    display_service_unlock();
    return err;
}

/* ── Boot ──────────────────────────────────────────────────────────────── */

esp_err_t cap_display_face_start(void)
{
    esp_err_t err = ensure_session();

    if (err != ESP_OK) {
        /* A board with no panel is a normal configuration, not a failure. */
        ESP_LOGI(TAG, "no display on this board; face not started");
        return ESP_OK;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    err = show_face_locked();
    display_service_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "face is up");
    }
    return err;
}

void cap_display_face_set_network(bool sta_connected, const char *ap_ssid, const char *ip)
{
    char line[64];

    /* What someone standing in front of the robot needs, in the order they
     * need it: before joining, the AP name; after, the address to type. */
    if (sta_connected && ip && ip[0]) {
        snprintf(line, sizeof(line), "%s", ip);
    } else if (ap_ssid && ap_ssid[0]) {
        snprintf(line, sizeof(line), "join %s", ap_ssid);
    } else {
        snprintf(line, sizeof(line), "no network");
    }
    (void)cap_display_face_set_status(line);
}

/* ── Tools ─────────────────────────────────────────────────────────────── */

static esp_err_t execute_show_text(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *text;
    char buf[CAP_DISPLAY_TEXT_MAX];

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' is required");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(buf, sizeof(buf), "%s", text->valuestring);
    cJSON_Delete(root);

    if (paint_text(buf) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: the display is not available on this device.");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(output, output_size, "{\"ok\":true,\"showing\":\"%s\"}", buf);
    return ESP_OK;
}

static esp_err_t execute_show_emotion(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *emotion;
    char name[24];
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    emotion = cJSON_GetObjectItem(root, "emotion");
    if (!cJSON_IsString(emotion) || !emotion->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'emotion' is required");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(name, sizeof(name), "%s", emotion->valuestring);
    cJSON_Delete(root);

    if (!cap_display_face_is_emotion(name)) {
        size_t at = (size_t)snprintf(output, output_size,
                                     "Error: no such emotion. Try one of: ");
        if (at < output_size) {
            cap_display_face_emotion_names(output + at, output_size - at);
        }
        return ESP_OK;
    }

    if (ensure_session() != ESP_OK || cap_display_face_set_emotion(name) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: the display is not available on this device.");
        return ESP_ERR_INVALID_STATE;
    }

    /* An expression change also cancels any message still on screen: the robot
     * reacting to what was just said should be visible immediately, not in
     * eight seconds when the previous text times out. */
    if (display_service_lock() == ESP_OK) {
        if (s_text_timer) {
            lv_timer_delete(s_text_timer);
            s_text_timer = NULL;
        }
        err = show_face_locked();
        display_service_unlock();
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: could not show the face.");
            return err;
        }
    }
    snprintf(output, output_size, "{\"ok\":true,\"emotion\":\"%s\"}", name);
    return ESP_OK;
}

static esp_err_t execute_clear(const char *input_json,
                               const claw_cap_call_context_t *ctx,
                               char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    /* Back to the face rather than closing the session. The face is this
     * robot's home screen -- releasing the session would restore ESP-Claw's
     * launcher, which on a board with no touch controller is a screen nobody
     * can do anything with. */
    if (s_session != NULL && display_service_lock() == ESP_OK) {
        if (s_text_timer) {
            lv_timer_delete(s_text_timer);
            s_text_timer = NULL;
        }
        (void)show_face_locked();
        display_service_unlock();
    }
    snprintf(output, output_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t execute_set_brightness(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root;
    const cJSON *percent_json;
    void *handle = NULL;
    periph_ledc_handle_t *ledc;
    int percent;
    uint32_t duty;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    percent_json = cJSON_GetObjectItem(root, "percent");
    if (!cJSON_IsNumber(percent_json)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'percent' (0-100) is required");
        return ESP_ERR_INVALID_ARG;
    }
    percent = (int)percent_json->valuedouble;
    cJSON_Delete(root);

    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }

    if (esp_board_device_get_handle(CAP_DISPLAY_BRIGHTNESS_DEVICE, &handle) != ESP_OK ||
            handle == NULL) {
        snprintf(output, output_size,
                 "Error: this board has no controllable backlight.");
        return ESP_ERR_NOT_FOUND;
    }

    /* The board declares the backlight as a 10-bit LEDC channel. Duty is
     * scaled against that resolution; output_invert in the board YAML handles
     * the P-channel FET, so 100 here always means bright. */
    ledc = (periph_ledc_handle_t *)handle;
    duty = (uint32_t)(((1U << LEDC_TIMER_10_BIT) - 1U) * percent / 100);

    if (ledc_set_duty(ledc->speed_mode, ledc->channel, duty) != ESP_OK ||
            ledc_update_duty(ledc->speed_mode, ledc->channel) != ESP_OK) {
        snprintf(output, output_size, "Error: could not set the backlight duty.");
        return ESP_FAIL;
    }

    snprintf(output, output_size, "{\"ok\":true,\"percent\":%d}", percent);
    return ESP_OK;
}

/* ── Descriptors ───────────────────────────────────────────────────────── */

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "display_show_text",
        .name = "display_show_text",
        .family = "display",
        .description =
            "Show a short message on the robot's screen, centred and scaled to "
            "fill it. Keep it to a few words - this is a 320x240 panel seen "
            "from across a room, not a place to put a paragraph.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"text\":{\"type\":\"string\",\"maxLength\":120}},"
            "\"required\":[\"text\"]}",
        .execute = execute_show_text,
    },
    {
        .id = "display_show_emotion",
        .name = "display_show_emotion",
        .family = "display",
        .description =
            "Show a face on the robot's screen: neutral, happy, excited, sad, "
            "sleepy, angry, surprised, love, confused or wink. Use it to give "
            "the robot a reaction to what was said.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"emotion\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\","
            "\"excited\",\"sad\",\"sleepy\",\"angry\",\"surprised\",\"love\","
            "\"confused\",\"wink\"]}},\"required\":[\"emotion\"]}",
        .execute = execute_show_emotion,
    },
    {
        .id = "display_clear",
        .name = "display_clear",
        .family = "display",
        .description =
            "Stop showing a message and go back to the robot's resting face.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = execute_clear,
    },
    {
        .id = "display_set_brightness",
        .name = "display_set_brightness",
        .family = "display",
        .description =
            "Set the screen backlight from 0 to 100 percent. 0 turns it off, "
            "which is the polite thing to do when someone says the robot should "
            "sleep.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
            "\"required\":[\"percent\"]}",
        .execute = execute_set_brightness,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = "cap_display",
    .plugin_name = "Robot display",
    .version = "1",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_display_register_group(void)
{
    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "registering %u display tools", (unsigned)s_group.descriptor_count);
    return claw_cap_register_group(&s_group);
}
