/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpx_voice_link.h"

#include <cinttypes>
#include <cstring>
#include <memory>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "mpx_audio.h"
#include "opus_decoder.h"
#include "opus_encoder.h"

static const char *TAG = "mpx_voice";

namespace {

std::unique_ptr<OpusEncoderWrapper> s_encoder;
std::unique_ptr<OpusDecoderWrapper> s_decoder;

}  // namespace

extern "C" esp_err_t mpx_voice_codec_start(uint32_t encode_rate, uint32_t decode_rate)
{
    if (s_encoder && s_decoder) {
        return ESP_OK;
    }
    s_encoder = std::make_unique<OpusEncoderWrapper>((int)encode_rate, 1, MPX_VOICE_FRAME_MS);
    s_decoder = std::make_unique<OpusDecoderWrapper>((int)decode_rate, 1, MPX_VOICE_FRAME_MS);
    if (!s_encoder || !s_decoder) {
        s_encoder.reset();
        s_decoder.reset();
        return ESP_ERR_NO_MEM;
    }

    /* Complexity 0, as the reference firmware sets. Higher settings buy a
     * little quality for a lot of CPU, and on a device that also runs an
     * acoustic front end and a TLS socket the CPU is worth more. */
    s_encoder->SetComplexity(0);

    ESP_LOGI(TAG, "opus ready: encode %" PRIu32 " Hz, decode %" PRIu32 " Hz, %d ms frames",
             encode_rate, decode_rate, MPX_VOICE_FRAME_MS);
    return ESP_OK;
}

extern "C" void mpx_voice_codec_stop(void)
{
    s_encoder.reset();
    s_decoder.reset();
}

extern "C" bool mpx_voice_codec_running(void)
{
    return s_encoder != nullptr && s_decoder != nullptr;
}

extern "C" int mpx_voice_encode(const int16_t *pcm, size_t frames,
                                uint8_t *out, size_t out_size)
{
    if (!s_encoder || !pcm || !out) {
        return -ESP_ERR_INVALID_STATE;
    }

    /* The wrapper takes ownership of the PCM vector, so this copy is not
     * avoidable without changing the library. One 60 ms frame is 1920 bytes. */
    std::vector<int16_t> in(pcm, pcm + frames);
    std::vector<uint8_t> packet;

    if (!s_encoder->Encode(std::move(in), packet)) {
        return -ESP_FAIL;
    }
    if (packet.size() > out_size) {
        ESP_LOGW(TAG, "opus packet %u B does not fit in %u B",
                 (unsigned)packet.size(), (unsigned)out_size);
        return -ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, packet.data(), packet.size());
    return (int)packet.size();
}

extern "C" int mpx_voice_decode(const uint8_t *packet, size_t len,
                                int16_t *pcm, size_t max_frames)
{
    if (!s_decoder || !packet || !pcm) {
        return -ESP_ERR_INVALID_STATE;
    }

    std::vector<uint8_t> in(packet, packet + len);
    std::vector<int16_t> out;

    if (!s_decoder->Decode(std::move(in), out)) {
        return -ESP_FAIL;
    }
    if (out.size() > max_frames) {
        return -ESP_ERR_INVALID_SIZE;
    }
    memcpy(pcm, out.data(), out.size() * sizeof(int16_t));
    return (int)out.size();
}

/* ── loopback ──────────────────────────────────────────────────────────────
 *
 * Encode and decode at the SAME rate here, which is not what the protocol
 * does -- uplink is 16 kHz and downlink 24 kHz. Matching them means the
 * decoded frame can go straight to the speaker at the rate it was captured,
 * so this tests the codec without also needing a resampler. Getting the two
 * rates right is the socket's job, in the next phase.
 */
extern "C" esp_err_t mpx_voice_loopback(uint32_t seconds)
{
    const uint32_t rate = MPX_VOICE_UPLINK_RATE;
    const size_t frame = MPX_VOICE_UPLINK_FRAME;
    esp_err_t ret = ESP_OK;
    uint32_t packets = 0;
    uint32_t bytes = 0;

    ESP_RETURN_ON_ERROR(mpx_voice_codec_start(rate, rate), TAG, "codec start failed");

    std::vector<int16_t> pcm(frame);
    std::vector<int16_t> back(frame * 2);
    std::vector<uint8_t> packet(MPX_VOICE_PACKET_MAX);

    ESP_RETURN_ON_ERROR(mpx_audio_capture_start(rate, 2, 0), TAG, "capture start failed");
    ret = mpx_audio_output_start(rate);
    if (ret != ESP_OK) {
        mpx_audio_capture_stop();
        return ret;
    }

    ESP_LOGI(TAG, "loopback for %" PRIu32 " s -- speak, and you should hear yourself",
             seconds);

    const uint32_t total = (seconds * 1000u) / MPX_VOICE_FRAME_MS;
    for (uint32_t i = 0; i < total; i++) {
        ret = mpx_audio_capture_read(pcm.data(), frame);
        if (ret != ESP_OK) {
            break;
        }

        const int len = mpx_voice_encode(pcm.data(), frame, packet.data(), packet.size());
        if (len < 0) {
            ESP_LOGE(TAG, "encode failed");
            ret = ESP_FAIL;
            break;
        }
        packets++;
        bytes += (uint32_t)len;

        const int got = mpx_voice_decode(packet.data(), (size_t)len,
                                         back.data(), back.size());
        if (got < 0) {
            ESP_LOGE(TAG, "decode failed");
            ret = ESP_FAIL;
            break;
        }

        ret = mpx_audio_output_write(back.data(), (size_t)got);
        if (ret != ESP_OK) {
            break;
        }
    }

    mpx_audio_output_stop();
    mpx_audio_capture_stop();

    if (packets) {
        /* Bitrate is the number worth seeing: a 60 ms frame at a sane rate is
         * roughly 150-400 bytes, so about 20-50 kbit/s. Wildly smaller means
         * the encoder is being fed silence. */
        ESP_LOGI(TAG, "%" PRIu32 " packets, %" PRIu32 " bytes, average %" PRIu32
                      " B/frame, about %" PRIu32 " kbit/s",
                 packets, bytes, bytes / packets,
                 (bytes * 8u) / (packets * MPX_VOICE_FRAME_MS));
    }
    return ret;
}
