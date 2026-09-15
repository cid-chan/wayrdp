// wayland.h — the compositor half, behind one interface.
//
// Everything that knows a Wayland protocol lives on the other side of this
// header. The RDP half asks for a frame and pushes input; it never learns that
// the screen arrives by a copy the compositor agreed to make, or that a
// keystroke needs a keymap sent over a file descriptor first.

#ifndef WAYRDP_WAYLAND_H
#define WAYRDP_WAYLAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wr_rect {
    int32_t x, y, width, height;
};

// Damage is reported per frame and is the whole point of the protocol: a still
// screen sends nothing, and a moving cursor sends a cursor-sized rectangle.
#define WR_MAX_DAMAGE 64

struct wr_frame {
    const uint8_t *pixels;
    uint32_t width, height;
    size_t stride;
    uint32_t format;                 // wl_shm format (a DRM fourcc)
    int bytes_per_pixel;

    struct wr_rect damage[WR_MAX_DAMAGE];
    int damage_count;
    bool damage_overflowed;          // more rectangles than we kept: repaint all
};

// One pixel to R,G,B. Format knowledge lives here and nowhere else: the RDP
// encoder needs the same answer the probe does, and two copies drift.
void wr_read_rgb(const uint8_t *pixel, uint32_t format, uint8_t out[3]);

// True when the format's bytes are already B,G,R in memory (as RemoteFX wants),
// so the RDP side can skip exchanging the red and blue halves when it repacks.
bool wr_format_is_bgr(uint32_t format);

struct wr_wayland;

// Connect, bind, and say plainly which piece is missing when one is: each is a
// compositor plugin somebody can turn on, and "failed to start" helps nobody.
struct wr_wayland *wr_open(const char **error);
void wr_close(struct wr_wayland *w);

// The display fd, so a server can poll it alongside its own sockets.
int wr_fd(const struct wr_wayland *w);
bool wr_flush(struct wr_wayland *w);
bool wr_dispatch_pending(struct wr_wayland *w);

// The Wayland connection is broken beyond recovery; stop and reconnect.
bool wr_fatal(const struct wr_wayland *w);

// --- capture ---------------------------------------------------------------

// Negotiates size and format. Fails if the compositor offers no format this
// understands, naming the fourcc it did offer.
bool wr_capture_open(struct wr_wayland *w, const char **error);

uint32_t wr_width(const struct wr_wayland *w);
uint32_t wr_height(const struct wr_wayland *w);

// Grab the screen as it is, now.
//
// The streaming path answers only when something changes, which is the right
// default and useless for the first paint: a client that has just connected has
// an empty window and no damage is coming. This copies on demand instead, and
// is also the answer to a client asking for a refresh.
const struct wr_frame *wr_capture_now(struct wr_wayland *w, int timeout_ms);

// Wait for one frame, up to timeout_ms.
//
// Returns NULL on timeout, which is not an error: the compositor answers a
// capture request when the screen changes, so an idle desktop produces nothing
// and that is exactly the bandwidth a remote desktop should use.
const struct wr_frame *wr_capture_frame(struct wr_wayland *w, int timeout_ms);

// --- input -----------------------------------------------------------------
// Coordinates are in output pixels; buttons and keys are Linux input codes,
// which is what RDP scancodes translate into.

bool wr_input_open(struct wr_wayland *w, const char **error);
void wr_pointer_motion(struct wr_wayland *w, uint32_t x, uint32_t y);
void wr_pointer_button(struct wr_wayland *w, uint32_t button, bool pressed);
void wr_pointer_axis(struct wr_wayland *w, bool horizontal, double value);
void wr_keyboard_key(struct wr_wayland *w, uint32_t keycode, bool pressed);
void wr_keyboard_modifiers(struct wr_wayland *w, uint32_t depressed, uint32_t locked);

#endif
