/* Renders the panel face on a host, using the same face.c the firmware runs, so
 * the layout can be checked without flashing. Writes one PPM per state.
 *
 *   cc -I../components/stick_s3_light -o facepreview facepreview.c \
 *      ../components/stick_s3_light/face.c ../components/stick_s3_light/font5x7.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"

static uint16_t frame[FACE_WIDTH * FACE_HEIGHT];

static void render(const face_state_t *st)
{
    const int band = 30;
    for (int y = 0; y < FACE_HEIGHT; y += band) {
        int lines = FACE_HEIGHT - y < band ? FACE_HEIGHT - y : band;
        face_draw_band(&frame[y * FACE_WIDTH], y, lines, st);
    }
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", FACE_WIDTH, FACE_HEIGHT);
    for (int i = 0; i < FACE_WIDTH * FACE_HEIGHT; i++) {
        /* face.c emits the panel's big-endian RGB565. */
        uint16_t be = frame[i];
        uint16_t c = (uint16_t)((be >> 8) | (be << 8));
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];

    struct {
        const char *name;
        face_state_t st;
    } states[] = {
        {"off", {false, 114, 0xff, 0x9d, 0x3d, FACE_NET_ONLINE, "v0.1.1-rc01-1bd27bd"}},
        {"min", {true, 1, 0xff, 0xb4, 0x5a, FACE_NET_ONLINE, "v0.1.1-rc01-1bd27bd"}},
        {"warm45", {true, 114, 0xff, 0x9d, 0x3d, FACE_NET_ONLINE, "v0.1.1-rc01-1bd27bd"}},
        {"cool100", {true, 254, 0xdb, 0xe9, 0xff, FACE_NET_ONLINE, "v0.1.1-rc01-1bd27bd"}},
        {"pairing72", {true, 183, 0xb0, 0x6c, 0xff, FACE_NET_PAIRING, "v0.1.1-rc01-1bd27bd"}},
        {"longver", {true, 183, 0xff, 0x9d, 0x3d, FACE_NET_ONLINE, "v0.10.12-rc123-1bd27bd"}},
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        render(&states[i].st);
        snprintf(path, sizeof(path), "%s/face-%s.ppm", dir, states[i].name);
        write_ppm(path);
        printf("%-10s level %3u -> %3u%%\n", states[i].name, states[i].st.level,
               face_level_percent(states[i].st.level));
    }
    return 0;
}
