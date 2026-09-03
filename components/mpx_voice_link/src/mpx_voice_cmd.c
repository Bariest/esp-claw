/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `voice` console command.
 */

#include "mpx_voice_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "voice_cmd";

static void voice_usage(void)
{
    printf("\n"
           "  voice loopback [secs]        microphone -> Opus -> speaker, no network\n"
           "  voice connect <url> [token]  open a xiaozhi websocket\n"
           "  voice disconnect             close it\n"
           "  voice send <json>            send one raw control message\n"
           "  voice info                   link state, session, frame sizes\n"
           "\n"
           "  voice connect ws://192.168.1.50:8000/xiaozhi/v1/\n"
           "\n"
           "Connect waits for the server's hello, not just for the socket to\n"
           "open. A socket that opens and then says nothing is the usual\n"
           "failure -- wrong path, wrong token, wrong protocol version -- and\n"
           "reporting success on TCP alone would hide all of them.\n"
           "\n"
           "Loopback is the Phase 2 gate. Hearing yourself proves the encoder,\n"
           "the decoder and the frame arithmetic without a server existing --\n"
           "which is the whole point of testing it before the socket.\n"
           "\n"
           "It RECORDS first and plays afterwards, never both at once. The\n"
           "microphone and speaker are centimetres apart with 30 dB of gain and\n"
           "no echo cancellation until Phase 4, so running them together is a\n"
           "feedback loop that reaches full scale almost immediately.\n"
           "\n"
           "Watch the summary line afterwards. Roughly 150-400 bytes per 60 ms\n"
           "frame is healthy. Far less means the encoder is being handed\n"
           "silence, and the microphone gain is the thing to look at.\n"
           "\n");
}

static int voice_cmd(int argc, char **argv)
{
    if (argc < 2) {
        voice_usage();
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            printf("  usage: voice connect ws://<host>:<port>/xiaozhi/v1/ [token]\n");
            return 1;
        }
        const esp_err_t err = mpx_voice_connect(argv[2], (argc >= 4) ? argv[3] : NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
            return 1;
        }
        printf("  connected, session %s\n", mpx_voice_session_id());
        return 0;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        mpx_voice_disconnect();
        return 0;
    }

    if (strcmp(argv[1], "send") == 0) {
        if (argc < 3) {
            printf("  usage: voice send <json>\n");
            return 1;
        }
        const esp_err_t err = mpx_voice_send_json(argv[2]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "send failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("  link         : %s\n", mpx_voice_state_name());
        printf("  session      : %s\n",
               mpx_voice_session_id()[0] ? mpx_voice_session_id() : "(none)");
        printf("  downlink     : %u Hz\n", (unsigned)mpx_voice_downlink_rate());
        printf("  codec        : %s\n", mpx_voice_codec_running() ? "running" : "stopped");
        printf("  uplink       : %d Hz, %d frames per %d ms\n",
               MPX_VOICE_UPLINK_RATE, MPX_VOICE_UPLINK_FRAME, MPX_VOICE_FRAME_MS);
        printf("  downlink     : %d Hz, %d frames per %d ms\n",
               MPX_VOICE_DOWNLINK_RATE, MPX_VOICE_DOWNLINK_FRAME, MPX_VOICE_FRAME_MS);
        return 0;
    }

    if (strcmp(argv[1], "loopback") == 0) {
        const uint32_t secs = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 5;
        const esp_err_t err = mpx_voice_loopback(secs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "loopback failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    voice_usage();
    return 0;
}

void register_voice_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "voice",
        .help = "Opus codec test (see `voice` with no arguments)",
        .hint = NULL,
        .func = voice_cmd,
    };
    if (esp_console_cmd_register(&cmd) == ESP_OK) {
        ESP_LOGI(TAG, "'voice' console command registered");
    }
}
