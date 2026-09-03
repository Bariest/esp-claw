/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Register the "cap_display" capability group. Idempotent. */
esp_err_t cap_display_register_group(void);

/** @brief Put the robot's face on the panel and start it blinking.
 *
 *  Call once after the display service is up (that is, after
 *  app_claw_ui_start()). The face becomes the resting screen: this board
 *  has no touch controller, so ESP-Claw's launcher home screen cannot be
 *  operated on it and holding the panel costs nothing.
 *
 *  A board with no display is not an error; this returns ESP_OK. */
/* True when a panel was found and a display session is open. False on a board
 * with no screen, where every display call is a no-op -- check this before
 * doing work whose only purpose is to put something on the panel. */
bool cap_display_available(void);

esp_err_t cap_display_face_start(void);

/** @brief Update the dim line under the eyes.
 *
 *  In AP mode the panel is the only thing that can tell someone which
 *  network to join, since they have not joined it yet. */
void cap_display_face_set_network(bool sta_connected, const char *ap_ssid, const char *ip);

/** @brief Draw a bring-up test pattern, hold it, then restore the face.
 *
 *  Corner labels, RGB bars and an edge frame -- enough to judge mirror_x,
 *  swap_xy, invert_color and any panel offset by eye, which is the only way
 *  those can be checked. Blocks for @p hold_ms.
 *
 *  @return ESP_ERR_NOT_SUPPORTED when the board has no panel. */
esp_err_t cap_display_show_test_pattern(uint32_t hold_ms);
#ifdef __cplusplus
}
#endif
