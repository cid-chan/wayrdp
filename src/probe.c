// probe.c — does this compositor support being remoted, and prove it.
//
// Run it inside the session you want to serve. It captures a frame and writes
// it out, then moves the pointer and taps a key. Anything wayrdp needs that the
// compositor does not offer is named here, before a client is ever pointed at
// the port.

#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wayland.h"

static void fourcc(uint32_t f, char out[5]) {
    out[0] = (char)(f & 0xff);        out[1] = (char)((f >> 8) & 0xff);
    out[2] = (char)((f >> 16) & 0xff); out[3] = (char)((f >> 24) & 0xff);
    out[4] = 0;
}

static bool write_ppm(const struct wr_frame *f, const char *path) {
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    fprintf(out, "P6\n%u %u\n255\n", f->width, f->height);
    for (uint32_t y = 0; y < f->height; y++) {
        const uint8_t *row = f->pixels + (size_t)y * f->stride;
        for (uint32_t x = 0; x < f->width; x++) {
            uint8_t rgb[3];
            wr_read_rgb(row + (size_t)x * f->bytes_per_pixel, f->format, rgb);
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    return true;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *path = argc > 1 ? argv[1] : "frame.ppm";
    const char *error = NULL;

    struct wr_wayland *w = wr_open(&error);
    if (!w) { fprintf(stderr, "wayrdp: %s\n", error); return 1; }

    if (!wr_capture_open(w, &error)) { fprintf(stderr, "wayrdp: %s\n", error); return 1; }

    char name[5];
    printf("capture: %ux%u\n", wr_width(w), wr_height(w));

    if (!wr_input_open(w, &error)) { fprintf(stderr, "wayrdp: %s\n", error); return 1; }
    printf("input:   virtual pointer and keyboard created\n");

    // Move first, then capture: a still screen answers a capture request with
    // silence, so the motion is both the test and the damage that produces a
    // frame at all.
    wr_pointer_motion(w, wr_width(w) / 2, wr_height(w) / 2);
    wr_pointer_motion(w, wr_width(w) / 2 + 40, wr_height(w) / 2 + 40);

    const struct wr_frame *f = wr_capture_frame(w, 5000);
    if (!f) {
        fprintf(stderr, "wayrdp: no frame in 5s. Nothing on screen changed, which is "
                        "the protocol working -- or the output is asleep.\n");
        return 2;
    }

    fourcc(f->format, name);
    printf("frame:   %ux%u %s, %d bytes per pixel, %d damage rect%s%s\n",
           f->width, f->height, name, f->bytes_per_pixel, f->damage_count,
           f->damage_count == 1 ? "" : "s",
           f->damage_overflowed ? " (overflowed)" : "");
    for (int i = 0; i < f->damage_count && i < 4; i++)
        printf("           %dx%d at %d,%d\n", f->damage[i].width, f->damage[i].height,
               f->damage[i].x, f->damage[i].y);

    if (!write_ppm(f, path)) { fprintf(stderr, "wayrdp: could not write %s\n", path); return 1; }
    printf("wrote:   %s\n", path);

    // A keystroke nothing can act on: the compositor either accepts the key or
    // rejects the virtual keyboard, and either way nothing is typed into a
    // document by a probe.
    wr_keyboard_key(w, KEY_LEFTSHIFT, true);
    wr_keyboard_key(w, KEY_LEFTSHIFT, false);
    printf("keys:    shift press and release accepted\n");

    // --binding asks the harder question: does a *combination* from a virtual
    // keyboard reach the compositor's own shortcuts? A remote desktop where
    // the keys type but the shortcuts do not is half a desktop.
    // --type sends keys whose character differs between layouts: on a US
    // keymap these are ; ' ], on br-abnt2 they are the Brazilian ones. What
    // comes out the other end says which keymap the compositor applied.
    if (argc > 2 && !strcmp(argv[2], "--type")) {
        // Enter last, or cat holds the line in its buffer and the test reads
        // an empty file and calls it a failure.
        const uint32_t keys[] = { KEY_A, KEY_B, KEY_C, KEY_ENTER };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            wr_keyboard_key(w, keys[i], true);
            usleep(40000);
            wr_keyboard_key(w, keys[i], false);
            usleep(60000);
        }
        printf("typed:   a b c enter\n");
        usleep(400000);
    }

    if (argc > 2 && !strcmp(argv[2], "--binding")) {
        wr_keyboard_key(w, KEY_LEFTMETA, true);
        wr_keyboard_key(w, KEY_W, true);
        usleep(120000);
        wr_keyboard_key(w, KEY_W, false);
        wr_keyboard_key(w, KEY_LEFTMETA, false);
        printf("binding: sent SUPER+W\n");
        usleep(400000);
    }

    wr_close(w);
    return 0;
}
