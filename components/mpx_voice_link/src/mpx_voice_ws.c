/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The xiaozhi-protocol WebSocket link.
 *
 * Connection and handshake only, for now. Audio streaming is the next step,
 * and keeping them apart matters: if the socket and the codec arrive
 * together, a handshake mismatch and a codec mismatch produce the same
 * symptom -- silence -- and there is no way to tell which you have.
 */

#include "mpx_voice_link.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mpx_voice_ws";

#define VOICE_SESSION_ID_MAX   64
#define VOICE_HEADER_MAX      320
#define VOICE_HANDSHAKE_MS   8000

/* The websocket client runs the event handler on its own task, and this
 * parses JSON there. 6 KB rather than the 4 KB default, because cJSON's
 * recursive descent plus a log line is not free and a stack overflow in a
 * callback is a miserable thing to diagnose. */
#define VOICE_WS_TASK_STACK  6144

static esp_websocket_client_handle_t s_client;
static mpx_voice_state_t s_state = MPX_VOICE_IDLE;
static char s_session_id[VOICE_SESSION_ID_MAX];
static uint32_t s_downlink_rate = MPX_VOICE_DOWNLINK_RATE;
static SemaphoreHandle_t s_handshake;
static uint32_t s_audio_frames;

/* Whether the WebSocket upgrade ever completed. The distinction matters when
 * reporting a failure: "the server never accepted the upgrade" and "the
 * server accepted it and then said nothing" have completely different
 * causes, and both look like a timeout from the caller's side. */
static bool s_socket_opened;

/* ── outgoing ────────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_send_json(const char *json)
{
    ESP_RETURN_ON_FALSE(s_client && json, ESP_ERR_INVALID_STATE, TAG, "not connected");

    const int len = (int)strlen(json);
    const int sent = esp_websocket_client_send_text(s_client, json, len,
                                                    pdMS_TO_TICKS(2000));
    ESP_RETURN_ON_FALSE(sent == len, ESP_FAIL, TAG, "send failed (%d of %d)", sent, len);
    return ESP_OK;
}

static esp_err_t voice_send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "out of memory");

    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");

    /* mcp:true is the flag that lets the server call back into this device.
     * It is what Phase 3 needs -- the server's model asking the robot to
     * change its face -- and it costs nothing to advertise now. */
    cJSON *features = cJSON_AddObjectToObject(root, "features");
    if (features) {
        cJSON_AddBoolToObject(features, "mcp", true);
    }

    cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
    if (audio) {
        cJSON_AddStringToObject(audio, "format", "opus");
        cJSON_AddNumberToObject(audio, "sample_rate", MPX_VOICE_UPLINK_RATE);
        cJSON_AddNumberToObject(audio, "channels", 1);
        cJSON_AddNumberToObject(audio, "frame_duration", MPX_VOICE_FRAME_MS);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "out of memory");

    ESP_LOGI(TAG, "-> %s", text);
    const esp_err_t err = mpx_voice_send_json(text);
    free(text);
    return err;
}

/* ── incoming ────────────────────────────────────────────────────────────── */

static void voice_handle_hello(const cJSON *root)
{
    const cJSON *session = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session) && session->valuestring) {
        strlcpy(s_session_id, session->valuestring, sizeof(s_session_id));
    }

    /* The server chooses the downlink rate, and it is not always the 24 kHz
     * the protocol documents -- so read it rather than assume it. Getting
     * this wrong plays speech at the wrong pitch, which sounds like a broken
     * decoder. */
    const cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio)) {
        const cJSON *rate = cJSON_GetObjectItem(audio, "sample_rate");
        if (cJSON_IsNumber(rate) && rate->valueint > 0) {
            s_downlink_rate = (uint32_t)rate->valueint;
        }
    }

    s_state = MPX_VOICE_READY;
    ESP_LOGI(TAG, "session ready: id=%s, downlink %" PRIu32 " Hz",
             s_session_id[0] ? s_session_id : "(none)", s_downlink_rate);
    ESP_LOGI(TAG, "internal RAM after connect: %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (s_handshake) {
        xSemaphoreGive(s_handshake);
    }
}

static void voice_handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable message (%d bytes)", len);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *kind = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(kind, "hello") == 0) {
        voice_handle_hello(root);
    } else if (strcmp(kind, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        ESP_LOGI(TAG, "heard: %s",
                 cJSON_IsString(text) ? text->valuestring : "(no text)");
    } else if (strcmp(kind, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        ESP_LOGI(TAG, "tts %s%s%s",
                 cJSON_IsString(state) ? state->valuestring : "?",
                 cJSON_IsString(text) ? ": " : "",
                 cJSON_IsString(text) ? text->valuestring : "");
    } else if (strcmp(kind, "llm") == 0) {
        /* Emotion hints. Phase 5 maps these onto the face; logging them now
         * shows what the server actually sends. */
        const cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
        ESP_LOGI(TAG, "emotion: %s",
                 cJSON_IsString(emotion) ? emotion->valuestring : "(none)");
    } else if (strcmp(kind, "mcp") == 0) {
        /* Phase 3. Logged rather than handled so the payload shape can be
         * seen before anything is written against it. */
        ESP_LOGI(TAG, "mcp message received (%d bytes) -- not handled yet", len);
    } else {
        ESP_LOGI(TAG, "<- %.*s", len > 200 ? 200 : len, data);
    }

    cJSON_Delete(root);
}

static void voice_ws_event(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "socket open");
            s_socket_opened = true;
            s_state = MPX_VOICE_HANDSHAKING;
            s_audio_frames = 0;
            if (voice_send_hello() != ESP_OK) {
                ESP_LOGE(TAG, "could not send hello");
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x01 && ev->data_len > 0) {
                voice_handle_text(ev->data_ptr, ev->data_len);
            } else if (ev->op_code == 0x02) {
                /* Opus from the server. Counted only, until the playback path
                 * is wired up -- decoding here would be on the websocket
                 * task's stack, which is nowhere near the 26 KB Opus wants. */
                s_audio_frames++;
                if ((s_audio_frames % 25u) == 0) {
                    ESP_LOGI(TAG, "%" PRIu32 " audio frames received",
                             s_audio_frames);
                }
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "socket closed");
            s_state = MPX_VOICE_IDLE;
            s_session_id[0] = '\0';
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "socket error");
            break;

        default:
            break;
    }
}

