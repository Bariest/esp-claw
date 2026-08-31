/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * /v1/robot/* -- the movement and calibration half of the API the MPX-Dog PWA
 * speaks. Ported from main/network/http_server.cc.
 *
 * The response shapes are a contract, not a preference: pwa-redesign reads
 * these exact field names. `data.config` seeds the home-screen sliders,
 * `offsets[]` drives the calibration screen. Renaming a key here breaks a
 * screen over there with no compile error in between.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_server_priv.h"
#include "mpx_robot.h"
#include "mpx_wasm.h"

static const char *TAG = "http_mpx_robot";

/* ── POST /v1/robot/gait ───────────────────────────────────────────────── */

static esp_err_t robot_gait_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    char mode[48] = {0};

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    http_server_json_read_string(body, "mode", mode, sizeof(mode));
    cJSON_Delete(body);

    if (mode[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode");
    }

    /* Through the movement resolver, not the gait table: a name provided by an
     * installed skill has to work here too. Built-ins still win. */
    const mpx_movement_result_t r = mpx_wasm_movement_run(mode, false);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    http_server_json_add_string(resp, "mode", mode);

    if (r == MPX_MOVEMENT_STARTED) {
        cJSON_AddBoolToObject(resp, "ok", true);
        return http_server_send_json_response(req, resp);
    }

    cJSON_AddBoolToObject(resp, "ok", false);
    http_server_json_add_string(resp, "error", mpx_wasm_movement_result_text(r));

    /* The PWA branches on the status code: 404 means "no such movement, tell
     * the user", 409 means "busy, offer to stop the skill". Collapsing both to
     * 400 would make a correct answer look like a broken robot. */
    httpd_resp_set_status(req, (r == MPX_MOVEMENT_UNKNOWN) ? "404 Not Found" : "409 Conflict");
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/robot/joy ────────────────────────────────────────────────── */

static double json_num(const cJSON *root, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static esp_err_t robot_joy_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    float f, s, t;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    f = (float)json_num(body, "f", 0.0);
    s = (float)json_num(body, "s", 0.0);
    t = (float)json_num(body, "t", 0.0);
    cJSON_Delete(body);

    mpx_robot_drive(f, s, t);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/robot/status ──────────────────────────────────────────────── */

static void add_config_object(cJSON *parent, const mpx_robot_config_t *cfg)
{
    cJSON *c = cJSON_CreateObject();
    if (!c) {
        return;
    }
    cJSON_AddNumberToObject(c, "period",    cfg->period);
    cJSON_AddNumberToObject(c, "height",    cfg->height);
    cJSON_AddNumberToObject(c, "up_height", cfg->up_height);
    cJSON_AddNumberToObject(c, "stride",    cfg->stride);
    cJSON_AddNumberToObject(c, "tilt",      cfg->tilt);
    cJSON_AddNumberToObject(c, "sg_speed",  cfg->sg_speed);
    cJSON_AddItemToObject(parent, "config", c);
}

static esp_err_t robot_status_handler(httpd_req_t *req)
{
    mpx_robot_config_t cfg;
    cJSON *resp = cJSON_CreateObject();
    cJSON *offsets = NULL;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    mpx_robot_get_config(&cfg);
    http_server_json_add_string(resp, "mode", mpx_robot_current_gait_name());
    add_config_object(resp, &cfg);

    offsets = cJSON_AddArrayToObject(resp, "offsets");
    if (offsets) {
        for (int id = 1; id <= 12; ++id) {
            cJSON_AddItemToArray(offsets, cJSON_CreateNumber(mpx_robot_get_offset(id)));
        }
    }
    return http_server_send_json_response(req, resp);
}

/* ── GET / POST /v1/robot/config ───────────────────────────────────────── */

static esp_err_t robot_config_get_handler(httpd_req_t *req)
{
    mpx_robot_config_t cfg;
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mpx_robot_get_config(&cfg);
    cJSON_AddNumberToObject(resp, "period",    cfg.period);
    cJSON_AddNumberToObject(resp, "height",    cfg.height);
    cJSON_AddNumberToObject(resp, "up_height", cfg.up_height);
    cJSON_AddNumberToObject(resp, "stride",    cfg.stride);
    cJSON_AddNumberToObject(resp, "tilt",      cfg.tilt);
    cJSON_AddNumberToObject(resp, "sg_speed",  cfg.sg_speed);
    return http_server_send_json_response(req, resp);
}

static esp_err_t robot_config_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    mpx_robot_config_t cfg;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    /* Read-modify-write. The PWA's sliders post only what moved, and `tilt`
     * legitimately goes negative -- so "absent" cannot be encoded as -1 the
     * way the original firmware did it. */
    mpx_robot_get_config(&cfg);
    cfg.period    = (int)json_num(body, "period",    cfg.period);
    cfg.height    = (int)json_num(body, "height",    cfg.height);
    cfg.up_height = (int)json_num(body, "up_height", cfg.up_height);
    cfg.stride    = (int)json_num(body, "stride",    cfg.stride);
    cfg.tilt      = (int)json_num(body, "tilt",      cfg.tilt);
    cfg.sg_speed  = (int)json_num(body, "sg_speed",  cfg.sg_speed);
    cJSON_Delete(body);

    mpx_robot_set_config(&cfg);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── Calibration ───────────────────────────────────────────────────────── */

static esp_err_t robot_calibrate_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    int servo;
    float offset;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    servo  = (int)json_num(body, "servo", 0);
    offset = (float)json_num(body, "offset", 0.0);
    cJSON_Delete(body);

    if (servo < 1 || servo > 12) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "servo must be 1-12");
    }

    mpx_robot_set_offset(servo, offset);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "servo", servo);
    cJSON_AddNumberToObject(resp, "offset", offset);
    return http_server_send_json_response(req, resp);
}

