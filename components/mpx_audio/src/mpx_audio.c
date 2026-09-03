/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpx_audio.h"

#include "esp_log.h"

static const char *TAG = "mpx_audio";

/* The whole implementation is behind this.
 *
 * esp_codec_dev only exists in the build when the selected board declares an
 * audio_codec device -- that is esp_board_manager's own `matches` rule, and
 * idf_component.yml here repeats it. On a board without audio the headers are
 * simply absent, so the code that uses them cannot be compiled at all, and
 * the stubs at the bottom of this file stand in. */
#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "claw_paths.h"
#include "dev_audio_codec.h"
#include "driver/i2s_std.h"
#include "esp_board_periph.h"
#include "esp_board_device.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 4 KB of 16-bit frames. Big enough that the per-read overhead disappears,
 * small enough to sit in PSRAM without anyone noticing. */
#define AUDIO_CHUNK_BYTES   4096

/* claw_paths.h publishes no maximum, so pick one. Deep enough for the data
 * root plus a couple of directories, short enough that the buffer is not a
 * meaningful stack cost. */
#define AUDIO_PATH_MAX      192

static dev_audio_codec_handles_t *s_mic;
static i2s_chan_handle_t s_tx;
static int s_volume = 70;

/* Microphone gain in dB, applied AFTER the codec is opened.
 *
 * 30 dB is what the reference firmware for this board uses
 * (AUDIO_CODEC_DEFAULT_MIC_GAIN in SantaTest). Without it the ES7210 runs at
 * its reset default and a normal speaking voice records at a peak of a couple
 * of thousand out of 32767 -- present, but far too quiet to hear on playback,
 * which is easy to mistake for a broken speaker.
 *
 * `adc_init_gain: 30` in board_devices.yaml does NOT do this. That value
 * lands in the board manager's own config struct; esp_codec_dev never reads
 * it, and nothing logs that the gain was left alone. */
static float s_mic_gain_db = 30.0f;

/* ── Output goes straight to I2S, with no codec in between ─────────────────
 *
 * The MAX98357A has no control interface: no I2C, no registers, nothing to
 * configure. esp_codec_dev's "dummy" codec exists to represent exactly that,
 * and going through it produced a TX path that clocked data out for the right
 * length of time and made no sound, while logging
 *
 *     i2s_channel_disable(): the channel has not been enabled yet
 *
 * on every open -- the channel's enable state was not what the driver
 * believed. Rather than keep guessing at that, this follows the firmware
 * already proven on this board (SantaTest's santa_audio_codec.cc), which
 * creates no output codec device at all and calls i2s_channel_write()
 * directly. There is genuinely nothing for a codec layer to do here.
 *
 * The board manager hands out the TX channel handle already enabled, so this
 * only reconfigures the sample rate when it differs from the board default. */
static esp_err_t audio_out_open(uint32_t rate)
{
    if (!s_tx) {
        void *handle = NULL;
        if (esp_board_periph_get_handle("i2s_audio_out", &handle) != ESP_OK || !handle) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_tx = (i2s_chan_handle_t)handle;
    }

    /* Reconfiguring requires the channel to be stopped. It may already be
     * stopped, and that is not an error worth reporting. */
    (void)i2s_channel_disable(s_tx);

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG,
                        "cannot set %" PRIu32 " Hz", rate);

    /* Philips framing, 16-bit, both slots -- what the amplifier expects and
     * what the working firmware sends. */
    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                            I2S_SLOT_MODE_STEREO);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_tx, &slot), TAG,
                        "cannot set slot format");

    return i2s_channel_enable(s_tx);
}

static esp_err_t audio_out_write(const void *buf, size_t bytes)
{
    size_t written = 0;
    return i2s_channel_write(s_tx, buf, bytes, &written, portMAX_DELAY);
}

static void audio_out_close(void)
{
    if (s_tx) {
        (void)i2s_channel_disable(s_tx);
    }
}

/* ── WAV ───────────────────────────────────────────────────────────────────
 *
 * Written by hand rather than pulled from a library: it is 44 bytes, it is
 * the only container this layer needs, and a dependency here would be a
 * dependency in every later phase too. */
