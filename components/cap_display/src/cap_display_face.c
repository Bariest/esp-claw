/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The robot's face: two eyes, drawn.
 *
 * The reference animation for this is a 320x240 GIF -- two yellow rounded
 * rectangles on black, blinking twice over a 3.5 s loop. It is not stored as a
 * GIF. Every frame of it is two rectangles, so the honest representation is
 * two rectangles: LVGL objects whose height an animation drives. That is about
 * 2 KB of code and no assets, against roughly 150 KB of RAM for a decoded
 * frame buffer plus the file itself, and it buys something the GIF cannot
 * give -- the face becomes programmable. The agent changes its expression from
 * a chat message; a skill can make it wink.
 *
 * Geometry is measured from that GIF rather than guessed, so the drawn face
 * and the reference are the same picture:
 *
 *   eye            59 x 114 px, corner radius 8
 *   centres        (82, 120) and (236, 120) on a 320x240 panel
 *   colour         #FFE631 on black
 *   blink          ~110 ms closed, ~110 ms open, every 2.5-3.5 s
 *   squash         the eye widens ~15% as it closes, which is what makes a
 *                  blink read as a blink rather than as a shutter
 *
 * ── Why the face is the home screen ───────────────────────────────────────
 *
 * ESP-Claw's system_ui owns the panel by default: a launcher of tiles and a
 * task panel, both driven by touch. This board has no touch controller, so
 * that UI is not reachable on it -- there is no gesture to open the task
 * panel and no way to pick a tile. Holding a display_service session with the
 * face in it is therefore not taking the screen away from anything usable.
 *
 * What system_ui did provide that mattered is the network line: on a robot in
 * AP mode you need to be told the SSID to join, and the panel is the only
 * place that can tell you before you have joined it. So the face carries that
 * line itself, fed from main.c's Wi-Fi state callback.
 */

#include "cap_display_face.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"

#include "display_service.h"

static const char *TAG = "cap_display_face";

#define FACE_EYE_DX          77      /* eye centre offset from screen centre */
#define FACE_EYE_COLOR       0xFFE631
#define FACE_EYE_MIN_H       5       /* a fully closed eye is a line, not nothing */
#define FACE_BLINK_MS        110
#define FACE_BLINK_MIN_GAP   2200
#define FACE_BLINK_MAX_GAP   5200
#define FACE_EMOTION_MS      220
#define FACE_SQUASH_PCT      15      /* width gain at full close */
#define FACE_STATUS_MAX      64

/* ── Expressions ───────────────────────────────────────────────────────────
 *
 * An expression is nothing but the two eyes' size, corner radius and vertical
 * offset. That is a smaller vocabulary than a face has, and it is deliberately
 * so: these read from across a room, they interpolate cleanly from any state
 * to any other, and none of them needs a bitmap. "Angry" here is a low, hard,
 * squared-off eye rather than an angled brow -- a rotated rectangle would be
 * more literal and would cost a transform per frame for a difference nobody
 * standing two metres away can see.
 */

typedef struct {
    int16_t w;
    int16_t h;
    int16_t r;
    int16_t dy;
} face_eye_t;

typedef struct {
    const char *name;
    face_eye_t  left;
    face_eye_t  right;
    uint32_t    color;
    bool        blinks;
} face_emotion_t;

#define EYE_NEUTRAL { 59, 114, 8, 0 }

static const face_emotion_t s_emotions[] = {
    { "neutral",   EYE_NEUTRAL,          EYE_NEUTRAL,          FACE_EYE_COLOR, true  },
    { "happy",     { 59,  54, 26,  16 }, { 59,  54, 26,  16 }, FACE_EYE_COLOR, true  },
    { "excited",   { 66, 126, 14,   0 }, { 66, 126, 14,   0 }, FACE_EYE_COLOR, true  },
    { "sad",       { 59,  62, 26,  26 }, { 59,  62, 26,  26 }, FACE_EYE_COLOR, true  },
    { "sleepy",    { 64,  14,  7,  22 }, { 64,  14,  7,  22 }, 0xB59A18,       false },
    { "angry",     { 62,  64, 10, -10 }, { 62,  64, 10, -10 }, FACE_EYE_COLOR, true  },
    { "surprised", { 72, 132, 30,   0 }, { 72, 132, 30,   0 }, FACE_EYE_COLOR, false },
    { "love",      { 70, 120, 34,   0 }, { 70, 120, 34,   0 }, 0xFF4D8D,       true  },
    { "confused",  EYE_NEUTRAL,          { 59,  70, 18,  14 }, FACE_EYE_COLOR, true  },
    { "wink",      EYE_NEUTRAL,          { 64,  12,  6,   2 }, FACE_EYE_COLOR, false },
};

#define FACE_EMOTION_COUNT (sizeof(s_emotions) / sizeof(s_emotions[0]))

