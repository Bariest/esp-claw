/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See include/mpx_pi_link.h for what this is and the line protocol.
 *
 * UART1 is used because UART0 is the console (the CH340K on GPIO 43/44) and
 * UART2 does not exist on the S3. The receive side is one task doing blocking
 * reads with a short timeout and assembling lines; everything else -- the
 * console commands, mpx_pi_link_ping() -- just writes to the UART and looks
 * at state the task updates. No event queue: at 115200 baud the 1 KB driver
 * ring buffer holds ~90 ms of traffic, and the task wakes every 20 ms.
 */

#include "mpx_pi_link.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "pi_link";

#define PI_UART            UART_NUM_1
#define PI_TX_GPIO         CONFIG_MP4_PI_LINK_TX_GPIO
#define PI_RX_GPIO         CONFIG_MP4_PI_LINK_RX_GPIO
#define PI_BAUD            CONFIG_MP4_PI_LINK_BAUD
#define PI_RX_BUF          1024
#define PI_TX_BUF          1024
#define PI_LINE_MAX        255          /* longest line accepted, sans newline */
#define PI_RX_POLL_MS      20
#define PI_TASK_STACK      3072
#define PI_TASK_PRIO       4
#define PI_OUR_NAME        "mp4-claw"
#define PI_PING_TIMEOUT_MS 1000

static bool               s_up        = false;
static SemaphoreHandle_t  s_tx_lock   = NULL;
static TaskHandle_t       s_rx_task   = NULL;
static mpx_pi_link_stats_t s_st;
static portMUX_TYPE       s_st_lock   = portMUX_INITIALIZER_UNLOCKED;

/* Ping bookkeeping: the sequence we last sent, the sequence last PONGed, and
 * the time the PONG arrived. The pinger spins on s_pong_seq.             */
static volatile uint32_t  s_ping_seq  = 0;
static volatile uint32_t  s_pong_seq  = 0;
static volatile int64_t   s_pong_us   = 0;

/* `pi watch` sets this so the receive task prints every line to the console
 * as it arrives, instead of only logging the ones it does not understand. */
static volatile bool      s_watch     = false;

/* ── sending ─────────────────────────────────────────────────────────────── */