#pragma pack(push, 1)
typedef struct {
    char     riff[4];
    uint32_t file_size;        /* everything after this field */
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t format;           /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

static void wav_header_fill(wav_header_t *h, uint32_t rate, uint16_t channels,
                            uint32_t data_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt,  "fmt ", 4);
    memcpy(h->data, "data", 4);
    h->fmt_size        = 16;
    h->format          = 1;
    h->channels        = channels;
    h->sample_rate     = rate;
    h->bits_per_sample = 16;
    h->block_align     = (uint16_t)(channels * 2);
    h->byte_rate       = rate * h->block_align;
    h->data_size       = data_bytes;
    h->file_size       = data_bytes + sizeof(wav_header_t) - 8;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Resolve a data-root-relative path and create its parent directories.
 *
 * mkdir() per component rather than a recursive helper, because FATFS has no
 * mkdir -p and the paths here are two levels deep at most. EEXIST is success. */
static esp_err_t audio_prepare_path(const char *rel, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(rel && rel[0], ESP_ERR_INVALID_ARG, TAG, "empty path");
    ESP_RETURN_ON_FALSE(!strstr(rel, ".."), ESP_ERR_INVALID_ARG, TAG, "path escapes root");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, rel, out, out_size),
                        TAG, "path too long");

    for (char *slash = strchr(out + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(out, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed: %s", out, strerror(errno));
        }
        *slash = '/';
    }
    return ESP_OK;
}

/* Integer square root, for the RMS report. sqrt() from libm would work, but
 * it drags float formatting into a logging path that runs on the console
 * task, and this is exact enough for a level meter. */
static uint32_t isqrt64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

static void *audio_alloc(size_t size)
{
    /* PSRAM first: these buffers are large and nothing DMAs straight out of
     * them -- esp_codec_dev copies through the I2S driver either way -- and
     * internal RAM is the scarce resource on this firmware. */
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

/* ── init ────────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_init(void)
{
    void *handle = NULL;

    if (esp_board_device_get_handle("audio_adc", &handle) == ESP_OK && handle) {
        s_mic = (dev_audio_codec_handles_t *)handle;
    }
    handle = NULL;
    if (esp_board_periph_get_handle("i2s_audio_out", &handle) == ESP_OK && handle) {
        s_tx = (i2s_chan_handle_t)handle;
    }

    ESP_LOGI(TAG, "microphone %s, speaker %s",
             mpx_audio_have_mic() ? "ready" : "absent",
             mpx_audio_have_speaker() ? "ready" : "absent");

#if !CONFIG_CODEC_DUMMY_SUPPORT
    if (!mpx_audio_have_speaker()) {
        /* Much the most likely reason, and invisible otherwise: an amplifier
         * with no control interface is declared `chip: internal`, and that is
         * backed by the "dummy" codec. Without CONFIG_CODEC_DUMMY_SUPPORT the
         * board manager cannot create the device, says nothing about it, and
         * the only trace is the word "absent" above. */
        ESP_LOGW(TAG, "audio_dac missing and CONFIG_CODEC_DUMMY_SUPPORT is off");
        ESP_LOGW(TAG, "a `chip: internal` amplifier needs it -- set it in the "
                      "board's sdkconfig.defaults.board");
    }
#endif
    return ESP_OK;
}

bool mpx_audio_have_mic(void)     { return s_mic && s_mic->codec_dev; }
bool mpx_audio_have_speaker(void) { return s_tx != NULL; }

void mpx_audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void mpx_audio_set_mic_gain(int db)
{
    s_mic_gain_db = (float)(db < 0 ? 0 : (db > 60 ? 60 : db));
}

int mpx_audio_get_mic_gain(void) { return (int)s_mic_gain_db; }

int mpx_audio_get_volume(void) { return s_volume; }

/* ── streaming ─────────────────────────────────────────────────────────────
 *
 * Same codec, same I2S channel as the file calls, but held open across calls
 * so a caller can push frames through Opus and a socket without paying an
 * open/close per block. */

