/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "claw_version.h"
#include "claw_paths.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_netif.h"
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#if CONFIG_MP4_ROBOT_ENABLE
#include "mpx_robot.h"
#include "mpx_wasm.h"
#include "mpx_rings.h"
#include "app_capabilities.h"
#include "cap_robot.h"
#include "cap_mpx_skill.h"
#include "mpx_selftest.h"
/* cap_display is built for every MP4_ROBOT_ENABLE configuration, panel or no
 * panel -- see main/idf_component.yml. Do NOT put this include behind
 * CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT: the face calls below are guarded
 * by MP4_ROBOT_ENABLE alone, and a narrower guard on the declaration is how
 * you get an implicit-declaration error on a display-less board.
 * cap_display_face_start() already treats "no panel" as success. */
#include "cap_display.h"
#endif

#define APP_ENABLE_MEM_LOG        (0)

/* The robot's own access point. Declared up here rather than beside
 * main_apply_ap_ip() because main_get_wifi_status(), which reports this
 * address, is defined earlier in the file. */
#define MP4_AP_IP_ADDR            "192.168.2.1"

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;

/* What the Wi-Fi endpoints under /v1/wifi/ still need after boot.
 *
 * s_config is freed at the end of app_main -- deliberately, because the full
 * app_config_t is about 6.5 KB and holds the LLM API key, the Telegram bot
 * token and every other secret. Keeping it resident just so the Wi-Fi
 * endpoints can read four fields would leave all of that in RAM for the life
 * of the device, which is a poor trade for a struct that is written once at
 * boot.
 *
 * So the Wi-Fi services keep their own copy of exactly what they need and
 * nothing else. The STA password is deliberately NOT here: /v1/wifi/connect
 * receives it as an argument, and no other caller needs it. Anything that has
 * to change what is stored loads a fresh app_config_t from NVS, edits it,
 * saves it and frees it -- see main_wifi_store_credentials(). */
typedef struct {
    char sta_ssid[APP_CONFIG_STR_LEN];
    char ap_ssid[APP_CONFIG_STR_LEN];
    char ap_password[APP_CONFIG_STR_LEN];
    char ap_behavior[16];
} main_wifi_state_t;

static main_wifi_state_t s_wifi;

static void main_wifi_state_capture(const app_config_t *config)
{
    if (!config) {
        return;
    }
    strlcpy(s_wifi.sta_ssid, config->wifi_ssid, sizeof(s_wifi.sta_ssid));
    strlcpy(s_wifi.ap_ssid, config->ap_ssid, sizeof(s_wifi.ap_ssid));
    strlcpy(s_wifi.ap_password, config->ap_password, sizeof(s_wifi.ap_password));
    strlcpy(s_wifi.ap_behavior, config->ap_behavior, sizeof(s_wifi.ap_behavior));
}

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

/* ── Network status, off the event task ────────────────────────────────────
 *
 * wifi_manager invokes the state callback from IDF's system event task
 * ("sys_evt"), whose stack is CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE -- 2304
 * bytes by default, sized for IDF's own handlers and nothing more.
 *
 * Doing the work there overflowed it:
 *
 *   ***ERROR*** A stack overflow in task sys_evt has been detected.
 *
 * Three things add up. The log line goes through the capture hook in
 * mpx_util/log_ring.cc, which formats into its own buffer before chaining to
 * the UART sink. app_claw_set_network_status() updates system_ui. And
 * cap_display_face_set_network() takes the display lock and writes an LVGL
 * label -- LVGL is the expensive one, and on a board with no panel it also
 * logs an error from inside the lock, which re-enters the same capture hook.
 *
 * Raising the event task stack alone would be treating the symptom. Blocking
 * sys_evt is the real problem: every Wi-Fi and IP event in the system queues
 * behind it, and on the MP4 board this contends for the display lock with the
 * face's own animation timer. So the callback now does the cheap part only --
 * read the status, copy the strings it needs, post -- and a worker task does
 * the rest.
 *
 * The queue holds one item and is written with xQueueOverwrite, so the
 * callback never blocks and never fails. Coalescing two transitions into the
 * latest one is correct here: this drives a status line, not a state machine. */
typedef struct {
    bool connected;
    bool ap_active;
    char mode[16];
    char ap_ssid[33];
    char sta_ip[16];
} main_net_status_t;

