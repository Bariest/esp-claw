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
#include <stddef.h>
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

/* ── Streaming ─────────────────────────────────────────────────────────────
 *
 * The file-based calls above are for bring-up. Everything after Phase 1 wants
 * frames: capture a block, encode it, send it; receive a block, decode it,
 * play it. These keep the codec open across calls so that is possible.
 *
 * Capture yields MONO -- one channel picked out of `channels`, as with
 * mpx_audio_record_wav -- and playback takes mono and duplicates it to both
 * I2S slots. Mono is what Opus and every speech model want.
 *
 * At most MPX_AUDIO_MAX_FRAMES per call. A 60 ms Opus frame at 16 kHz is 960,
 * so this is two of them. */
#define MPX_AUDIO_MAX_FRAMES  1920

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick);
esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames);
void      mpx_audio_capture_stop(void);

/* True between a successful mpx_audio_capture_start() and the matching
 * mpx_audio_capture_stop() -- the codec is a single hardware stream, so
 * whoever calls capture_start() next while this is true does NOT get an
 * error (capture_start() is intentionally idempotent, for a caller
 * resuming its own already-open capture) but WOULD be reading the same
 * stream as whoever opened it, silently interleaving with them. Callers
 * that do not own the capture themselves -- mpx_voice_wake's `wake start`,
 * mpx_voice_link's `voice talk` -- check this first and refuse instead of
 * risking that. */
bool mpx_audio_capture_active(void);

/* How many channels the current (or most recent) capture_start() opened.
 * Needed by a caller reading the interleaved raw stream below, since it
 * has to size its own buffer -- mpx_audio_capture_read()'s mono callers
 * never needed to know this. */
uint8_t mpx_audio_capture_channels(void);

/* Same capture, without the de-interleave-to-mono step: `interleaved` gets
 * `frames * mpx_audio_capture_channels()` samples, straight off the codec,
 * in whatever order the ES7210's TDM slots deliver them (see
 * mpx_audio_cmd.c's usage text for what is documented about that order,
 * and mpx_voice_wake's comments for what is actually verified). For a
 * multi-mic consumer like an AFE front end that needs every channel, not
 * just one picked out of it. */
esp_err_t mpx_audio_capture_read_raw(int16_t *interleaved, size_t frames);

esp_err_t mpx_audio_output_start(uint32_t sample_rate);
esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames);
void      mpx_audio_output_stop(void);

/* Microphone gain in dB, 0-60, applied on the next recording. */
void mpx_audio_set_mic_gain(int db);
int  mpx_audio_get_mic_gain(void);

/* Console command: audio rec|play|vol|info */
void register_audio_command(void);

#ifdef __cplusplus
}
#endif