static bool     s_cap_open;
static uint8_t  s_cap_channels = 1;
static uint8_t  s_cap_pick;
static int16_t *s_cap_scratch;      /* interleaved, straight off the codec */
static int16_t *s_out_scratch;      /* stereo, after volume */

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick)
{
    int rc;

    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");
    ESP_RETURN_ON_FALSE(channels >= 1 && channels <= 4 && pick < channels,
                        ESP_ERR_INVALID_ARG, TAG, "bad channel selection");
    if (s_cap_open) {
        return ESP_OK;
    }

    /* Non-zero mask, for the reason spelled out in mpx_audio_record_wav. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = channels,
        .channel_mask    = (uint16_t)((1u << channels) - 1u),
        .sample_rate     = sample_rate,
        .mclk_multiple   = 0,
    };

    rc = esp_codec_dev_open(s_mic->codec_dev, &fs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "codec open failed: rc=%d", rc);

    if (esp_codec_dev_set_in_channel_gain(s_mic->codec_dev, fs.channel_mask,
                                          s_mic_gain_db) != 0) {
        (void)esp_codec_dev_set_in_gain(s_mic->codec_dev, s_mic_gain_db);
    }

    s_cap_scratch = audio_alloc(MPX_AUDIO_MAX_FRAMES * channels * sizeof(int16_t));
    if (!s_cap_scratch) {
        esp_codec_dev_close(s_mic->codec_dev);
        return ESP_ERR_NO_MEM;
    }

    s_cap_channels = channels;
    s_cap_pick = pick;
    s_cap_open = true;
    return ESP_OK;
}

esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames)
{
    ESP_RETURN_ON_FALSE(s_cap_open, ESP_ERR_INVALID_STATE, TAG, "capture not started");
    ESP_RETURN_ON_FALSE(mono && frames && frames <= MPX_AUDIO_MAX_FRAMES,
                        ESP_ERR_INVALID_ARG, TAG, "bad frame count");

    const int bytes = (int)(frames * s_cap_channels * sizeof(int16_t));
    const int rc = esp_codec_dev_read(s_mic->codec_dev, s_cap_scratch, bytes);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "read failed: rc=%d", rc);

    for (size_t i = 0; i < frames; i++) {
        mono[i] = s_cap_scratch[i * s_cap_channels + s_cap_pick];
    }
    return ESP_OK;
}

void mpx_audio_capture_stop(void)
{
    if (!s_cap_open) {
        return;
    }
    esp_codec_dev_close(s_mic->codec_dev);
    free(s_cap_scratch);
    s_cap_scratch = NULL;
    s_cap_open = false;
}

esp_err_t mpx_audio_output_start(uint32_t sample_rate)
{
    ESP_RETURN_ON_ERROR(audio_out_open(sample_rate), TAG, "cannot open I2S output");

    if (!s_out_scratch) {
        s_out_scratch = audio_alloc(MPX_AUDIO_MAX_FRAMES * 2u * sizeof(int16_t));
        if (!s_out_scratch) {
            audio_out_close();
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames)
{
    ESP_RETURN_ON_FALSE(s_out_scratch, ESP_ERR_INVALID_STATE, TAG, "output not started");
    ESP_RETURN_ON_FALSE(mono && frames && frames <= MPX_AUDIO_MAX_FRAMES,
                        ESP_ERR_INVALID_ARG, TAG, "bad frame count");

    for (size_t i = 0; i < frames; i++) {
        const int16_t v = (int16_t)((int32_t)mono[i] * s_volume / 100);
        s_out_scratch[i * 2]     = v;
        s_out_scratch[i * 2 + 1] = v;
    }
    return audio_out_write(s_out_scratch, frames * 2u * sizeof(int16_t));
}

void mpx_audio_output_stop(void)
{
    audio_out_close();
    free(s_out_scratch);
    s_out_scratch = NULL;
}

/* ── record ──────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_record_wav(const char *rel_path, uint32_t seconds,
                               uint32_t sample_rate, uint8_t channels,
                               uint8_t pick)
{
    char full[AUDIO_PATH_MAX];
    wav_header_t header = {0};
    uint8_t *chunk = NULL;
    int16_t *mono = NULL;
    FILE *f = NULL;
    esp_err_t ret = ESP_OK;
    uint32_t written_bytes = 0;
    int32_t peak = 0;
    uint64_t energy = 0;
    uint32_t energy_count = 0;
    int rc;

    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");
    ESP_RETURN_ON_FALSE(channels >= 1 && channels <= 4, ESP_ERR_INVALID_ARG,
                        TAG, "channels must be 1-4");
    ESP_RETURN_ON_FALSE(pick < channels, ESP_ERR_INVALID_ARG, TAG,
                        "pick must be below channels");
    ESP_RETURN_ON_ERROR(audio_prepare_path(rel_path, full, sizeof(full)), TAG, "bad path");

    /* channel_mask MUST be non-zero. This is not a preference.
     *
     * es7210_config_fs() contains:
     *
     *     if (es7210_is_tdm_mode(codec) && fs->channel <= 2 &&
     *             fs->channel_mask == 0) {
     *         bits >>= 1;
     *     }
     *
     * With the ES7210 in TDM and a request for two channels and no mask, the
     * driver silently HALVES the sample width -- the log line is
     * "ES7210: Bits 8" -- so it can pack two 8-bit TDM channels into each
     * 16-bit I2S slot. That is a deliberate trick for reading four
     * microphones through a two-slot I2S port, but the buffer that comes back
     * is not 16-bit PCM: each word holds two different channels, one per
     * byte. Read it as int16 and you get noise at very low level, which
     * sounds exactly like a broken microphone or a dead speaker.
     *
     * Passing a real mask skips that branch, keeps the ADC at 16 bits, and
     * gives ordinary interleaved PCM. (1 << channels) - 1 selects the first
     * `channels` channels, which is what the de-interleave below assumes. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = channels,
        .channel_mask    = (uint16_t)((1u << channels) - 1u),
        .sample_rate     = sample_rate,
        .mclk_multiple   = 0,          /* 0 means 256x, which is what the board wires */
    };

    rc = esp_codec_dev_open(s_mic->codec_dev, &fs);
    if (rc != 0) {
        /* Not necessarily fatal for the user: the ES7210 is in TDM and the
         * I2S slot config may not agree with the channel count asked for.
         * Saying so beats a bare error code, because the fix is to pass a
         * different channel count on the command line. */
        ESP_LOGE(TAG, "codec open failed (rc=%d) at %" PRIu32 " Hz, %u channel(s)",
                 rc, sample_rate, (unsigned)channels);
        ESP_LOGE(TAG, "try a different channel count: `audio rec 5 0 2`");
        return ESP_FAIL;
    }

    chunk = audio_alloc(AUDIO_CHUNK_BYTES);
    mono  = audio_alloc(AUDIO_CHUNK_BYTES / channels);
    ESP_GOTO_ON_FALSE(chunk && mono, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    f = fopen(full, "wb");
    ESP_GOTO_ON_FALSE(f, ESP_FAIL, cleanup, TAG, "cannot write %s", full);

    /* Header first with a zero length, patched once the size is known. */
    wav_header_fill(&header, sample_rate, 1, 0);
    ESP_GOTO_ON_FALSE(fwrite(&header, 1, sizeof(header), f) == sizeof(header),
                      ESP_FAIL, cleanup, TAG, "header write failed");

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / (channels * 2u);
    const uint32_t frames_total     = sample_rate * seconds;

    /* Gain must be set after open: before that the codec is unconfigured and
     * there is nothing to write the register to. */
    rc = esp_codec_dev_set_in_channel_gain(s_mic->codec_dev, fs.channel_mask,
                                           s_mic_gain_db);
    if (rc != 0) {
        rc = esp_codec_dev_set_in_gain(s_mic->codec_dev, s_mic_gain_db);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "could not set microphone gain (rc=%d); recording will "
                      "be quiet", rc);
    }

    ESP_LOGI(TAG, "recording %" PRIu32 " s at %" PRIu32 " Hz, %u channel(s), "
                  "keeping channel %u, gain %d dB",
             seconds, sample_rate, (unsigned)channels, (unsigned)pick,
             (int)s_mic_gain_db);

    for (uint32_t done = 0; done < frames_total; done += frames_per_chunk) {
        rc = esp_codec_dev_read(s_mic->codec_dev, chunk, AUDIO_CHUNK_BYTES);
        if (rc != 0) {
            ESP_LOGE(TAG, "read failed: rc=%d", rc);
            ret = ESP_FAIL;
            goto cleanup;
        }

        /* De-interleave: keep one channel out of every frame. */
        const int16_t *src = (const int16_t *)chunk;
        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            mono[i] = src[i * channels + pick];
        }

        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            const int32_t v = mono[i] < 0 ? -(int32_t)mono[i] : mono[i];
            if (v > peak) {
                peak = v;
            }
            energy += (uint64_t)((int32_t)mono[i] * (int32_t)mono[i]);
            energy_count++;
        }

        const size_t bytes = frames_per_chunk * sizeof(int16_t);
        if (fwrite(mono, 1, bytes, f) != bytes) {
            ESP_LOGE(TAG, "short write -- storage full?");
            ret = ESP_FAIL;
            goto cleanup;
        }
        written_bytes += (uint32_t)bytes;
    }

    /* Patch the two length fields now that the size is known. */
    wav_header_fill(&header, sample_rate, 1, written_bytes);
    if (fseek(f, 0, SEEK_SET) != 0 ||
            fwrite(&header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "could not patch WAV header");
        ret = ESP_FAIL;
        goto cleanup;
    }

    /* The single most useful number here, because it answers "did the
     * microphone hear anything" without leaving the console. Full scale is
     * 32767; a silent channel sits in the low tens. */
    {
        const uint32_t rms = energy_count
                             ? (uint32_t)isqrt64(energy / energy_count) : 0;
        ESP_LOGI(TAG, "level: peak %" PRId32 " / 32767, rms %" PRIu32,
                 peak, rms);
        if (peak < 200) {
            ESP_LOGW(TAG, "that is silence -- wrong channel, or the "
                          "microphone is not being reached");
        }
    }

    ESP_LOGI(TAG, "wrote %s (%" PRIu32 " bytes)", full, written_bytes);
    ESP_LOGI(TAG, "download it: http://<robot-ip>/files/%s", rel_path);

