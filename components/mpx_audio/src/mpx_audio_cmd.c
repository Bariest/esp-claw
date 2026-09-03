/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `audio` console command -- the Phase 1 gate from docs/voice-plan.md.
 *
 * Deliberately a console command and not an HTTP route or an agent tool.
 * Bringing up audio means recording, listening, changing one parameter and
 * recording again, and a serial prompt is the shortest loop for that. It also
 * works before Wi-Fi is up, which matters when the thing being tested is a
 * microphone rather than a network.
 */

#include "mpx_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "audio_cmd";

#define AUDIO_DEFAULT_PATH      "audio/rec.wav"
#define AUDIO_DEFAULT_SECONDS   5
#define AUDIO_DEFAULT_RATE      16000
#define AUDIO_DEFAULT_CHANNELS  2

static void audio_usage(void)
{
    printf("\n"
           "  audio info                       what the board has, and the current volume\n"
           "  audio rec [secs] [pick] [chans]  record to /fatfs/%s\n"
           "  audio play [path]                play a 16-bit WAV from the data root\n"
           "  audio vol [0-100]                software output volume\n"
           "\n"
           "Recording keeps ONE channel out of `chans`, chosen by `pick`, so the\n"
           "microphones can be told apart. On the ES7210 boards the channel order\n"
           "declared in board_devices.yaml is [mic4, mic3, mic2, mic1], where mic3\n"
           "is the echo reference wired from the amplifier output -- record it\n"
           "while something is playing and you should hear the playback.\n"
           "\n"
           "If `audio rec` fails to open the codec, the channel count is the thing\n"
           "to change: the ES7210 runs in TDM and the I2S slot configuration may\n"
           "not agree with what was asked for. Try 1, 2 and 4.\n"
           "\n"
           "  audio rec 5 0 2                  5 s, first channel of two\n"
           "  audio play audio/rec.wav         hear it back through the speaker\n"
           "\n"
           "Off-device: http://<robot-ip>/files/audio/rec.wav downloads it.\n"
           "\n", AUDIO_DEFAULT_PATH);
}

static int audio_cmd(int argc, char **argv)
{
    if (argc < 2) {
        audio_usage();
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("  microphone : %s\n", mpx_audio_have_mic() ? "present" : "absent");
        printf("  speaker    : %s\n", mpx_audio_have_speaker() ? "present" : "absent");
        printf("  volume     : %d%%\n", mpx_audio_get_volume());
        return 0;
    }

    if (strcmp(argv[1], "vol") == 0) {
        if (argc >= 3) {
            mpx_audio_set_volume(atoi(argv[2]));
        }
        printf("  volume %d%%\n", mpx_audio_get_volume());
        return 0;
    }

    if (strcmp(argv[1], "rec") == 0) {
        const uint32_t secs  = (argc >= 3) ? (uint32_t)atoi(argv[2]) : AUDIO_DEFAULT_SECONDS;
        const uint8_t  pick  = (argc >= 4) ? (uint8_t)atoi(argv[3]) : 0;
        const uint8_t  chans = (argc >= 5) ? (uint8_t)atoi(argv[4]) : AUDIO_DEFAULT_CHANNELS;
        const esp_err_t err = mpx_audio_record_wav(AUDIO_DEFAULT_PATH, secs,
                                                   AUDIO_DEFAULT_RATE, chans, pick);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "play") == 0) {
        const char *path = (argc >= 3) ? argv[2] : AUDIO_DEFAULT_PATH;
        const esp_err_t err = mpx_audio_play_wav(path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "playback failed: %s", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    audio_usage();
    return 0;
}

void register_audio_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audio",
        .help = "Record and play audio (see `audio` with no arguments)",
        .hint = NULL,
        .func = audio_cmd,
    };
    if (esp_console_cmd_register(&cmd) == ESP_OK) {
        ESP_LOGI(TAG, "'audio' console command registered");
    }
}