/* ── connect ─────────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_connect(const char *url, const char *token)
{
    char headers[VOICE_HEADER_MAX];
    uint8_t mac[6] = {0};

    /* Fall back to whatever `voice provision` discovered, which is the normal
     * case -- the cloud tells the device where to connect, so a hand-typed
     * URL is the exception. */
    if (!url || !url[0]) {
        url = mpx_voice_stored_url();
    }
    if (!token || !token[0]) {
        token = mpx_voice_stored_token();
    }
    ESP_RETURN_ON_FALSE(url && url[0], ESP_ERR_INVALID_ARG, TAG,
                        "no url -- run `voice provision` first");
    if (s_client) {
        mpx_voice_disconnect();
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Device-Id is the MAC and Client-Id identifies this installation. The
     * xiaozhi server uses Device-Id to tell devices apart, so it has to be
     * stable -- deriving both from the MAC gives that for free. */
    const int written = snprintf(headers, sizeof(headers),
                                 "Authorization: Bearer %s\r\n"
                                 "Protocol-Version: 1\r\n"
                                 "Device-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                                 "Client-Id: %02x%02x%02x%02x%02x%02x\r\n",
                                 (token && token[0]) ? token : "none",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(headers),
                        ESP_ERR_INVALID_SIZE, TAG, "token too long for headers");

    if (!s_handshake) {
        s_handshake = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_handshake, ESP_ERR_NO_MEM, TAG, "out of memory");
    }

    const esp_websocket_client_config_t cfg = {
        .uri = url,
        .headers = headers,
        .task_stack = VOICE_WS_TASK_STACK,
        .buffer_size = 2048,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        /* Manual reconnect. An automatic one re-runs the handshake behind the
         * caller's back, and during bring-up a connection that silently comes
         * back hides the reason it dropped. */
        .disable_auto_reconnect = true,
    };

    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "client init failed");

    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                      voice_ws_event, NULL),
                        TAG, "cannot register events");

    s_state = MPX_VOICE_CONNECTING;
    s_socket_opened = false;
    ESP_LOGI(TAG, "connecting to %s", url);

    /* Internal RAM before and after, because the websocket client takes its
     * buffers from it and this firmware has very little to give. When the
     * panel starts logging ESP_ERR_NO_MEM the moment a socket opens, these
     * two numbers are the evidence. */
    ESP_LOGI(TAG, "internal RAM before connect: %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        mpx_voice_disconnect();
        return err;
    }

    /* Wait for the server's hello rather than returning on socket-open. A
     * socket that opens and then says nothing is the common failure -- wrong
     * path, wrong token, wrong protocol version -- and reporting success on
     * TCP alone would hide every one of them. */
    if (xSemaphoreTake(s_handshake, pdMS_TO_TICKS(VOICE_HANDSHAKE_MS)) != pdTRUE) {
        if (!s_socket_opened) {
            /* The WebSocket upgrade never completed. If the log above shows
             * "Connection reset by peer" and "Error read response for Upgrade
             * header", TCP reached the server and it hung up without even
             * sending an HTTP response -- which is what a server does when
             * the PATH is not one it serves. A firewall or wrong address
             * fails earlier and differently, with a refusal or a timeout. */
            ESP_LOGE(TAG, "the server closed the connection during the "
                          "WebSocket upgrade");
            ESP_LOGE(TAG, "the address and port are reachable, so the PATH is "
                          "the likely problem -- check what the server prints "
                          "at startup, it names the websocket URL it serves");
        } else {
            ESP_LOGE(TAG, "socket opened but no hello within %d ms -- the token "
                          "or the protocol version is the likely problem",
                     VOICE_HANDSHAKE_MS);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void mpx_voice_disconnect(void)
{
    if (!s_client) {
        return;
    }
    esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_state = MPX_VOICE_IDLE;
    s_session_id[0] = '\0';
    ESP_LOGI(TAG, "disconnected");
}

mpx_voice_state_t mpx_voice_state(void) { return s_state; }
const char *mpx_voice_session_id(void) { return s_session_id; }
uint32_t mpx_voice_downlink_rate(void) { return s_downlink_rate; }

const char *mpx_voice_state_name(void)
{
    switch (s_state) {
        case MPX_VOICE_IDLE:        return "idle";
        case MPX_VOICE_CONNECTING:  return "connecting";
        case MPX_VOICE_HANDSHAKING: return "handshaking";
        case MPX_VOICE_READY:       return "ready";
        default:                    return "?";
    }
}