cleanup:
    if (f) {
        fclose(f);
    }
    free(chunk);
    free(mono);
    esp_codec_dev_close(s_mic->codec_dev);
    return ret;
}

/* ── tone ──────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds)
{
    const uint32_t rate = 16000;
    int16_t *buf = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(mpx_audio_have_speaker(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no speaker on this board");
    ESP_RETURN_ON_FALSE(hz >= 20 && hz <= rate / 2, ESP_ERR_INVALID_ARG, TAG,
                        "frequency must be 20 Hz to %" PRIu32 " Hz", rate / 2);

    ESP_RETURN_ON_ERROR(audio_out_open(rate), TAG, "cannot open I2S output");

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / 4u;
    buf = audio_alloc(frames_per_chunk * 2u * sizeof(int16_t));
    ESP_GOTO_ON_FALSE(buf, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    ESP_LOGI(TAG, "playing %" PRIu32 " Hz for %" PRIu32 " s at volume %d%%",
             hz, seconds, s_volume);

    /* A third of full scale on purpose: a full-scale tone into a small
     * speaker is unpleasant and tells you nothing extra. */
    const float amplitude = 10000.0f * (float)s_volume / 100.0f;
    uint32_t phase = 0;
    const uint32_t frames_total = rate * seconds;

    for (uint32_t done = 0; done < frames_total; done += frames_per_chunk) {
        for (uint32_t i = 0; i < frames_per_chunk; i++) {
            const float t = (float)phase / (float)rate;
            const int16_t v =
                (int16_t)(amplitude * sinf(2.0f * (float)M_PI * (float)hz * t));
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
            /* Wrap on a whole second so the phase stays continuous and the
             * float keeps its precision over a long tone. */
            if (++phase >= rate) {
                phase = 0;
            }
        }
        ret = audio_out_write(buf, frames_per_chunk * 2u * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "tone done");

cleanup:
    free(buf);
    audio_out_close();
    return ret;
}

