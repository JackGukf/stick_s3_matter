/*
 * SPDX-FileCopyrightText: 2026 Jack Gu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the node stands on the fabric, shown in the status bar. */
typedef enum {
    STICK_S3_NET_OFFLINE = 0, /* no fabric, or commissioned but no network */
    STICK_S3_NET_PAIRING,     /* commissioning window open */
    STICK_S3_NET_ONLINE,      /* on a fabric and reachable */
} stick_s3_net_state_t;

/** Initialize the StickS3 LCD as the light.
 *
 * Powers up the LCD rail through the M5PM1 PMIC, brings up the ST7789P3 panel
 * and the backlight, and starts the render task. The panel stays black until
 * the first set_power(true).
 */
esp_err_t stick_s3_light_init(void);

/** Turn the light on or off.
 *
 * Off blanks the panel and drops the backlight; on redraws the whole face.
 */
esp_err_t stick_s3_light_set_power(bool power);

/** Set the dim level, 1-254, as carried by LevelControl's CurrentLevel.
 *
 * The level drives the bulb fill and the percentage readout. It deliberately
 * does not touch the backlight, which stays at full while the light is on.
 */
esp_err_t stick_s3_light_set_level(uint8_t level);

/** Set hue, 0-360. Tints the bulb. */
esp_err_t stick_s3_light_set_hue(uint16_t hue);

/** Set saturation, 0-100. Tints the bulb. */
esp_err_t stick_s3_light_set_saturation(uint8_t saturation);

/** Set color temperature in kelvin. Tints the bulb. */
esp_err_t stick_s3_light_set_temperature(uint32_t temperature);

/** Set color from CIE xy chromaticity, as sent by the ColorControl cluster. */
esp_err_t stick_s3_light_set_xy(uint16_t x, uint16_t y);

/** Set the network state shown in the status bar. */
esp_err_t stick_s3_light_set_net_state(stick_s3_net_state_t state);

/** Set the firmware build shown along the bottom, e.g. "fw v0.1.1".
 *
 * Truncated to what fits the 135 px panel and copied, so the caller keeps
 * ownership.
 */
esp_err_t stick_s3_light_set_firmware(const char *firmware);

#ifdef __cplusplus
}
#endif