static QueueHandle_t s_net_status_queue;

static void main_apply_net_status(const main_net_status_t *st)
{
    const char *ap_ssid = st->ap_active && st->ap_ssid[0] ? st->ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             st->connected, st->ap_active,
             st->mode[0] ? st->mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(st->connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network UI: %s", esp_err_to_name(err));
    }

#if CONFIG_MP4_ROBOT_ENABLE
    /* The face carries this line itself: system_ui's status bar is on a home
     * screen this board cannot show. */
    cap_display_face_set_network(st->connected, ap_ssid, st->sta_ip);
#endif
}

static void main_net_status_task(void *arg)
{
    (void)arg;
    main_net_status_t st;

    for (;;) {
        if (xQueueReceive(s_net_status_queue, &st, portMAX_DELAY) == pdTRUE) {
            main_apply_net_status(&st);
        }
    }
}

static esp_err_t main_net_status_start(void)
{
    s_net_status_queue = xQueueCreate(1, sizeof(main_net_status_t));
    ESP_RETURN_ON_FALSE(s_net_status_queue, ESP_ERR_NO_MEM, TAG,
                        "Failed to create network status queue");

    /* 4 KB because LVGL is on the far end of this. Low priority: nothing
     * waits on it, and it must never preempt the gait task. */
    BaseType_t ok = xTaskCreate(main_net_status_task, "net_status", 4096,
                                NULL, 3, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_net_status_queue);
        s_net_status_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);

    main_net_status_t st = {0};
    st.connected = connected;
    st.ap_active = status.ap_active;
    if (status.mode) {
        strlcpy(st.mode, status.mode, sizeof(st.mode));
    }
    if (status.ap_active && status.ap_ssid) {
        strlcpy(st.ap_ssid, status.ap_ssid, sizeof(st.ap_ssid));
    }
    if (status.sta_ip) {
        strlcpy(st.sta_ip, status.sta_ip, sizeof(st.sta_ip));
    }

    if (s_net_status_queue) {
        xQueueOverwrite(s_net_status_queue, &st);
    } else {
        /* Before the worker exists -- the first callback can arrive from
         * inside wifi_manager_start(). That one runs on the caller's stack,
         * which is app_main's, and has room. */
        main_apply_net_status(&st);
    }
}

/* One line saying how much room is left, and -- more usefully -- the largest
 * single block, because an allocation fails on contiguous space long before
 * the total runs out. Called at the points where the boot sequence takes its
 * big bites, so an ESP_ERR_NO_MEM has a number next to it instead of being a
 * guess. Cheap enough to leave in unconditionally. */
