// Exercise the capture interface against a deterministic Wayland compositor.
#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "wlr-screencopy-unstable-v1-server-protocol.h"
#include "ext-image-capture-source-v1-server-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include "wayland.h"

struct observed {
    atomic_uint captures, copies, cancels, commands;
};
struct fixture {
    struct wl_display *display;
    struct wl_global *output;
    struct wl_resource *pending, *buffer;
    struct observed *seen;
    uint32_t width, height, format, color;
    bool describe;
};

static int64_t milliseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void resource_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    wl_resource_destroy(r);
}

static void frame_destroy(struct wl_resource *r) {
    struct fixture *f = wl_resource_get_user_data(r);
    if (f->pending == r) {
        f->pending = f->buffer = NULL;
        atomic_fetch_add(&f->seen->cancels, 1);
    }
}

static void finish_copy(struct fixture *f, bool damage) {
    if (!f->pending || !f->buffer) return;
    struct wl_shm_buffer *b = wl_shm_buffer_get(f->buffer);
    assert(b);
    assert((uint32_t)wl_shm_buffer_get_width(b) == f->width);
    assert((uint32_t)wl_shm_buffer_get_height(b) == f->height);
    assert((uint32_t)wl_shm_buffer_get_stride(b) == f->width * 4);
    assert(wl_shm_buffer_get_format(b) == f->format);
    wl_shm_buffer_begin_access(b);
    uint32_t *pixels = wl_shm_buffer_get_data(b);
    for (uint32_t i = 0; i < f->width * f->height; i++) pixels[i] = f->color;
    wl_shm_buffer_end_access(b);
    if (damage)
        zwlr_screencopy_frame_v1_send_damage(f->pending, 0, 0, f->width, f->height);
    zwlr_screencopy_frame_v1_send_ready(f->pending, 0, 0, 0);
    f->pending = f->buffer = NULL;
}

static void copy_frame(struct wl_client *c, struct wl_resource *r, struct wl_resource *b) {
    (void)c;
    struct fixture *f = wl_resource_get_user_data(r);
    f->pending = r;
    f->buffer = b;
    atomic_fetch_add(&f->seen->copies, 1);
    finish_copy(f, false);
}

static void copy_damage(struct wl_client *c, struct wl_resource *r, struct wl_resource *b) {
    (void)c;
    struct fixture *f = wl_resource_get_user_data(r);
    f->pending = r;
    f->buffer = b;
    atomic_fetch_add(&f->seen->copies, 1);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
    .copy = copy_frame, .destroy = resource_destroy, .copy_with_damage = copy_damage,
};

static void capture_output(struct wl_client *c, struct wl_resource *r, uint32_t id,
                           int32_t cursor, struct wl_resource *output) {
    (void)cursor; (void)output;
    struct fixture *f = wl_resource_get_user_data(r);
    assert(!f->pending);
    struct wl_resource *frame = wl_resource_create(c, &zwlr_screencopy_frame_v1_interface, 3, id);
    wl_resource_set_implementation(frame, &frame_impl, f, frame_destroy);
    f->pending = frame;
    atomic_fetch_add(&f->seen->captures, 1);
    if (f->describe) {
        zwlr_screencopy_frame_v1_send_buffer(frame, f->format, f->width, f->height, f->width * 4);
        zwlr_screencopy_frame_v1_send_buffer_done(frame);
    }
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
    .capture_output = capture_output, .destroy = resource_destroy,
};

static void bind_manager(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zwlr_screencopy_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

static void bind_output(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, version, id);
    wl_resource_set_implementation(r, NULL, NULL, NULL);
}

static void bind_seat(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, version, id);
    wl_resource_set_implementation(r, NULL, NULL, NULL);
}