esp_err_t mpx_pi_link_send_line(const char *line)
{
    if (!s_up || line == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t n = strlen(line);
    if (n > PI_LINE_MAX || strchr(line, '\n') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int w1 = uart_write_bytes(PI_UART, line, n);
    int w2 = uart_write_bytes(PI_UART, "\n", 1);
    xSemaphoreGive(s_tx_lock);
    if (w1 < 0 || w2 < 0) {
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_st_lock);
    s_st.bytes_tx += (uint32_t)(w1 + w2);
    portEXIT_CRITICAL(&s_st_lock);
    return ESP_OK;
}

int64_t mpx_pi_link_ping(uint32_t timeout_ms)
{
    if (!s_up) {
        return -1;
    }
    char msg[24];
    const uint32_t seq = ++s_ping_seq;
    snprintf(msg, sizeof(msg), "PING %" PRIu32, seq);
    const int64_t t0 = esp_timer_get_time();
    if (mpx_pi_link_send_line(msg) != ESP_OK) {
        return -1;
    }
    const int64_t deadline = t0 + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_pong_seq == seq) {
            return s_pong_us - t0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return -1;
}

bool mpx_pi_link_heard_within(uint32_t within_ms)
{
    portENTER_CRITICAL(&s_st_lock);
    const int64_t last = s_st.last_rx_us;
    portEXIT_CRITICAL(&s_st_lock);
    return last != 0 && (esp_timer_get_time() - last) <= (int64_t)within_ms * 1000;
}

void mpx_pi_link_get_stats(mpx_pi_link_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_st_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_st_lock);
}

/* ── receiving ───────────────────────────────────────────────────────────── */

static void handle_line(char *line, size_t len)
{
    /* Strip a trailing CR: the Pi's terminal tools send CRLF. */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_st_lock);
    s_st.lines_rx++;
    strlcpy(s_st.last_line, line, sizeof(s_st.last_line));
    portEXIT_CRITICAL(&s_st_lock);

    if (s_watch) {
        printf("  pi> %s\n", line);
    }

    if (strncmp(line, "PONG ", 5) == 0) {
        s_pong_us  = esp_timer_get_time();
        s_pong_seq = (uint32_t)strtoul(line + 5, NULL, 10);
        portENTER_CRITICAL(&s_st_lock);
        s_st.pongs_rx++;
        portEXIT_CRITICAL(&s_st_lock);
        return;
    }
    if (strncmp(line, "PING", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        /* Echo the sequence back as a number, not as the text we received:
         * a PING line can be up to PI_LINE_MAX bytes and the reply buffer
         * is 24, which GCC's -Wformat-truncation rightly refuses. */
        char reply[24];
        const unsigned long seq = line[4] ? strtoul(line + 5, NULL, 10) : 0;
        snprintf(reply, sizeof(reply), "PONG %lu", seq);
        (void)mpx_pi_link_send_line(reply);
        portENTER_CRITICAL(&s_st_lock);
        s_st.pings_rx++;
        portEXIT_CRITICAL(&s_st_lock);
        return;
    }
    if (strncmp(line, "HELLO", 5) == 0) {
        portENTER_CRITICAL(&s_st_lock);
        strlcpy(s_st.peer_name, line[5] == ' ' ? line + 6 : "?", sizeof(s_st.peer_name));
        portEXIT_CRITICAL(&s_st_lock);
        (void)mpx_pi_link_send_line("HELLO " PI_OUR_NAME);
        ESP_LOGI(TAG, "Pi says hello: %s", line[5] == ' ' ? line + 6 : "(no name)");
        return;
    }

    /* Not protocol. This is where a command dispatcher goes later; for now
     * it is visible in the log so the first real message from the Pi can be
     * seen without `pi watch` running. */
    if (!s_watch) {
        ESP_LOGI(TAG, "pi rx: %s", line);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static char line[PI_LINE_MAX + 1];
    size_t at = 0;
    bool overflow = false;
    uint8_t buf[64];

    for (;;) {
        const int n = uart_read_bytes(PI_UART, buf, sizeof(buf), pdMS_TO_TICKS(PI_RX_POLL_MS));
        if (n <= 0) {
            continue;
        }
        portENTER_CRITICAL(&s_st_lock);
        s_st.bytes_rx  += (uint32_t)n;
        s_st.last_rx_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_st_lock);

        for (int i = 0; i < n; i++) {
            const char c = (char)buf[i];
            if (c == '\n') {
                line[at] = '\0';
                if (overflow) {
                    portENTER_CRITICAL(&s_st_lock);
                    s_st.lines_bad++;
                    portEXIT_CRITICAL(&s_st_lock);
                    ESP_LOGW(TAG, "dropped an overlong line (> %d bytes)", PI_LINE_MAX);
                } else {
                    handle_line(line, at);
                }
                at = 0;
                overflow = false;
                continue;
            }
            /* Framing garbage -- wrong baud, a floating RX pin -- shows up as
             * bytes outside printable ASCII. Count the line bad and move on
             * rather than logging every byte. */
            if (c != '\r' && c != '\t' && (c < 0x20 || c > 0x7E)) {
                overflow = true;
                continue;
            }
            if (at < PI_LINE_MAX) {
                line[at++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

/* ── init ────────────────────────────────────────────────────────────────── */

esp_err_t mpx_pi_link_init(void)
{
    if (s_up) {
        return ESP_ERR_INVALID_STATE;
    }

    const uart_config_t cfg = {
        .baud_rate  = PI_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PI_UART, PI_RX_BUF, PI_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(PI_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(PI_UART, PI_TX_GPIO, PI_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART1 config on TX %d / RX %d failed: %s",
                 PI_TX_GPIO, PI_RX_GPIO, esp_err_to_name(err));
        uart_driver_delete(PI_UART);
        return err;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        uart_driver_delete(PI_UART);
        return ESP_ERR_NO_MEM;
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.tx_gpio = PI_TX_GPIO;
    s_st.rx_gpio = PI_RX_GPIO;
    s_st.baud    = PI_BAUD;
    s_up = true;

    if (xTaskCreate(rx_task, "pi_link", PI_TASK_STACK, NULL, PI_TASK_PRIO, &s_rx_task) != pdPASS) {
        s_up = false;
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        uart_driver_delete(PI_UART);
        return ESP_ERR_NO_MEM;
    }

    /* Say hello once. If the Pi is not up yet this goes nowhere, and that is
     * fine: the Pi's script says HELLO when it starts and we answer then. */
    (void)mpx_pi_link_send_line("HELLO " PI_OUR_NAME);

    ESP_LOGI(TAG, "UART1 to the Pi up: TX GPIO%d -> Pi RXD, RX GPIO%d <- Pi TXD, %d baud",
             PI_TX_GPIO, PI_RX_GPIO, PI_BAUD);
    return ESP_OK;
}

/* ── console ─────────────────────────────────────────────────────────────── */

static void pi_print_status(void)
{
    mpx_pi_link_stats_t st;
    mpx_pi_link_get_stats(&st);

    printf("  UART1  TX GPIO%d -> Pi RXD (GPIO15, header pin 10)\n"
           "         RX GPIO%d <- Pi TXD (GPIO14, header pin 8)   %" PRIu32 " 8N1, GND to GND\n",
           st.tx_gpio, st.rx_gpio, st.baud);
    printf("  tx %" PRIu32 " B   rx %" PRIu32 " B in %" PRIu32 " lines (%" PRIu32 " bad)   "
           "pings from Pi %" PRIu32 "   pongs %" PRIu32 "\n",
           st.bytes_tx, st.bytes_rx, st.lines_rx, st.lines_bad, st.pings_rx, st.pongs_rx);
    if (st.last_rx_us) {
        printf("  last heard %.1f s ago: \"%s\"\n",
               (double)(esp_timer_get_time() - st.last_rx_us) / 1e6, st.last_line);
    } else {
        printf("  nothing received yet\n");
    }
    if (st.peer_name[0]) {
        printf("  Pi identified itself as \"%s\"\n", st.peer_name);
    }

    printf("  ping ... ");
    fflush(stdout);
    const int64_t rtt = mpx_pi_link_ping(PI_PING_TIMEOUT_MS);
    if (rtt >= 0) {
        printf("PONG in %.1f ms\n  >> CONNECTED\n", (double)rtt / 1000.0);
        return;
    }
    printf("no reply in %d ms\n  >> NOT CONNECTED\n", PI_PING_TIMEOUT_MS);
    mpx_pi_link_get_stats(&st);
    if (st.bytes_rx == 0) {
        printf("     Nothing has ever arrived on RX. Either the Pi script is not running,\n"
               "     the wires are swapped (ESP TX must go to Pi RXD), or GND is not shared.\n"
               "     `pi loopback` checks this end on its own.\n");
    } else if (st.lines_bad > st.lines_rx) {
        printf("     Bytes arrive but are garbage: baud mismatch (Pi side must be %" PRIu32 ")\n"
               "     or the Pi's login console is still on that UART (raspi-config).\n", st.baud);
    } else {
        printf("     Lines arrive but no PONG: the Pi is sending, but not running\n"
               "     tools/pi/mp4_link.py -- or its serial console is echoing.\n");
    }
}

static int pi_cmd(int argc, char **argv)
{
    if (!s_up) {
        printf("  pi link is not up -- see the boot log for 'pi_link'\n");
        return 1;
    }
    const char *sub = argc >= 2 ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        pi_print_status();
        return 0;
    }
    if (strcmp(sub, "ping") == 0) {
        int count = argc >= 3 ? atoi(argv[2]) : 5;
        if (count < 1) count = 1;
        if (count > 100) count = 100;
        int ok = 0;
        int64_t best = -1, worst = -1, sum = 0;
        for (int i = 0; i < count; i++) {
            const int64_t rtt = mpx_pi_link_ping(PI_PING_TIMEOUT_MS);
            if (rtt >= 0) {
                ok++;
                sum += rtt;
                if (best < 0 || rtt < best) best = rtt;
                if (rtt > worst) worst = rtt;
                printf("  %2d  PONG  %.1f ms\n", i + 1, (double)rtt / 1000.0);
            } else {
                printf("  %2d  timeout\n", i + 1);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (ok) {
            printf("  %d/%d replies   min %.1f  avg %.1f  max %.1f ms\n", ok, count,
                   (double)best / 1000.0, (double)sum / ok / 1000.0, (double)worst / 1000.0);
        } else {
            printf("  0/%d replies -- see `pi status` for what to check\n", count);
        }
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "send") == 0) {
        if (argc < 3) {
            printf("  usage: pi send <text...>\n");
            return 1;
        }
        char line[PI_LINE_MAX + 1] = {0};
        for (int i = 2; i < argc; i++) {
            if (i > 2) strlcat(line, " ", sizeof(line));
            strlcat(line, argv[i], sizeof(line));
        }
        const esp_err_t err = mpx_pi_link_send_line(line);
        printf("  %s: \"%s\"\n", err == ESP_OK ? "sent" : esp_err_to_name(err), line);
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "watch") == 0) {
        uint32_t secs = argc >= 3 ? (uint32_t)atoi(argv[2]) : 30;
        if (secs < 1) secs = 1;
        printf("  printing everything from the Pi for %" PRIu32 " s (type on the Pi: it shows here)\n", secs);
        s_watch = true;
        vTaskDelay(pdMS_TO_TICKS(secs * 1000));
        s_watch = false;
        printf("  done\n");
        return 0;
    }
    if (strcmp(sub, "loopback") == 0) {
        /* The UART peripheral can feed its own TX back into RX. That proves
         * the driver, the baud rate and this end's pin configuration with
         * nothing plugged in: what we send must come back as a line, and
         * our own PING must draw our own PONG. */
        mpx_pi_link_stats_t before, after;
        mpx_pi_link_get_stats(&before);
        uart_set_loop_back(PI_UART, true);
        vTaskDelay(pdMS_TO_TICKS(50));
        const int64_t rtt = mpx_pi_link_ping(300);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_set_loop_back(PI_UART, false);
        mpx_pi_link_get_stats(&after);
        const uint32_t lines = after.lines_rx - before.lines_rx;
        if (rtt >= 0 && lines >= 2) {
            printf("  loopback OK: PING came back and was answered in %.2f ms (%" PRIu32 " lines)\n"
                   "  the UART driver on GPIO%d/GPIO%d works; if `pi status` still fails,\n"
                   "  the problem is the wire or the Pi side\n",
                   (double)rtt / 1000.0, lines, after.tx_gpio, after.rx_gpio);
            return 0;
        }
        printf("  loopback FAILED (rtt %" PRId64 " us, %" PRIu32 " lines) -- the UART driver itself\n"
               "  is not moving bytes; check CONFIG_MP4_PI_LINK_*_GPIO against the pin map\n",
               rtt, lines);
        return 1;
    }

    printf("\n  pi status        pins, counters, then one PING: CONNECTED / NOT CONNECTED\n"
           "  pi ping [n]      n pings with round-trip times (default 5)\n"
           "  pi send <text>   send one line to the Pi\n"
           "  pi watch [s]     print everything the Pi sends for s seconds (default 30)\n"
           "  pi loopback      self-test this end with the UART's internal loopback, no Pi needed\n\n"
           "  Other end: tools/pi/mp4_link.py on the Pi (answers PING, prints lines, can ping back).\n\n");
    return 0;
}

void register_pi_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "pi",
        .help = "Raspberry Pi UART link: `pi status`, `pi ping`, `pi send`, `pi watch`, `pi loopback`",
        .hint = NULL,
        .func = pi_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