static void main_log_heap(const char *stage)
{
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (psram_free) {
        ESP_LOGI(TAG, "heap [%s]: internal %u free / %u largest, PSRAM %u free",
                 stage, (unsigned)internal_free, (unsigned)internal_block,
                 (unsigned)psram_free);
    } else {
        ESP_LOGI(TAG, "heap [%s]: internal %u free / %u largest, no PSRAM",
                 stage, (unsigned)internal_free, (unsigned)internal_block);
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate Claw config for runtime update");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update running Claw config: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void main_copy_claw_to_app_config(const app_claw_config_t *src, app_config_t *dst)
{
    strlcpy(dst->llm_api_key, src->llm_api_key, sizeof(dst->llm_api_key));
    strlcpy(dst->llm_backend_type, src->llm_backend_type, sizeof(dst->llm_backend_type));
    strlcpy(dst->llm_model, src->llm_model, sizeof(dst->llm_model));
    strlcpy(dst->llm_base_url, src->llm_base_url, sizeof(dst->llm_base_url));
    strlcpy(dst->llm_auth_type, src->llm_auth_type, sizeof(dst->llm_auth_type));
    strlcpy(dst->llm_timeout_ms, src->llm_timeout_ms, sizeof(dst->llm_timeout_ms));
    strlcpy(dst->llm_max_tokens, src->llm_max_tokens, sizeof(dst->llm_max_tokens));
    strlcpy(dst->llm_default_image_max_bytes,
            src->llm_default_image_max_bytes,
            sizeof(dst->llm_default_image_max_bytes));
    strlcpy(dst->llm_max_tokens_field, src->llm_max_tokens_field, sizeof(dst->llm_max_tokens_field));
    strlcpy(dst->llm_supports_tools, src->llm_supports_tools, sizeof(dst->llm_supports_tools));
    strlcpy(dst->llm_supports_vision, src->llm_supports_vision, sizeof(dst->llm_supports_vision));
    strlcpy(dst->llm_image_remote_url_only,
            src->llm_image_remote_url_only,
            sizeof(dst->llm_image_remote_url_only));
}

static esp_err_t main_save_claw_config(const app_claw_config_t *config, void *user_ctx)
{
    esp_err_t err;
    app_config_t *app_config = NULL;

    (void)user_ctx;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    app_config = calloc(1, sizeof(*app_config));
    ESP_RETURN_ON_FALSE(app_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate app config for Claw save");

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for Claw save: %s", esp_err_to_name(err));
        free(app_config);
        return err;
    }
    main_copy_claw_to_app_config(config, app_config);
    err = app_config_save(app_config);
    free(app_config);
    return err;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    /* Not wifi_status.ap_ip: wifi_manager snapshots that string when the AP
     * comes up and never refreshes it, so it still holds the address the
     * interface had before main_apply_ap_ip() renumbered it. Everything that
     * tells a user where to point -- /v1/wifi/status, the captive portal
     * redirect -- reads it through here, so this is the one place to correct. */
    status->ap_ip = wifi_status.ap_active ? MP4_AP_IP_ADDR : wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    status->sta_configured = wifi_status.sta_configured;
    /* The SSID comes from the saved config rather than the radio, which does
     * not expose it -- but only while the radio is actually carrying it.
     * Reporting a name that /v1/wifi/disconnect just took off the air would
     * have the setup screen showing a network the robot is not on. */
    status->sta_ssid = (wifi_status.sta_configured || wifi_status.sta_connected)
                       ? s_wifi.sta_ssid : "";
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

/* ── The robot's own access point ──────────────────────────────────────────
 *
 * ESP-Claw's wifi_manager leaves the AP on the ESP-IDF default, 192.168.4.1.
 * The MPX toolchain does not: mpx-cli defaults to 192.168.2.1, its
 * connection-failure hint names that address, and so does every note anyone
 * has written down about these robots. Moving the firmware is a dozen lines;
 * moving the toolchain means a per-machine .env on every clone forever, and a
 * built-in error hint that confidently tells you the wrong address.
 */
static esp_err_t main_apply_ap_ip(void)
{
    esp_netif_t *ap = wifi_manager_get_ap_netif();
    esp_netif_ip_info_t ip = {0};

    ESP_RETURN_ON_FALSE(ap, ESP_ERR_INVALID_STATE, TAG, "no AP netif");

    ip.ip.addr      = esp_ip4addr_aton(MP4_AP_IP_ADDR);
    ip.gw.addr      = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    /* The DHCP server has to be down to renumber the interface, and it hands
     * out the gateway from this same ip_info once it is back up. Do this
     * before captive_dns_start(), which writes the DNS option into the very
     * DHCP configuration a restart here would otherwise discard. */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap), TAG, "AP dhcps stop failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap, &ip), TAG, "AP set ip failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap), TAG, "AP dhcps start failed");

    ESP_LOGI(TAG, "AP address set to %s", MP4_AP_IP_ADDR);
    return ESP_OK;
}

/* ── Live Wi-Fi control for the PWA's setup screen ─────────────────────────
 *
 * ESP-Claw's /api/config saves credentials and asks for a reboot. The PWA does
 * not reboot: the phone running it is normally joined to the robot's own AP,
 * so a restart mid-setup drops it off the network and leaves the user on a
 * page that cannot reload. These three apply the change to the radio as well,
 * and the setup screen polls /v1/wifi/status for the outcome.
 *
 * Credentials still travel through main_save_config, so validation and the
 * running Claw config see them exactly as they would from the settings page.
 */
static esp_err_t main_wifi_apply(const char *sta_ssid, const char *sta_password)
{
    /* The AP half is passed every time because apply_sta_config replaces the
     * whole configuration: omitting it would silently rename the robot's own
     * access point back to the default while joining a network. */
    return wifi_manager_apply_sta_config(&(wifi_manager_config_t) {
        .sta_ssid = sta_ssid,
        .sta_password = sta_password,
        .ap_ssid = s_wifi.ap_ssid[0] ? s_wifi.ap_ssid : NULL,
        .ap_password = s_wifi.ap_password[0] ? s_wifi.ap_password : NULL,
        .ap_behavior = s_wifi.ap_behavior,
    });
}

