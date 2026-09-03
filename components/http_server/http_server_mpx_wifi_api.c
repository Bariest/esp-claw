/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/wifi routes -- what the PWA's WiFiSetup and APModeConfig screens use.
 *
 * ESP-Claw already has /api/config, which saves Wi-Fi credentials and tells you
 * to reboot. That is fine from a laptop on the same LAN and wrong here: the
 * phone doing the setup is usually joined to the robot's own AP, so a reboot
 * drops it off the network in the middle of the flow. WiFiSetup instead posts
 * credentials and polls /v1/wifi/status until the state turns "connected", so
 * these routes apply the change to the radio, not just to NVS.
 *
 * The radio work itself lives in main, behind the services table -- this file
 * only translates between JSON and those calls. That is the same seam
 * /api/config uses, and it keeps http_server free of a wifi_manager dependency.
 *
 * "Disconnect" here means drop the STA link now and stay dropped. It has to be
 * spelled that way because wifi_manager re-arms its reconnect timer on every
 * disconnect event, so esp_wifi_disconnect() alone would undo itself within
 * seconds. The credentials stay in NVS; only /v1/wifi/forget clears them.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_wifi";

#define MPX_WIFI_FIELD_MAX  128

static esp_err_t wifi_send_ok(httpd_req_t *req, const char *extra_key)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    if (extra_key) {
        cJSON_AddBoolToObject(resp, extra_key, true);
    }
    return http_server_send_json_response(req, resp);
}

static esp_err_t wifi_send_err(httpd_req_t *req, const char *message)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "error", message);
    return http_server_send_json_response(req, resp);
}

/* Pull "ssid" and "password" out of a POST body. Returns false and answers the
 * request itself when either the body or the SSID is unusable, so callers can
 * simply return on false. */
static bool wifi_read_credentials(httpd_req_t *req,
                                  char *ssid, size_t ssid_size,
                                  char *password, size_t password_size,
                                  esp_err_t *out_result)
{
    cJSON *body = NULL;

    ssid[0] = '\0';
    password[0] = '\0';

    if (http_server_parse_json_body(req, &body) != ESP_OK || !body) {
        *out_result = wifi_send_err(req, "invalid JSON body");
        return false;
    }
    http_server_json_read_string(body, "ssid", ssid, ssid_size);
    http_server_json_read_string(body, "password", password, password_size);
    cJSON_Delete(body);

    if (ssid[0] == '\0') {
        *out_result = wifi_send_err(req, "missing 'ssid' field");
        return false;
    }
    return true;
}

/* ── GET /v1/wifi/status ───────────────────────────────────────────────────
 *
 * {"ok":true,"ap":{"ssid","ip"},"sta":{"state","ssid","ip"}}
 *
 * state is one of connected / connecting / disconnected. The PWA polls this
 * every second or so during setup and renders straight off those three words,
 * so they are the contract -- not the underlying flags. */

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    cJSON *resp, *ap, *sta;
    const char *state;

    if (!ctx->services.get_wifi_status ||
            ctx->services.get_wifi_status(&status) != ESP_OK) {
        return wifi_send_err(req, "wifi status unavailable");
    }

    state = status.wifi_connected ? "connected"
            : (status.sta_configured ? "connecting" : "disconnected");

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);

    ap = cJSON_AddObjectToObject(resp, "ap");
    sta = cJSON_AddObjectToObject(resp, "sta");
    if (!ap || !sta) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    http_server_json_add_string(ap, "ssid", status.ap_active ? status.ap_ssid : "");
    http_server_json_add_string(ap, "ip", status.ap_active ? status.ap_ip : "");
    http_server_json_add_string(sta, "state", state);
    http_server_json_add_string(sta, "ssid", status.sta_ssid);
    http_server_json_add_string(sta, "ip", status.wifi_connected ? status.ip : "");
    http_server_json_add_string(sta, "error",
                                (status.wifi_connected || !status.sta_error)
                                ? "" : status.sta_error);

    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/wifi/connect ─────────────────────────────────────────────── */

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    char ssid[MPX_WIFI_FIELD_MAX];
    char password[MPX_WIFI_FIELD_MAX];
    esp_err_t result = ESP_OK;
    esp_err_t err;

    if (!ctx->services.wifi_connect) {
        return wifi_send_err(req, "wifi control unavailable");
    }
    if (!wifi_read_credentials(req, ssid, sizeof(ssid), password, sizeof(password), &result)) {
        return result;
    }

    ESP_LOGI(TAG, "POST /v1/wifi/connect  ssid=%s pass_len=%u",
             ssid, (unsigned)strlen(password));

    err = ctx->services.wifi_connect(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        return wifi_send_err(req, esp_err_to_name(err));
    }
    /* Answering as soon as the attempt starts, not when it succeeds: the join
     * can take 10 seconds and the PWA is already polling /v1/wifi/status for
     * the outcome. Waiting here would only risk the socket timing out first. */
    return wifi_send_ok(req, NULL);
}