/* ── play ────────────────────────────────────────────────────────────────── */

esp_err_t mpx_audio_play_wav(const char *rel_path)
{
    char full[AUDIO_PATH_MAX];
    wav_header_t header = {0};
    int16_t *file_buf = NULL;
    int16_t *out_buf = NULL;
    FILE *f = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(mpx_audio_have_speaker(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no speaker on this board");
    ESP_RETURN_ON_ERROR(audio_prepare_path(rel_path, full, sizeof(full)), TAG, "bad path");

    f = fopen(full, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", full);

    if (fread(&header, 1, sizeof(header), f) != sizeof(header) ||
            memcmp(header.riff, "RIFF", 4) != 0 ||
            memcmp(header.wave, "WAVE", 4) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "%s is not a WAV file", full);
        return ESP_ERR_INVALID_ARG;
    }
    if (header.format != 1 || header.bits_per_sample != 16 ||
            header.channels < 1 || header.channels > 2) {
        fclose(f);
        ESP_LOGE(TAG, "need 16-bit PCM, 1 or 2 channels; got format=%u bits=%u ch=%u",
                 header.format, header.bits_per_sample, header.channels);
        return ESP_ERR_INVALID_ARG;
    }

    /* The amplifier takes two I2S slots regardless of what the file holds, so
     * a mono file is written to both. */
    ret = audio_out_open(header.sample_rate);
    if (ret != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "cannot open I2S output: %s", esp_err_to_name(ret));
        return ret;
    }

    const uint32_t frames_per_chunk = AUDIO_CHUNK_BYTES / 4u;   /* stereo out */
    file_buf = audio_alloc(frames_per_chunk * header.channels * sizeof(int16_t));
    out_buf  = audio_alloc(frames_per_chunk * 2u * sizeof(int16_t));
    ESP_GOTO_ON_FALSE(file_buf && out_buf, ESP_ERR_NO_MEM, cleanup, TAG, "out of memory");

    ESP_LOGI(TAG, "playing %s: %" PRIu32 " Hz, %u channel(s), volume %d%%",
             full, header.sample_rate, header.channels, s_volume);

    for (;;) {
        const size_t want = frames_per_chunk * header.channels;
        const size_t got = fread(file_buf, sizeof(int16_t), want, f);
        if (got == 0) {
            break;
        }
        const size_t frames = got / header.channels;

        for (size_t i = 0; i < frames; i++) {
            const int32_t l = file_buf[i * header.channels];
            const int32_t r = (header.channels == 2) ? file_buf[i * 2 + 1] : l;
            /* Volume in the samples: the MAX98357A cannot do it in hardware.
             * int32 intermediate because a full-scale sample times 100 does
             * not fit in 16 bits. */
            out_buf[i * 2]     = (int16_t)(l * s_volume / 100);
            out_buf[i * 2 + 1] = (int16_t)(r * s_volume / 100);
        }

        ret = audio_out_write(out_buf, frames * 2u * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        if (got < want) {
            break;                      /* short read means end of file */
        }
    }
    ESP_LOGI(TAG, "playback done");

cleanup:
    if (f) {
        fclose(f);
    }
    free(file_buf);
    free(out_buf);
    audio_out_close();
    return ret;
}

#else  /* CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT */

/* No audio codec on this board. Everything answers honestly rather than
 * failing to link, so `audio info` still works and says why. */

esp_err_t mpx_audio_init(void)
{
    ESP_LOGI(TAG, "no audio codec on this board");
    return ESP_OK;
}

bool mpx_audio_have_mic(void)     { return false; }
bool mpx_audio_have_speaker(void) { return false; }

esp_err_t mpx_audio_record_wav(const char *rel_path, uint32_t seconds,
                               uint32_t sample_rate, uint8_t channels,
                               uint8_t pick)
{
    (void)rel_path; (void)seconds; (void)sample_rate; (void)channels; (void)pick;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_play_wav(const char *rel_path)
{
    (void)rel_path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_play_tone(uint32_t hz, uint32_t seconds)
{
    (void)hz; (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

static int s_volume_stub = 70;
void mpx_audio_set_volume(int percent) { s_volume_stub = percent; }
int  mpx_audio_get_volume(void)        { return s_volume_stub; }

static int s_gain_stub = 30;
void mpx_audio_set_mic_gain(int db) { s_gain_stub = db; }
int  mpx_audio_get_mic_gain(void)   { return s_gain_stub; }

esp_err_t mpx_audio_capture_start(uint32_t sample_rate, uint8_t channels, uint8_t pick)
{
    (void)sample_rate; (void)channels; (void)pick;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_capture_read(int16_t *mono, size_t frames)
{
    (void)mono; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

void mpx_audio_capture_stop(void) { }

esp_err_t mpx_audio_output_start(uint32_t sample_rate)
{
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mpx_audio_output_write(const int16_t *mono, size_t frames)
{
    (void)mono; (void)frames;
    return ESP_ERR_NOT_SUPPORTED;
}

void mpx_audio_output_stop(void) { }

#endif  /* CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT */
