/*
 * SPDX-FileCopyrightText: 2026 Jack Gu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The face: what the panel shows. Pure drawing, no hardware -- it renders a
 * horizontal band of the screen into a caller-supplied buffer, which lets
 * tools/facepreview.c render the same pixels on a host. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FACE_WIDTH 135
#define FACE_HEIGHT 240

/* The four bands. A state change dirties whole bands, and because they are
 * contiguous the lowest and highest dirty band bound the repaint. */
#define FACE_BAND_STATUS_Y0 0
#define FACE_BAND_STATUS_Y1 18
#define FACE_BAND_BULB_Y0 18
#define FACE_BAND_BULB_Y1 154
#define FACE_BAND_LEVEL_Y0 154
#define FACE_BAND_LEVEL_Y1 200
#define FACE_BAND_FOOTER_Y0 200
#define FACE_BAND_FOOTER_Y1 240

/* The build string, e.g. "v0.1.1-rc01-1bd27bd". Drawn as two lines split at the
 * commit, so 21 is the cap per line rather than for the whole string. */
#define FACE_MAX_CHARS 31

typedef enum {
    FACE_NET_OFFLINE = 0,
    FACE_NET_PAIRING,
    FACE_NET_ONLINE,
} face_net_t;

typedef struct {
    bool power;
    uint8_t level; /* Matter CurrentLevel, 1-254 */
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    face_net_t net;
    const char *line_fw; /* the build string, e.g. "v0.1.1-rc01-1bd27bd" */
} face_state_t;

/** Percentage shown for a raw Matter level, floored at 1. */
uint8_t face_level_percent(uint8_t level);

/** Draw rows [y, y + lines) of the face into buf.
 *
 * buf holds lines * FACE_WIDTH pixels in the panel's big-endian RGB565. When
 * the light is off the band comes back black -- the face is only composed while
 * it is on.
 */
void face_draw_band(uint16_t *buf, int y, int lines, const face_state_t *state);