/* Persist a change to the stored STA credentials.
 *
 * Loads the whole config from NVS, edits the two fields, saves, frees. The
 * temporary is about 6.5 KB, which is why it is heap rather than stack -- the
 * httpd task's stack is 8 KB and this runs on it. Passing NULL for ssid
 * clears both fields, which is what /v1/wifi/forget wants. */
static esp_err_t main_wifi_store_credentials(const char *ssid, const char *password)
{
    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "Out of memory for config");

    esp_err_t err = app_config_load(config);
    if (err == ESP_OK) {
        strlcpy(config->wifi_ssid, ssid ? ssid : "", sizeof(config->wifi_ssid));
        strlcpy(config->wifi_password, password ? password : "",
                sizeof(config->wifi_password));
        err = main_save_config(config);
        if (err == ESP_OK) {
            strlcpy(s_wifi.sta_ssid, config->wifi_ssid, sizeof(s_wifi.sta_ssid));
        }
    }

    free(config);
    return err;
}

static esp_err_t main_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    ESP_RETURN_ON_ERROR(main_wifi_store_credentials(ssid, password), TAG,
                        "Failed to save Wi-Fi credentials");

    ESP_LOGI(TAG, "Joining Wi-Fi network %s", ssid);
    return main_wifi_apply(ssid, password);
}

/* Drop the station link and stay dropped.
 *
 * esp_wifi_disconnect() on its own would not do it: wifi_manager arms a
 * reconnect timer from the disconnect event, so the robot would rejoin within
 * seconds and the button would look broken. Reapplying with no station SSID is
 * what actually takes the radio back to AP-only. NVS is untouched, so the next
 * boot -- or the next /v1/wifi/connect -- reconnects. */
static esp_err_t main_wifi_disconnect(void)
{
    ESP_LOGI(TAG, "Dropping Wi-Fi station link (credentials kept)");
    return main_wifi_apply(NULL, NULL);
}

