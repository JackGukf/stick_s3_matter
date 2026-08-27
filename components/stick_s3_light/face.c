/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Everything is drawn with flat spans and a 5x7 bitmap font: no blur, no
 * gradients, no anti-aliasing, so the circle is a midpoint walk and the whole
 * face costs a few hundred span writes. That is also why there is no graphics
 * library here -- LVGL would add ~250 kB to an image that has to fit a 1.92 MB
 * OTA slot, for a screen that draws one bulb.
 *
 * Rendering is banded rather than buffered: a full 135x240 framebuffer is 63 kB
 * and this target has no PSRAM, so the caller hands over one band at a time and
 * every primitive clips itself to it. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "face.h"
#include "font5x7.h"

/* Layout, in panel pixels. */
#define UI_STATUS_TEXT_Y 4
#define UI_RULE_TOP_Y 17
#define UI_BULB_CX 67
#define UI_BULB_CY 78
#define UI_BULB_R 40
#define UI_BULB_RING 3
#define UI_GLASS_TOP (UI_BULB_CY - UI_BULB_R) /* 38 */
#define UI_GLASS_BOT (UI_BULB_CY + UI_BULB_R) /* 118 */
#define UI_NECK_TOP 112
#define UI_NECK_BOT 128
#define UI_NECK_TOP_L 50
#define UI_NECK_TOP_R 84
#define UI_NECK_BOT_L 53
#define UI_NECK_BOT_R 81
#define UI_BASE_TOP 128
#define UI_BASE_BOT 148
#define UI_PCT_SCALE 5
#define UI_PCT_TOP 158 /* 7 rows * 5 = 35 px tall */
#define UI_SIGN_SCALE 2
#define UI_SIGN_GAP 4
#define UI_RULE_BOT_Y 199
#define UI_FW_TOP 203  /* first build line; second sits UI_FW_STEP below */
#define UI_FW_STEP 20
#define UI_FW_SCALE 2
#define UI_FW_SLOT (FONT5X7_ROWS * UI_FW_SCALE) /* height of one build line */
#define UI_MARGIN 6
#define UI_DOT_R 2

/* Palette. The bulb takes the light's own color; everything else is fixed so
 * the readout stays legible whatever color was commanded. */
#define C_BG 0, 0, 0
#define C_RULE 0x24, 0x28, 0x2c
#define C_GLASS_DIM 0x1b, 0x1b, 0x1e
#define C_BASE 0x3a, 0x3f, 0x45
#define C_LABEL 0x9a, 0xa4, 0xab
#define C_PCT 0xec, 0xef, 0xef
#define C_MUTED 0x8a, 0x95, 0x9c
#define C_FOOT 0xc8, 0xce, 0xd2
#define C_ONLINE 0x4f, 0xb8, 0x9f
#define C_PAIRING 0xe8, 0xa5, 0x45

/* The band being drawn. Not reentrant, and does not need to be: rendering
 * happens only on the driver's render task. */
static uint16_t *s_buf;
static int s_y0;
static int s_lines;

/* The panel takes big-endian RGB565. */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static void band_clear(uint16_t color)
{
    for (int i = 0; i < s_lines * FACE_WIDTH; i++) {
        s_buf[i] = color;
    }
}

static void hline(int x0, int x1, int y, uint16_t color)
{
    int row = y - s_y0;
    if (row < 0 || row >= s_lines) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > FACE_WIDTH - 1) {
        x1 = FACE_WIDTH - 1;
    }
    uint16_t *p = &s_buf[row * FACE_WIDTH];
    for (int x = x0; x <= x1; x++) {
        p[x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int i = 0; i < h; i++) {
        hline(x, x + w - 1, y + i, color);
    }
}

/* Half-width of a circle of radius r at vertical distance dy from its center. */
static inline int chord(int r, int dy)
{
    int d2 = r * r - dy * dy;
    return d2 <= 0 ? 0 : (int)sqrtf((float)d2);
}

/* Filled circle, restricted to rows [y_top, y_bot]. Used for the whole glass
 * and again for the level fill, which is the same disc cut off at a height. */