/* ── State ─────────────────────────────────────────────────────────────────
 *
 * Two independent inputs decide what the eyes look like at any instant: which
 * expression is showing (possibly mid-transition between two of them) and how
 * far through a blink we are. They are kept apart and combined in
 * face_render(), because a blink has to work the same whatever the expression
 * is -- folding the blink into the expression would mean re-deriving every
 * expression's closed form.
 */

static lv_obj_t   *s_screen;
static lv_obj_t   *s_eye[2];
static lv_obj_t   *s_status;
static lv_timer_t *s_blink_timer;

static face_eye_t  s_from[2];       /* expression we are animating away from */
static face_eye_t  s_to[2];         /* expression we are animating towards   */
static uint32_t    s_from_color;
static uint32_t    s_to_color;
static int32_t     s_morph = 1000;  /* 0 = at `from`, 1000 = at `to`         */
static int32_t     s_open  = 1000;  /* 1000 = eyes open, 0 = fully closed    */
static bool        s_blinks = true;
static uint32_t    s_background = 0x000000;   /* survives a rebuild of the screen */

static int32_t lerp(int32_t a, int32_t b, int32_t t)
{
    return a + (b - a) * t / 1000;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int32_t t)
{
    uint32_t r = (uint32_t)lerp((int32_t)((a >> 16) & 0xFF), (int32_t)((b >> 16) & 0xFF), t);
    uint32_t g = (uint32_t)lerp((int32_t)((a >> 8) & 0xFF), (int32_t)((b >> 8) & 0xFF), t);
    uint32_t bl = (uint32_t)lerp((int32_t)(a & 0xFF), (int32_t)(b & 0xFF), t);

    return (r << 16) | (g << 8) | bl;
}

/* Apply expression and blink to both eyes. Runs with the LVGL lock already
 * held -- animation and timer callbacks are called by LVGL itself. */
static void face_render(void)
{
    uint32_t color = lerp_color(s_from_color, s_to_color, s_morph);

    for (int i = 0; i < 2; i++) {
        int32_t w  = lerp(s_from[i].w,  s_to[i].w,  s_morph);
        int32_t h  = lerp(s_from[i].h,  s_to[i].h,  s_morph);
        int32_t r  = lerp(s_from[i].r,  s_to[i].r,  s_morph);
        int32_t dy = lerp(s_from[i].dy, s_to[i].dy, s_morph);
        int32_t closed = 1000 - s_open;

        /* The eye shortens towards its own centre and widens as it does. The
         * widening is what sells it: an eyelid squeezes the eye, it does not
         * merely crop it. */
        h = h * s_open / 1000;
        if (h < FACE_EYE_MIN_H) {
            h = FACE_EYE_MIN_H;
        }
        w = w + (w * FACE_SQUASH_PCT / 100) * closed / 1000;
        if (r > h / 2) {
            r = h / 2;   /* LVGL clamps this anyway; being explicit avoids a
                          * radius that visibly pops as the eye closes */
        }

        if (!s_eye[i]) {
            continue;
        }
        lv_obj_set_size(s_eye[i], (int32_t)w, (int32_t)h);
        lv_obj_set_style_radius(s_eye[i], (int32_t)r, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_eye[i], lv_color_hex(color), LV_PART_MAIN);
        lv_obj_align(s_eye[i], LV_ALIGN_CENTER,
                     i == 0 ? -FACE_EYE_DX : FACE_EYE_DX, (int32_t)dy);
    }
}

/* ── Animations ────────────────────────────────────────────────────────── */

static void anim_set_open(void *unused, int32_t value)
{
    (void)unused;
    s_open = value;
    face_render();
}

static void anim_set_morph(void *unused, int32_t value)
{
    (void)unused;
    s_morph = value;
    face_render();
}

static void blink_open_phase(lv_anim_t *closed_anim)
{
    lv_anim_t a;

    (void)closed_anim;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_eye[0]);
    lv_anim_set_exec_cb(&a, anim_set_open);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, FACE_BLINK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void blink_timer_cb(lv_timer_t *timer)
{
    lv_anim_t a;
    uint32_t gap;

    if (s_blinks && s_eye[0]) {
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_eye[0]);
        lv_anim_set_exec_cb(&a, anim_set_open);
        lv_anim_set_values(&a, 1000, 0);
        lv_anim_set_duration(&a, FACE_BLINK_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_completed_cb(&a, blink_open_phase);
        lv_anim_start(&a);
    }

    /* Reschedule with a fresh interval every time. A fixed period reads as a
     * metronome, which is the one thing a living face never does. */
    gap = FACE_BLINK_MIN_GAP +
          esp_random() % (FACE_BLINK_MAX_GAP - FACE_BLINK_MIN_GAP);
    lv_timer_set_period(timer, gap);
}