static const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = resource_destroy,
};
static void create_source(struct wl_client *c, struct wl_resource *r, uint32_t id,
                          struct wl_resource *output) {
    (void)output;
    struct wl_resource *source = wl_resource_create(c, &ext_image_capture_source_v1_interface, 1, id);
    wl_resource_set_implementation(source, &source_impl, wl_resource_get_user_data(r), NULL);
}
static const struct ext_output_image_capture_source_manager_v1_interface sources_impl = {
    .create_source = create_source, .destroy = resource_destroy,
};
static void bind_sources(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &ext_output_image_capture_source_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &sources_impl, data, NULL);
}
static void ext_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer) {
    (void)c;
    struct fixture *f = wl_resource_get_user_data(r);
    f->buffer = buffer;
}
static void ext_damage(struct wl_client *c, struct wl_resource *r,
                       int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)c; (void)r; (void)x; (void)y; (void)width; (void)height;
}
static void ext_capture(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct fixture *f = wl_resource_get_user_data(r);
    struct wl_shm_buffer *b = wl_shm_buffer_get(f->buffer);
    assert(b);
    wl_shm_buffer_begin_access(b);
    uint32_t *pixels = wl_shm_buffer_get_data(b);
    for (uint32_t i = 0; i < f->width * f->height; i++) pixels[i] = f->color;
    wl_shm_buffer_end_access(b);
    f->buffer = NULL;
    ext_image_copy_capture_frame_v1_send_transform(r, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(r, 0, 0, f->width, f->height);
    ext_image_copy_capture_frame_v1_send_presentation_time(r, 0, 0, 0);
    ext_image_copy_capture_frame_v1_send_ready(r);
}
static const struct ext_image_copy_capture_frame_v1_interface ext_frame_impl = {
    .destroy = resource_destroy, .attach_buffer = ext_attach,
    .damage_buffer = ext_damage, .capture = ext_capture,
};
static void ext_create_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *frame = wl_resource_create(c, &ext_image_copy_capture_frame_v1_interface, 1, id);
    wl_resource_set_implementation(frame, &ext_frame_impl, wl_resource_get_user_data(r), NULL);
}
static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = ext_create_frame, .destroy = resource_destroy,
};
static void create_session(struct wl_client *c, struct wl_resource *r, uint32_t id,
                           struct wl_resource *source, uint32_t options) {
    (void)source; (void)options;
    struct fixture *f = wl_resource_get_user_data(r);
    struct wl_resource *session = wl_resource_create(c, &ext_image_copy_capture_session_v1_interface, 1, id);
    wl_resource_set_implementation(session, &session_impl, f, NULL);
    ext_image_copy_capture_session_v1_send_buffer_size(session, f->width, f->height);
    ext_image_copy_capture_session_v1_send_shm_format(session, f->format);
    ext_image_copy_capture_session_v1_send_done(session);
}
static const struct ext_image_copy_capture_manager_v1_interface ext_manager_impl = {
    .create_session = create_session, .destroy = resource_destroy,
};
static void bind_ext_manager(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &ext_image_copy_capture_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &ext_manager_impl, data, NULL);
}
static int control(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct fixture *f = data;
    char command;
    if (read(fd, &command, 1) != 1) { wl_display_terminate(f->display); return 0; }
    switch (command) {
    case 'D': f->color += 0x00010101; finish_copy(f, true); break;
    case 'F':
        if (f->pending) {
            zwlr_screencopy_frame_v1_send_failed(f->pending);
            f->pending = f->buffer = NULL;
        }
        break;
    case 'R':
        // Replace the output, preserving the buffer's byte count but not its layout.
        f->width = 8; f->height = 16; f->format = WL_SHM_FORMAT_XBGR8888;
        wl_global_destroy(f->output);
        f->output = wl_global_create(f->display, &wl_output_interface, 1, f, bind_output);
        break;
    case 'S': f->describe = false; break;
    case 'P': f->describe = true; break;
    case 'L':
        assert(f->pending);
        zwlr_screencopy_frame_v1_send_buffer(f->pending, f->format, f->width, f->height, f->width * 4);
        zwlr_screencopy_frame_v1_send_buffer_done(f->pending);
        break;
    case 'N':
        // Unrelated traffic must not extend a synchronous capture deadline.
        wl_global_create(f->display, &wl_seat_interface, 1, f, bind_seat);
        break;
    case 'Q': wl_display_terminate(f->display); break;
    default: abort();
    }
    atomic_fetch_add(&f->seen->commands, 1);
    return 0;
}