static void disc(int cx, int cy, int r, int y_top, int y_bot, uint16_t color)
{
    if (y_top < cy - r) {
        y_top = cy - r;
    }
    if (y_bot > cy + r) {
        y_bot = cy + r;
    }
    for (int y = y_top; y <= y_bot; y++) {
        int h = chord(r, y - cy);
        hline(cx - h, cx + h, y, color);
    }
}

/* Annulus of the given thickness -- the glass outline. */
static void ring(int cx, int cy, int r, int thickness, uint16_t color)
{
    int inner = r - thickness;
    for (int y = cy - r; y <= cy + r; y++) {
        int dy = y - cy;
        int ho = chord(r, dy);
        if (dy <= -inner || dy >= inner) {
            hline(cx - ho, cx + ho, y, color);
            continue;
        }
        int hi = chord(inner, dy);
        hline(cx - ho, cx - hi, y, color);
        hline(cx + hi, cx + ho, y, color);
    }
}

static void draw_glyph(int x, int y, char ch, int scale, uint16_t color)
{
    if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) {
        ch = '?';
    }
    const uint8_t *rows = font5x7[(int)ch - FONT5X7_FIRST];
    for (int row = 0; row < FONT5X7_ROWS; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < FONT5X7_COLS; col++) {
            if (bits & (1 << (FONT5X7_COLS - 1 - col))) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
    if (!s) {
        return;
    }
    for (; *s; s++) {
        draw_glyph(x, y, *s, scale, color);
        x += FONT5X7_ADVANCE * scale;
    }
}

/* Ink width: the trailing tracking column of the last glyph does not count. */
static int text_width(const char *s, int scale)
{
    size_t n = s ? strlen(s) : 0;
    return n == 0 ? 0 : (int)n * FONT5X7_ADVANCE * scale - scale;
}

uint8_t face_level_percent(uint8_t level)
{
    /* Matter level 1 is 0.4%, which would read as 0 on a light that is on. */
    uint32_t pct = ((uint32_t)level * 100 + 127) / 254;
    return pct < 1 ? 1 : (uint8_t)pct;
}

static void draw_status_bar(const face_state_t *st)
{
    const char *label;
    uint16_t color;
    switch (st->net) {
    case FACE_NET_ONLINE:
        label = "ONLINE";
        color = rgb565(C_ONLINE);
        break;
    case FACE_NET_PAIRING:
        label = "PAIRING";
        color = rgb565(C_PAIRING);
        break;
    default:
        label = "OFFLINE";
        color = rgb565(C_MUTED);
        break;
    }

    draw_text(UI_MARGIN, UI_STATUS_TEXT_Y, "MATTER", 1, rgb565(C_LABEL));

    int x = FACE_WIDTH - UI_MARGIN - text_width(label, 1);
    draw_text(x, UI_STATUS_TEXT_Y, label, 1, color);
    disc(x - UI_MARGIN, UI_STATUS_TEXT_Y + 3, UI_DOT_R, 0, FACE_HEIGHT, color);

    hline(0, FACE_WIDTH - 1, UI_RULE_TOP_Y, rgb565(C_RULE));
}

