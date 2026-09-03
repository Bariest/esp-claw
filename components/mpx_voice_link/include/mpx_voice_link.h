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

/* ── The link ──────────────────────────────────────────────────────────────
 *
 * A xiaozhi-protocol WebSocket. This half is the connection and the
 * handshake; audio streaming follows once a server answers.
 *
 * The gate for it is `voice connect ws://<host>:<port>/xiaozhi/v1/` printing
 * the server's hello reply and a session id. That proves the URL, the
 * headers, the JSON shape and the protocol version all agree, which is worth
 * establishing before audio is in the picture -- a handshake mismatch and a
 * codec mismatch look identical from the outside. */
typedef enum {
    MPX_VOICE_IDLE = 0,      /* no socket */
    MPX_VOICE_CONNECTING,    /* socket opening */
    MPX_VOICE_HANDSHAKING,   /* our hello sent, waiting for theirs */
    MPX_VOICE_READY,         /* session established */
} mpx_voice_state_t;

/* ── Provisioning ──────────────────────────────────────────────────────────
 *
 * The websocket URL is not something you configure. The device asks an OTA
 * endpoint for it, and the reply carries the address and the token -- plus,
 * the first time, a six-digit code to type into the Xiaozhi console to bind
 * the device to an agent.
 *
 * So the order is: provision, read the code, bind it in the console,
 * provision again, then connect. */
#define MPX_VOICE_DEFAULT_OTA_URL  "https://api.tenclass.net/xiaozhi/ota/"

esp_err_t mpx_voice_provision(const char *ota_url);
const char *mpx_voice_stored_url(void);
const char *mpx_voice_stored_token(void);

/* Passing NULL for either uses whatever provisioning stored. */
esp_err_t mpx_voice_connect(const char *url, const char *token);
void      mpx_voice_disconnect(void);

mpx_voice_state_t mpx_voice_state(void);
const char *mpx_voice_state_name(void);
const char *mpx_voice_session_id(void);
uint32_t    mpx_voice_downlink_rate(void);

/* Send one control message. `json` is sent verbatim, so the caller owns the
 * shape -- this is deliberately thin while the protocol is being brought up. */
esp_err_t mpx_voice_send_json(const char *json);

void register_voice_command(void);

#ifdef __cplusplus
}
#endif