static void compositor(int socket, int commands, struct observed *seen, bool ext, bool wlr) {
    struct fixture f = { .seen = seen, .width = 16, .height = 8,
        .format = WL_SHM_FORMAT_XRGB8888, .color = 0xff202020, .describe = true };
    f.display = wl_display_create();
    assert(f.display && wl_display_init_shm(f.display) == 0);
    wl_display_add_shm_format(f.display, WL_SHM_FORMAT_XBGR8888);
    f.output = wl_global_create(f.display, &wl_output_interface, 1, &f, bind_output);
    wl_global_create(f.display, &wl_seat_interface, 1, &f, bind_seat);
    if (wlr)
        wl_global_create(f.display, &zwlr_screencopy_manager_v1_interface, 3, &f, bind_manager);
    if (ext) {
        wl_global_create(f.display, &ext_output_image_capture_source_manager_v1_interface, 1, &f, bind_sources);
        wl_global_create(f.display, &ext_image_copy_capture_manager_v1_interface, 1, &f, bind_ext_manager);
    }
    assert(wl_client_create(f.display, socket));
    wl_event_loop_add_fd(wl_display_get_event_loop(f.display), commands, WL_EVENT_READABLE, control, &f);
    wl_display_run(f.display);
    wl_display_destroy_clients(f.display);
    wl_display_destroy(f.display);
    close(commands);
}

static void command(int fd, struct observed *seen, char value) {
    unsigned before = atomic_load(&seen->commands);
    assert(write(fd, &value, 1) == 1);
    int64_t deadline = milliseconds() + 1000;
    while (atomic_load(&seen->commands) == before && milliseconds() < deadline) usleep(1000);
    assert(atomic_load(&seen->commands) != before);
}

static void dispatch(struct wr_wayland *w) {
    assert(wr_flush(w));
    struct pollfd p = { .fd = wr_fd(w), .events = POLLIN };
    assert(poll(&p, 1, 5) >= 0);
    assert(wr_dispatch_pending(w));
}

static const struct wr_frame *await_frame(struct wr_wayland *w) {
    int64_t deadline = milliseconds() + 2000;
    do {
        const struct wr_frame *frame = wr_capture_frame(w, 16);
        if (frame) return frame;
        dispatch(w);
    } while (milliseconds() < deadline);
    assert(!"capture did not complete");
    return NULL;
}

static void await_pending(struct wr_wayland *w, struct observed *seen, unsigned copies) {
    int64_t deadline = milliseconds() + 1000;
    while (atomic_load(&seen->copies) == copies && milliseconds() < deadline) dispatch(w);
    assert(atomic_load(&seen->copies) == copies + 1);
}