static void draw_bulb(const face_state_t *st)
{
    uint16_t tint = rgb565(st->red, st->green, st->blue);
    uint16_t dim = rgb565(C_GLASS_DIM);

    /* Glass: unlit behind, filled from the bottom up to the level. */
    disc(UI_BULB_CX, UI_BULB_CY, UI_BULB_R, UI_GLASS_TOP, UI_GLASS_BOT, dim);

    /* The fill is measured against the glass the neck does not cover: below
     * UI_NECK_TOP the neck paints over it, so a level measured to the bottom of
     * the circle would leave 1% invisible. */
    int span = UI_NECK_TOP - UI_GLASS_TOP;
    int fill = (span * face_level_percent(st->level) + 99) / 100; /* round up */
    if (fill < 3) {
        fill = 3; /* a sliver, so 1% never reads as an empty bulb */
    }
    disc(UI_BULB_CX, UI_BULB_CY, UI_BULB_R, UI_NECK_TOP - fill, UI_GLASS_BOT, tint);
    ring(UI_BULB_CX, UI_BULB_CY, UI_BULB_R, UI_BULB_RING, tint);

    /* Neck: a trapezoid over the bottom of the glass, in the same grey as the
     * base so the stem reads as one solid piece, with the tint carried down its
     * slanted edges so the bulb stays one object. */
    uint16_t base = rgb565(C_BASE);
    int height = UI_NECK_BOT - UI_NECK_TOP;
    for (int y = UI_NECK_TOP; y < UI_NECK_BOT; y++) {
        int t = y - UI_NECK_TOP;
        int xl = UI_NECK_TOP_L + (UI_NECK_BOT_L - UI_NECK_TOP_L) * t / height;
        int xr = UI_NECK_TOP_R - (UI_NECK_TOP_R - UI_NECK_BOT_R) * t / height;
        hline(xl, xr, y, base);
        hline(xl, xl + UI_BULB_RING - 1, y, tint);
        hline(xr - UI_BULB_RING + 1, xr, y, tint);
    }

    /* Screw base. */
    int base_w = UI_NECK_BOT_R - UI_NECK_BOT_L + 1;
    fill_rect(UI_NECK_BOT_L, UI_BASE_TOP, base_w, UI_BASE_BOT - UI_BASE_TOP, base);
}

static void draw_level(const face_state_t *st)
{
    char pct[4];
    snprintf(pct, sizeof(pct), "%u", face_level_percent(st->level));

    /* Percentage and sign are centered as one unit, the sign sitting on the
     * digits' baseline. */
    int digits_w = text_width(pct, UI_PCT_SCALE);
    int sign_w = text_width("%", UI_SIGN_SCALE);
    int x = (FACE_WIDTH - (digits_w + UI_SIGN_GAP + sign_w)) / 2;
    int sign_y = UI_PCT_TOP + FONT5X7_ROWS * (UI_PCT_SCALE - UI_SIGN_SCALE);

    draw_text(x, UI_PCT_TOP, pct, UI_PCT_SCALE, rgb565(C_PCT));
    draw_text(x + digits_w + UI_SIGN_GAP, sign_y, "%", UI_SIGN_SCALE, rgb565(C_LABEL));

    hline(0, FACE_WIDTH - 1, UI_RULE_BOT_Y, rgb565(C_RULE));
}

/* One build line, drawn as large as it fits and centered in its slot. */
static void draw_footer_line(const char *text, int slot_top)
{
    int scale = UI_FW_SCALE;
    if (text_width(text, scale) > FACE_WIDTH - 4) {
        scale = 1;
    }
    int x = (FACE_WIDTH - text_width(text, scale)) / 2;
    int y = slot_top + (UI_FW_SLOT - FONT5X7_ROWS * scale) / 2;

    draw_text(x, y, text, scale, rgb565(C_FOOT));
}

/* The build string, e.g. "v0.1.1-rc01-1bd27bd". Split at the last '-' so the
 * commit gets its own line: 19 characters on one line would have to drop to
 * single size to fit 135 px, while both halves clear it at double size. */
static void draw_footer(const face_state_t *st)
{
    const char *build = st->line_fw;
    if (!build || !*build) {
        return;
    }

    const char *split = strrchr(build, '-');
    if (!split || split == build || !split[1]) {
        /* No commit to peel off: one line, centered across both slots. */
        draw_footer_line(build, UI_FW_TOP + UI_FW_STEP / 2);
        return;
    }

    char head[FACE_MAX_CHARS + 1];
    size_t n = (size_t)(split - build);
    if (n > FACE_MAX_CHARS) {
        n = FACE_MAX_CHARS;
    }
    memcpy(head, build, n);
    head[n] = '\0';

    draw_footer_line(head, UI_FW_TOP);
    draw_footer_line(split + 1, UI_FW_TOP + UI_FW_STEP);
}

void face_draw_band(uint16_t *buf, int y, int lines, const face_state_t *state)
{
    s_buf = buf;
    s_y0 = y;
    s_lines = lines;

    band_clear(rgb565(C_BG));
    if (!state->power) {
        return; /* off is a dark panel */
    }

    draw_status_bar(state);
    draw_bulb(state);
    draw_level(state);
    draw_footer(state);
}