static esp_err_t robot_calibrate_reset_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    mpx_robot_reset_offsets();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    return http_server_send_json_response(req, resp);
}

/* ── POST /v1/robot/diagnostic/ping ────────────────────────────────────── */

static esp_err_t robot_ping_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    cJSON *resp = NULL;
    int servo, result;

    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    servo = (int)json_num(body, "servo", 1);
    cJSON_Delete(body);

    if (servo < 1 || servo > 12) {
        servo = 1;
    }
    result = mpx_robot_ping_servo(servo);

    resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", result > 0);
    cJSON_AddNumberToObject(resp, "servo", servo);
    cJSON_AddNumberToObject(resp, result > 0 ? "model" : "error", result);
    return http_server_send_json_response(req, resp);
}

/* ── GET /v1/robot/movements ───────────────────────────────────────────── */

static esp_err_t robot_movements_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = NULL;

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    arr = cJSON_AddArrayToObject(resp, "movements");
    if (!arr) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    for (int i = 0; i < mpx_robot_gait_name_count(); ++i) {
        const char *name = mpx_robot_gait_name_at(i);
        cJSON *item = NULL;
        if (!name) {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", name);
        http_server_json_add_string(item, "source", "builtin");
        cJSON_AddItemToArray(arr, item);
    }

    /* Skill-provided movement names, merged in. This is the only endpoint that
     * reports both, which is why AddActionView should be using it rather than
     * browsing the filesystem. */
    for (int i = 0; i < mpx_wasm_skill_count(); ++i) {
        const char *file = NULL, *slug = NULL, *gait = NULL;
        bool behaviour = false;
        cJSON *item = NULL;
        if (!mpx_wasm_skill_at(i, &file, &slug, &gait, NULL, NULL, &behaviour)) {
            continue;
        }
        if (!gait || gait[0] == '\0') {
            continue;
        }
        item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", gait);
        http_server_json_add_string(item, "source", "skill");
        http_server_json_add_string(item, "skill", slug ? slug : "");
        cJSON_AddBoolToObject(item, "behaviour", behaviour);
        cJSON_AddItemToArray(arr, item);
    }

    return http_server_send_json_response(req, resp);
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_robot_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/v1/robot/gait",             .method = HTTP_POST, .handler = robot_gait_handler },
        { .uri = "/v1/robot/joy",              .method = HTTP_POST, .handler = robot_joy_handler },
        { .uri = "/v1/robot/status",           .method = HTTP_GET,  .handler = robot_status_handler },
        { .uri = "/v1/robot/config",           .method = HTTP_GET,  .handler = robot_config_get_handler },
        { .uri = "/v1/robot/config",           .method = HTTP_POST, .handler = robot_config_post_handler },
        { .uri = "/v1/robot/calibrate",        .method = HTTP_POST, .handler = robot_calibrate_handler },
        { .uri = "/v1/robot/calibrate/reset",  .method = HTTP_POST, .handler = robot_calibrate_reset_handler },
        { .uri = "/v1/robot/diagnostic/ping",  .method = HTTP_POST, .handler = robot_ping_handler },
        { .uri = "/v1/robot/movements",        .method = HTTP_GET,  .handler = robot_movements_handler },
    };

    ESP_LOGI(TAG, "registering %u robot routes",
             (unsigned)(sizeof(handlers) / sizeof(handlers[0])));
    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "robot");
}
