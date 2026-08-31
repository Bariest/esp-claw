/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal interface to the drawn face. Everything here except
 * cap_display_face_screen_locked() takes the LVGL lock itself.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The face screen, building it on first call. Caller must already
 *         hold the display_service lock. NULL if it could not be built. */
lv_obj_t *cap_display_face_screen_locked(void);

/** @brief Whether `name` is a known expression. */
bool cap_display_face_is_emotion(const char *name);

/** @brief Write the known expression names, comma separated, into `out`.
 *  @return number of bytes written. */
size_t cap_display_face_emotion_names(char *out, size_t out_size);

/** @brief Animate the face to an expression. ESP_ERR_NOT_FOUND if unknown. */
esp_err_t cap_display_face_set_emotion(const char *name);

/** @brief Set the dim line under the eyes (network state). "" clears it. */
esp_err_t cap_display_face_set_status(const char *text);

#ifdef __cplusplus
}
#endif
