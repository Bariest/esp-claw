/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART link to a Raspberry Pi on the J-PI expansion header.
 *
 * The CORE board brings four GPIOs to that header (IO18_PI_1, IO8_PI_2,
 * IO3_PI_3, IO11_PI_4 -- see boards/mp4_esp32_core/README.md). Two of them
 * are a UART here: CONFIG_MP4_PI_LINK_TX_GPIO (ESP -> Pi) and
 * CONFIG_MP4_PI_LINK_RX_GPIO (Pi -> ESP), 3.3 V logic on both ends, so no
 * level shifter -- the Pi's GPIO14/15 UART is 3.3 V too.
 *
 * Bring-up protocol, deliberately the simplest thing that proves the wire:
 * newline-terminated ASCII lines.
 *
 *   ESP -> Pi   PING <n>          Pi answers   PONG <n>
 *   Pi  -> ESP  PING <n>          ESP answers  PONG <n>
 *   either      HELLO <name>      other side answers HELLO <its name>
 *   anything else is logged as `pi rx: ...` and kept as "last line".
 *
 * tools/pi/mp4_link.py is the other end: it answers pings, prints what it
 * hears, and can ping back. `pi status` on the console sends a PING and says
 * CONNECTED or NOT CONNECTED from whether a PONG comes back within a second;
 * `pi loopback` proves the driver and the pin configuration with no Pi at all
 * (the UART's internal loopback feeds TX straight back into RX).
 *
 * What this is NOT yet: a command protocol. Once the Pi is talking, the
 * "anything else" branch in mpx_pi_link.c is where a real dispatcher goes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the UART and start the receive task. Safe to call once; refuses a
 * second call with ESP_ERR_INVALID_STATE. */
esp_err_t mpx_pi_link_init(void);

/* Send one line. `line` must not contain a newline; one is appended. */
esp_err_t mpx_pi_link_send_line(const char *line);

/* Send PING, wait up to `timeout_ms` for the matching PONG. Returns the
 * round trip in microseconds, or -1 on timeout. */
int64_t mpx_pi_link_ping(uint32_t timeout_ms);

/* True if anything was heard from the Pi within the last `within_ms`. */
bool mpx_pi_link_heard_within(uint32_t within_ms);

typedef struct {
    int      tx_gpio, rx_gpio;
    uint32_t baud;
    uint32_t bytes_tx, bytes_rx;
    uint32_t lines_rx, lines_bad;   /* bad = overlong or non-ASCII */
    uint32_t pongs_rx, pings_rx;
    int64_t  last_rx_us;            /* esp_timer time of last byte, 0 = never */
    char     last_line[96];
    char     peer_name[32];         /* from the Pi's HELLO, "" if none yet */
} mpx_pi_link_stats_t;

void mpx_pi_link_get_stats(mpx_pi_link_stats_t *out);

/* `pi status|ping|send|watch|loopback|help` */
void register_pi_command(void);

#ifdef __cplusplus
}
#endif
