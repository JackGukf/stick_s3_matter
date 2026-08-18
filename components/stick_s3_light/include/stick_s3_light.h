/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the StickS3 LCD as the light.
 *
 * Powers up the LCD rail through the M5PM1 PMIC, brings up the ST7789P3 panel
 * and the backlight. The panel stays black until the first set_power(true).
 */
esp_err_t stick_s3_light_init(void);

/** Turn the light on or off. */
esp_err_t stick_s3_light_set_power(bool power);

/** Set brightness, 0-100. Driven by the backlight PWM. */
esp_err_t stick_s3_light_set_brightness(uint8_t brightness);

/** Set hue, 0-360. */
esp_err_t stick_s3_light_set_hue(uint16_t hue);

/** Set saturation, 0-100. */
esp_err_t stick_s3_light_set_saturation(uint8_t saturation);

/** Set color temperature in kelvin. */
esp_err_t stick_s3_light_set_temperature(uint32_t temperature);

/** Set color from CIE xy chromaticity, as sent by the ColorControl cluster. */
esp_err_t stick_s3_light_set_xy(uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