/* ── Building the screen ───────────────────────────────────────────────── */

/* Caller holds the LVGL lock. */
static esp_err_t face_build_locked(void)
{
    if (s_screen) {
        return ESP_OK;
    }

    s_screen = lv_obj_create(NULL);
    if (!s_screen) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        s_eye[i] = lv_obj_create(s_screen);
        if (!s_eye[i]) {
            return ESP_ERR_NO_MEM;
        }
        lv_obj_set_style_bg_opa(s_eye[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_eye[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_eye[i], 0, LV_PART_MAIN);
        lv_obj_remove_flag(s_eye[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    s_status = lv_label_create(s_screen);
    if (s_status) {
        /* Dim on purpose: it is a thing to read when you need it, not part of
         * the face. */
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x6B6B6B), LV_PART_MAIN);
        lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_status, lv_pct(96));
        lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_label_set_text(s_status, "");
    }

    s_from[0] = s_to[0] = s_emotions[0].left;
    s_from[1] = s_to[1] = s_emotions[0].right;
    s_from_color = s_to_color = s_emotions[0].color;
    s_morph = 1000;
    s_open = 1000;
    s_blinks = true;
    face_render();

    if (!s_blink_timer) {
        s_blink_timer = lv_timer_create(blink_timer_cb, FACE_BLINK_MIN_GAP, NULL);
    }
    return ESP_OK;
}

lv_obj_t *cap_display_face_screen_locked(void)
{
    if (face_build_locked() != ESP_OK) {
        return NULL;
    }
    return s_screen;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool cap_display_face_is_emotion(const char *name)
{
    for (size_t i = 0; i < FACE_EMOTION_COUNT; i++) {
        if (strcasecmp(name, s_emotions[i].name) == 0) {
            return true;
        }
    }
    return false;
}

size_t cap_display_face_emotion_names(char *out, size_t out_size)
{
    size_t at = 0;

    for (size_t i = 0; i < FACE_EMOTION_COUNT && at < out_size; i++) {
        at += (size_t)snprintf(out + at, out_size - at, "%s%s",
                               i ? ", " : "", s_emotions[i].name);
    }
    return at;
}

esp_err_t cap_display_face_set_emotion(const char *name)
{
    const face_emotion_t *target = NULL;

    for (size_t i = 0; i < FACE_EMOTION_COUNT; i++) {
        if (strcasecmp(name, s_emotions[i].name) == 0) {
            target = &s_emotions[i];
            break;
        }
    }
    if (!target) {
        return ESP_ERR_NOT_FOUND;
    }
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() != ESP_OK) {
        display_service_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Morph from wherever the eyes are *now*, not from the previous
     * expression's resting shape: interrupting a transition half way through
     * must not snap. */
    for (int i = 0; i < 2; i++) {
        s_from[i].w  = (int16_t)lerp(s_from[i].w,  s_to[i].w,  s_morph);
        s_from[i].h  = (int16_t)lerp(s_from[i].h,  s_to[i].h,  s_morph);
        s_from[i].r  = (int16_t)lerp(s_from[i].r,  s_to[i].r,  s_morph);
        s_from[i].dy = (int16_t)lerp(s_from[i].dy, s_to[i].dy, s_morph);
    }
    s_from_color = lerp_color(s_from_color, s_to_color, s_morph);
    s_to[0] = target->left;
    s_to[1] = target->right;
    s_to_color = target->color;
    s_blinks = target->blinks;
    s_morph = 0;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_eye[0]);
    lv_anim_set_exec_cb(&a, anim_set_morph);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, FACE_EMOTION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    display_service_unlock();
    ESP_LOGI(TAG, "expression: %s", target->name);
    return ESP_OK;
}

esp_err_t cap_display_face_set_status(const char *text)
{
    char buf[FACE_STATUS_MAX];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() == ESP_OK && s_status) {
        lv_label_set_text(s_status, buf);
    }
    display_service_unlock();
    return ESP_OK;
}

esp_err_t cap_display_face_set_colors(int32_t background_rgb, int32_t eyes_rgb)
{
    if (display_service_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (face_build_locked() != ESP_OK) {
        display_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (background_rgb >= 0) {
        s_background = (uint32_t)background_rgb & 0xFFFFFF;
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_background), LV_PART_MAIN);
    }
    if (eyes_rgb >= 0) {
        /* Jump straight there rather than morphing: a colour request reads
         * as "now", and the running blink/morph animation keeps working on
         * the shapes regardless. */
        s_from_color = s_to_color = (uint32_t)eyes_rgb & 0xFFFFFF;
        face_render();
    }
    display_service_unlock();
    ESP_LOGI(TAG, "colours: background #%06X eyes %s", (unsigned)s_background,
             eyes_rgb >= 0 ? "set" : "unchanged");
    return ESP_OK;
}