static esp_err_t main_wifi_forget(void)
{
    ESP_LOGW(TAG, "Forgetting Wi-Fi credentials for %s",
             s_wifi.sta_ssid[0] ? s_wifi.sta_ssid : "(none)");

    ESP_RETURN_ON_ERROR(main_wifi_store_credentials(NULL, NULL), TAG,
                        "Failed to clear Wi-Fi credentials");

    return main_wifi_apply(NULL, NULL);
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif


#if CONFIG_MP4_ROBOT_ENABLE
/* ── Robot capability groups ──────────────────────────────────────────────
 *
 * ESP-Claw takes application-provided capability groups through a registry
 * that app_capabilities_init() merges with its own compiled table, so none of
 * this requires editing anything inside the submodule. Registration has to
 * happen BEFORE app_claw_start(), which is where that merge runs.
 *
 * The register_fn signature carries the config and storage paths because some
 * built-in groups need them (cap_lua wants the script root, the IM groups want
 * credentials). These three do not: everything they touch is already
 * initialised by the time app_claw_start() is reached. */
static esp_err_t app_register_cap_robot(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_robot_register_group();
}

static esp_err_t app_register_cap_mpx_skill(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_mpx_skill_register_group();
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
static esp_err_t app_register_cap_display(const app_claw_config_t *config,
                                          const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_display_register_group();
}
#endif

static void app_register_robot_capabilities(void)
{
    static const app_capability_external_group_t groups[] = {
        {
            .group_id = "cap_robot",
            .display_name = "Robot",
            .llm_visible_by_default = true,
            .reg = app_register_cap_robot,
        },
        {
            .group_id = "cap_mpx_skill",
            .display_name = "Robot skills",
            .llm_visible_by_default = true,
            .reg = app_register_cap_mpx_skill,
        },
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
        {
            .group_id = "cap_display",
            .display_name = "Robot display",
            .llm_visible_by_default = true,
            .reg = app_register_cap_display,
        },
#endif
    };

    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        const esp_err_t err = app_capabilities_register_external_group(&groups[i]);
        if (err != ESP_OK) {
            /* Not fatal. A device that cannot walk is still worth booting into
             * the web UI so it can be told why. */
            ESP_LOGE(TAG, "Could not register %s: %s",
                     groups[i].group_id, esp_err_to_name(err));
        }
    }
}
#endif  /* CONFIG_MP4_ROBOT_ENABLE */

void app_main(void)
{
#if CONFIG_MP4_ROBOT_ENABLE
    /* First, before anything logs. The ring installs a vprintf hook that
     * chains to the existing sink, so serial output is unchanged -- but
     * anything logged before this point is not in it, and the boot sequence
     * is exactly the part you want when a device will not come up. */
    mpx_log_ring_init();
#endif

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "ESP-Claw version: %s", claw_get_version());
    ESP_LOGI(TAG, "ESP-Claw git version: %s", claw_get_git_version());
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error

    /* Take the Wi-Fi fields now, while the config is still in memory. Every
     * request to the endpoints under /v1/wifi/ reads them from here after
     * boot, because s_config itself is freed at the end of app_main. */
    main_wifi_state_capture(s_config);

    /* Deliberately not ESP_ERROR_CHECK'd.
     *
     * This initialises every chip the selected board file declares. On the
     * MP4 that is the ST7789, the ES7210 and the BMI270; if one of them does
     * not answer -- an unseated FPC, a board revision with a different part --
     * ESP_ERROR_CHECK aborts inside app_main, the panic handler reboots, and
     * it does it again on the next boot. The visible symptom is a silent boot
     * loop with no way in, which is exactly the situation you most need a
     * console and a web UI for.
     *
     * Everything downstream already treats a missing device as a normal
     * configuration: cap_display returns ESP_ERR_NOT_SUPPORTED with no panel,
     * mpx_robot_init() and mpx_wasm_init() warn and continue, and `selftest`
     * reports SKIP. So log loudly and carry on -- a half-initialised board
     * that boots into the agent can be diagnosed; one that reboots cannot. */
    {
        const esp_err_t bm_err = esp_board_manager_init();
        if (bm_err != ESP_OK) {
            ESP_LOGE(TAG, "Board '%s' did not initialise: %s",
                     CONFIG_ESP_BOARD_NAME, esp_err_to_name(bm_err));
            ESP_LOGE(TAG, "Continuing without board peripherals. Run `selftest`"
                          " on the console to see which ones are missing.");
        }
    }
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

#if CONFIG_MP4_ROBOT_ENABLE
    /* Robot HAL: SPI3, the four AT32F413 driver boards, the BMI270, the
     * persisted servo offsets, and the gait task on core 1.
     *
     * Here rather than later because it needs NVS and the board manager and
     * nothing else, and because a robot that cannot stand is worth knowing
     * about before we spend thirty seconds waiting for Wi-Fi. Deliberately
     * NOT fatal: a board with no servo harness attached should still boot
     * into the agent and the web UI so it can be diagnosed.
     *
     * The WASM skill runtime attaches to this later, after app_claw_start(),
     * so that a misbehaving autorun skill cannot keep the web UI from
     * coming up. That ordering is the difference between a bad skill and a
     * brick, and it is why the two are not initialised together. */
    if (!mpx_robot_init()) {
        ESP_LOGW(TAG, "Robot init failed - continuing without servos");
    }

    /* WAMR, the 74 host functions, and the hook table mpx_robot asks through.
     * Needs mpx_robot up first. Not fatal either: a device that cannot run
     * skills is still a usable robot. */
    if (!mpx_wasm_init()) {
        ESP_LOGW(TAG, "WASM runtime init failed - skills unavailable");
    }

#endif


    main_log_heap("boot");
    ESP_ERROR_CHECK(wifi_manager_init());

    /* Also non-fatal, for the same reason: on a board whose panel failed to
     * initialise above, this is where a display-less build would otherwise
     * abort. Without a UI the agent still runs and the web UI still serves. */
    {
        const esp_err_t ui_err = app_claw_ui_start();
        if (ui_err != ESP_OK) {
            ESP_LOGE(TAG, "UI did not start: %s -- continuing headless",
                     esp_err_to_name(ui_err));
        }
    }

#if CONFIG_MP4_ROBOT_ENABLE
    /* Straight after the display service is up, so the eyes are the first
     * thing on the panel -- before Wi-Fi, before the agent. On a robot that
     * takes a few seconds to find a network, a face that is already blinking
     * is the difference between "booting" and "broken". */
    {
        const esp_err_t face_err = cap_display_face_start();
        if (face_err != ESP_OK) {
            ESP_LOGW(TAG, "Face not started: %s", esp_err_to_name(face_err));
        }
    }
#endif

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
            .wifi_connect = main_wifi_connect,
            .wifi_disconnect = main_wifi_disconnect,
            .wifi_forget = main_wifi_forget,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    /* Before the callback is registered, so no event can find a half-built
     * worker. */
    if (main_net_status_start() != ESP_OK) {
        ESP_LOGW(TAG, "Network status worker unavailable; updates run inline");
    }
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    log_wifi_startup_config(s_config);

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        esp_err_t ap_ip_err = main_apply_ap_ip();
        if (ap_ip_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not move the AP to %s: %s -- mpx-cli will need "
                          "MPX_HOST set", MP4_AP_IP_ADDR, esp_err_to_name(ap_ip_err));
        }
        /* Non-fatal, like the board and UI init above. A route that fails to
         * register costs one endpoint; aborting here costs the console, the
         * agent and any chance of reading why. The error names the route. */
        const esp_err_t http_err = http_server_start();
        if (http_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server did not start: %s -- web UI unavailable, "
                          "console still works", esp_err_to_name(http_err));
        }
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            esp_err_t wait_err = wifi_manager_wait_connected(30000);
            if (wait_err == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else if (wait_err == ESP_ERR_TIMEOUT) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA not connected within wait window; retrying in background: mode=%s ap_active=%d ap_ip=%s",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.ap_ip ? status.ap_ip : "0.0.0.0");
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_set_save_config_callback(main_save_claw_config, NULL));
#if CONFIG_MP4_ROBOT_ENABLE
    /* Must precede app_claw_start(): that is where the external groups are
     * merged with the built-in table and the LLM-visible set is computed. */
    app_register_robot_capabilities();