int main(int argc, char **argv) {
    bool ext = argc > 1;
    bool wlr = !ext || !strcmp(argv[1], "--both");
    int status;
    int sockets[2], commands[2], input[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, commands) == 0);
    assert(pipe(input) == 0);
    struct observed *seen = mmap(NULL, sizeof(*seen), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(seen != MAP_FAILED);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(sockets[0]); close(commands[0]); close(input[0]); close(input[1]);
        compositor(sockets[1], commands[1], seen, ext, wlr);
        _exit(0);
    }
    close(sockets[1]); close(commands[1]);
    char socket_name[32];
    snprintf(socket_name, sizeof(socket_name), "%d", sockets[0]);
    setenv("WAYLAND_SOCKET", socket_name, 1);
    const char *error = NULL;
    struct wr_wayland *w = wr_open(&error);
    assert(w && wr_capture_open(w, &error));
    if (ext) {
        if (wlr) {
            command(commands[0], seen, 'S');
            wr_capture_refresh(w);
            dispatch(w);
            command(commands[0], seen, 'F');
            dispatch(w);
            assert(!wr_capture_frame(w, 100));
            const struct wr_frame *frame = wr_capture_frame(w, 100);
            assert(frame && *(const uint32_t *)frame->pixels == 0xff202020);
            puts("PASS: optional screencopy failure does not starve ext capture");
        } else {
            const struct wr_frame *frame = wr_capture_now(w, 1000);
            assert(frame && *(const uint32_t *)frame->pixels == 0xff202020);
            puts("PASS: synchronous probe captures on an ext-only compositor");
        }
        goto done;
    }

    wr_capture_refresh(w);
    const struct wr_frame *frame = await_frame(w);
    assert(frame->damage_overflowed && *(const uint32_t *)frame->pixels == 0xff202020);
    unsigned copies = atomic_load(&seen->copies);
    assert(!wr_capture_frame(w, 16));
    await_pending(w, seen, copies);
    unsigned captures = atomic_load(&seen->captures);
    unsigned cancels = atomic_load(&seen->cancels);
    // A one-second capture budget must not delay servicing an already-ready input fd.
    assert(write(input[1], "i", 1) == 1);
    int64_t started = milliseconds();
    assert(!wr_capture_frame(w, 1000));
    char key;
    assert(read(input[0], &key, 1) == 1 && key == 'i');
    assert(milliseconds() - started < 200);
    for (int i = 0; i < 12; i++) {
        assert(!wr_capture_frame(w, 16));
        dispatch(w);
    }
    assert(atomic_load(&seen->captures) == captures);
    assert(atomic_load(&seen->cancels) == cancels);
    command(commands[0], seen, 'D');
    frame = await_frame(w);
    assert(!frame->damage_overflowed && frame->damage_count == 1);
    assert(*(const uint32_t *)frame->pixels == 0xff212121);
    const uint32_t *pixels = (const uint32_t *)frame->pixels;
    for (int i = 0; i < 4; i++) dispatch(w);
    command(commands[0], seen, 'D');
    for (int i = 0; i < 4; i++) dispatch(w);
    assert(*pixels == 0xff212121);
    assert(atomic_load(&seen->captures) == captures);
    puts("PASS: idle capture does not block input, cancel requests, or overwrite consumed pixels");

    copies = atomic_load(&seen->copies);
    assert(!wr_capture_frame(w, 16));
    await_pending(w, seen, copies);
    wr_capture_refresh(w);
    frame = await_frame(w);
    assert(frame->damage_overflowed && *(const uint32_t *)frame->pixels == 0xff222222);
    copies = atomic_load(&seen->copies);
    assert(!wr_capture_frame(w, 16));
    await_pending(w, seen, copies);
    wr_capture_cancel(w);
    dispatch(w);
    command(commands[0], seen, 'D');
    wr_capture_refresh(w);
    frame = await_frame(w);
    assert(frame->damage_overflowed && *(const uint32_t *)frame->pixels == 0xff232323);
    puts("PASS: refresh and reconnect repaint current pixels after canceling idle capture");

    copies = atomic_load(&seen->copies);
    assert(!wr_capture_frame(w, 16));
    await_pending(w, seen, copies);
    command(commands[0], seen, 'R');
    dispatch(w);
    frame = await_frame(w);
    assert(frame->width == 8 && frame->height == 16);
    assert(frame->format == WL_SHM_FORMAT_XBGR8888 && frame->damage_overflowed);
    assert(wr_width(w) == 8 && wr_height(w) == 16);
    puts("PASS: output replacement rebuilds equal-sized storage with new geometry and format");

    copies = atomic_load(&seen->copies);
    assert(!wr_capture_frame(w, 16));
    await_pending(w, seen, copies);
    command(commands[0], seen, 'F');
    dispatch(w);
    started = milliseconds();
    assert(!wr_capture_frame(w, 16));
    assert(milliseconds() - started < 200);
    captures = atomic_load(&seen->captures);
    for (int i = 0; i < 8; i++) { assert(!wr_capture_frame(w, 16)); dispatch(w); }
    assert(atomic_load(&seen->captures) == captures);
    frame = await_frame(w);
    assert(frame->damage_overflowed);
    puts("PASS: failed capture retries with a cooldown and a full repaint");

    wr_capture_cancel(w);
    command(commands[0], seen, 'S');
    pid_t noise = fork();
    assert(noise >= 0);
    if (!noise) {
        for (int i = 0; i < 80; i++) { assert(write(commands[0], "N", 1) == 1); usleep(5000); }
        _exit(0);
    }
    started = milliseconds();
    assert(!wr_capture_now(w, 60));
    assert(milliseconds() - started < 250);
    assert(waitpid(noise, &status, 0) == noise && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    command(commands[0], seen, 'P');
    frame = wr_capture_now(w, 1000);
    assert(frame && frame->width == 8);
    puts("PASS: synchronous probe deadline survives unrelated Wayland traffic");

    // Dispatch a late buffer_done before collection, as the RDP loop does.
    command(commands[0], seen, 'S');
    assert(!wr_capture_frame(w, 16));
    dispatch(w);
    copies = atomic_load(&seen->copies);
    usleep(5100000);
    command(commands[0], seen, 'L');
    dispatch(w);
    assert(!wr_capture_frame(w, 16));
    dispatch(w);
    assert(atomic_load(&seen->copies) == copies);
    command(commands[0], seen, 'P');
    frame = await_frame(w);
    assert(frame->damage_overflowed);
    puts("PASS: late buffer negotiation cannot bypass its deadline");

done:

    wr_close(w);
    command(commands[0], seen, 'Q');
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(commands[0]); close(input[0]); close(input[1]);
    munmap(seen, sizeof(*seen));
    return 0;
}
