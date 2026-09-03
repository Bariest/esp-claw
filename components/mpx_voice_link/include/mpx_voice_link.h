/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 2 of docs/voice-plan.md: Opus, then the socket.
 *
 * This file currently covers the codec half only. The gate for it is
 * `voice loopback`: capture from the microphone, encode to Opus, decode
 * straight back, and play. If that sounds like you, the codec, the frame
 * sizes and the buffer handling are all correct -- and none of it depends on
 * a server existing, which is the point of doing it first.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16 kHz up, 24 kHz down, 60 ms frames -- the xiaozhi protocol's numbers, and
 * what the server will negotiate in its hello reply. Fixed here so the frame
 * arithmetic elsewhere has one place to read them from. */
#define MPX_VOICE_UPLINK_RATE     16000
#define MPX_VOICE_DOWNLINK_RATE   24000
#define MPX_VOICE_FRAME_MS        60
#define MPX_VOICE_UPLINK_FRAME    ((MPX_VOICE_UPLINK_RATE / 1000) * MPX_VOICE_FRAME_MS)
#define MPX_VOICE_DOWNLINK_FRAME  ((MPX_VOICE_DOWNLINK_RATE / 1000) * MPX_VOICE_FRAME_MS)

/* An Opus frame at these rates is a few hundred bytes; this is generous. */
#define MPX_VOICE_PACKET_MAX      1500

esp_err_t mpx_voice_codec_start(uint32_t encode_rate, uint32_t decode_rate);
void      mpx_voice_codec_stop(void);
bool      mpx_voice_codec_running(void);

/* Returns the encoded length, or a negative esp_err_t on failure. */
int mpx_voice_encode(const int16_t *pcm, size_t frames, uint8_t *out, size_t out_size);

/* Returns the number of PCM frames produced, or negative on failure. */
int mpx_voice_decode(const uint8_t *packet, size_t len, int16_t *pcm, size_t max_frames);

/* Microphone to Opus to speaker, with nothing in between. */
esp_err_t mpx_voice_loopback(uint32_t seconds);

void register_voice_command(void);

#ifdef __cplusplus
}
#endif