/* ── POST /v1/wifi/disconnect ──────────────────────────────────────────── */

static esp_err_t wifi_disconnect_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err;

    if (!ctx->services.wifi_disconnect) {
        return wifi_send_err(req, "wifi control unavailable");
    }
    ESP_LOGI(TAG, "POST /v1/wifi/disconnect");

    err = ctx->services.wifi_disconnect();
    if (err != ESP_OK) {
        return wifi_send_err(req, esp_err_to_name(err));
    }
    return wifi_send_ok(req, NULL);
}

/* ── POST /v1/wifi/forget ──────────────────────────────────────────────── */

static esp_err_t wifi_forget_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err;

    if (!ctx->services.wifi_forget) {
        return wifi_send_err(req, "wifi control unavailable");
    }
    ESP_LOGW(TAG, "POST /v1/wifi/forget  clearing saved credentials");

    err = ctx->services.wifi_forget();
    if (err != ESP_OK) {
        return wifi_send_err(req, esp_err_to_name(err));
    }
    return wifi_send_ok(req, NULL);
}

/* ── POST /v1/wifi/ap-config ───────────────────────────────────────────────
 *
 * Renaming the robot's own access point. Unlike the STA routes this one does
 * reboot, because the AP the requesting phone is sitting on is the thing being
 * reconfigured -- there is no way to change it without dropping that client.
 * The response goes out first and the restart follows half a second later, so
 * APModeConfig gets to show its reconnect instructions. */

static esp_err_t wifi_ap_config_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    char ssid[MPX_WIFI_FIELD_MAX];
    char password[MPX_WIFI_FIELD_MAX];
    esp_err_t result = ESP_OK;
    app_config_t *config;
    esp_err_t err;

    if (!ctx->services.load_config || !ctx->services.save_config || !ctx->services.restart_device) {
        return wifi_send_err(req, "config service unavailable");
    }
    if (!wifi_read_credentials(req, ssid, sizeof(ssid), password, sizeof(password), &result)) {
        return result;
    }

    config = calloc(1, sizeof(*config));
    if (!config) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    err = ctx->services.load_config(config);
    if (err != ESP_OK) {
        free(config);
        return wifi_send_err(req, "failed to load config");
    }

    strlcpy(config->ap_ssid, ssid, sizeof(config->ap_ssid));
    strlcpy(config->ap_password, password, sizeof(config->ap_password));

    ESP_LOGI(TAG, "POST /v1/wifi/ap-config  ssid=%s pass_len=%u",
             ssid, (unsigned)strlen(password));

    err = ctx->services.save_config(config);
    free(config);
    if (err != ESP_OK) {
        /* app_config_validate_wifi rejects an AP password under 8 characters,
         * which is the mistake a user actually makes here. Say so rather than
         * reporting a bare error code. */
        return wifi_send_err(req, "rejected: AP password must be empty or at least 8 characters");
    }

    result = wifi_send_ok(req, "restarting");
    ESP_LOGW(TAG, "AP config saved -- restarting");
    ctx->services.restart_device();
    return result;
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_wifi_routes(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        { .uri = "/v1/wifi/status",     .method = HTTP_GET,  .handler = wifi_status_handler     },
        { .uri = "/v1/wifi/connect",    .method = HTTP_POST, .handler = wifi_connect_handler    },
        { .uri = "/v1/wifi/disconnect", .method = HTTP_POST, .handler = wifi_disconnect_handler },
        { .uri = "/v1/wifi/forget",     .method = HTTP_POST, .handler = wifi_forget_handler     },
        { .uri = "/v1/wifi/ap-config",  .method = HTTP_POST, .handler = wifi_ap_config_handler  },
    };

    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "mpx wifi");
}