#endif

    main_log_heap("before agent");

    /* Non-fatal, like every other subsystem above.
     *
     * ESP_ERR_NO_MEM here is the expected outcome on a devkit with no PSRAM:
     * the agent, the scheduler, the event router and the Lua VM all expect
     * external RAM to spill into. Aborting turns that into a boot loop with
     * no console; continuing leaves Wi-Fi, the web server and the console
     * commands up, which is enough to read the heap numbers above and decide
     * what to turn off. */
    const esp_err_t claw_err = app_claw_start(s_claw_config);
    if (claw_err != ESP_OK) {
        ESP_LOGE(TAG, "Agent did not start: %s", esp_err_to_name(claw_err));
        if (claw_err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Out of internal RAM. This build expects PSRAM. "
                          "Enable CONFIG_SPIRAM for your module, or set "
                          "MP4_ROBOT_ENABLE=n to drop the robot stack.");
        }
    } else {
        main_log_heap("after agent");
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

#if CONFIG_MP4_ROBOT_ENABLE
    /* Scan the skills directory, start the IMU event watcher, and run the
     * autorun skill -- deliberately the last thing that happens.
     *
     * An autorun skill that crashes the robot runs again on the next boot,
     * and the next. A user with no serial cable then has a brick, and the
     * thing that bricked it arrived from a marketplace. By this point the
     * web server is listening and the agent is up, so they can always
     * uninstall it. That is the difference between a bad skill and a brick,
     * and it is the whole reason this call is here and not next to
     * mpx_wasm_init(). */
    mpx_wasm_start_skills();
#endif

    register_wifi_command();
#if CONFIG_MP4_ROBOT_ENABLE
    register_selftest_command();
#endif

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    /* s_config and s_claw_config are boot-time scaffolding: about 6.5 KB of
     * strings, most of them secrets, that nothing needs once the subsystems
     * they configured are running.
     *
     * Anything reached from an HTTP handler or a console command runs AFTER
     * this point, so it must not hold a pointer into either of them. That is
     * exactly how /v1/wifi/connect came to fail with "config not loaded" on
     * every request: the endpoints worked from s_config, which by then was
     * NULL. See main_wifi_state_capture() for the pattern to follow -- copy
     * the few fields you need before the free, or reload from NVS on demand. */
    app_free_runtime_state();
}
