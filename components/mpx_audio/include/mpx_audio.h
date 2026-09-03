/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 1 of docs/voice-plan.md: get sound in and out, and nothing else.
 *
 * No Opus, no WebSocket, no wake word, no AI. The point of this layer is to
 * answer one question on real hardware -- do the microphones and the speaker
 * work, and which channel is which -- because every later phase is miserable
 * to debug if that answer is uncertain.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Look up the board's audio_adc and audio_dac devices.
 *
 * Never fatal. A board with neither -- generic_esp32s3, for instance --
 * leaves mpx_audio_available() false and every call below returns
 * ESP_ERR_NOT_SUPPORTED. */
esp_err_t mpx_audio_init(void);

bool mpx_audio_have_mic(void);
bool mpx_audio_have_speaker(void);

/* Record to a mono 16-bit WAV under the data root.
 *
 * rel_path is relative to CLAW_PATH_DATA, e.g. "audio/rec.wav"; parent
 * directories are created. `channels` is how many the codec is opened with
 * and `pick` selects which one is written, so a four-microphone ES7210 can be
 * examined one channel at a time -- including the echo-cancellation
 * reference, which should carry whatever the speaker just played. */
esp_err_t mpx_audio_record_wav(const char *rel_path,
                               uint32_t seconds,
                               uint32_t sample_rate,
                               uint8_t channels,
                               uint8_t pick);

/* Play a 16-bit PCM WAV from the data root. Mono files are duplicated to both
 * I2S slots; the sample rate comes from the file. */
esp_err_t mpx_audio_play_wav(const char *rel_path);

/* Play a sine wave, generated on the fly.
 *
 * The point of this is diagnosis: it takes the microphone, the filesystem and
 * the recording out of the question entirely. If a tone is audible then the
 * amplifier, the speaker, the I2S wiring and the volume are all fine, and
 * anything still wrong is on the capture side. If it is not audible, nothing
 * on the capture side can be blamed. */
esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds);

/* Software volume, 0-100.
 *
 * Software because the MAX98357A on these boards has no control interface at
 * all: SD_MODE is strapped and GAIN goes to a test point, so
 * esp_codec_dev_set_out_vol() is a no-op. Applied to the samples before they
 * reach I2S. */
void mpx_audio_set_volume(int percent);
int  mpx_audio_get_volume(void);

/* Console command: audio rec|play|vol|info */
void register_audio_command(void);

#ifdef __cplusplus
}
#endif
