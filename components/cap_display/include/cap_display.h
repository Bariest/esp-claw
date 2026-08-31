/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Register the "cap_display" capability group. Idempotent. */
esp_err_t cap_display_register_group(void);
#ifdef __cplusplus
}
#endif
