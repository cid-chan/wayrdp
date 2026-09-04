// audio.c — PipeWire on one side, a ring buffer on the other.
//
// The desktop's sound leaves through a capture stream on the default sink's
// monitor, which is the same thing a screen recorder does and needs no
// permission and no configuration: whatever comes out of the speakers is what
// the client hears, including applications started long before it connected.
//
// The client's microphone arrives as a node whose media.class is Audio/Source,
// which is all it takes for the session manager to treat it as a real capture
// device. It is only published while a client is actually sending, so the list
// in a browser's device picker does not grow a microphone that answers silence.

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "audio.h"

// A quarter of a second each way. Long enough to ride out a slow frame on the
// protocol side, short enough that catching up never means catching up on
// audio somebody said a second ago.
#define RING_BYTES (64 * 1024)

// --- ring -------------------------------------------------------------------

struct ring {
    uint8_t *data;
    size_t size;
    size_t used;
    size_t read;
    size_t frame;    // bytes per sample frame; every drop is a multiple of it
};

static bool ring_init(struct ring *r, size_t size, size_t frame) {
    r->data = malloc(size);
    r->size = size;
    r->used = 0;
    r->read = 0;
    r->frame = frame;
    return r->data != NULL;
}

// An overrun drops the oldest bytes rather than the newest. Late audio is worse
// than missing audio: keeping it would push everything said afterwards further
// behind, and the gap never closes on its own.
static void ring_write(struct ring *r, const uint8_t *src, size_t n) {
    if (n > r->size) { src += n - r->size; n = r->size; }

    size_t free_bytes = r->size - r->used;
    if (n > free_bytes) {
        // Rounded up to a whole frame. Dropping half a frame would swap the
        // channels for the rest of the connection, and nothing ever puts them
        // back: a stutter is recoverable, a permanent shift is not.
        size_t drop = n - free_bytes;
        drop += (r->frame - drop % r->frame) % r->frame;
        if (drop > r->used) drop = r->used;
        r->read = (r->read + drop) % r->size;
        r->used -= drop;
    }

    size_t at = (r->read + r->used) % r->size;
    size_t first = r->size - at;
    if (first > n) first = n;
    memcpy(r->data + at, src, first);
    memcpy(r->data, src + first, n - first);
    r->used += n;
}

static size_t ring_read(struct ring *r, uint8_t *dst, size_t n) {
    if (n > r->used) n = r->used;
    size_t first = r->size - r->read;
    if (first > n) first = n;
    memcpy(dst, r->data + r->read, first);
    memcpy(dst + first, r->data, n - first);
    r->read = (r->read + n) % r->size;
    r->used -= n;
    return n;
}

// --- the two streams --------------------------------------------------------

struct wr_audio {
    struct pw_thread_loop *loop;

    struct pw_stream *speaker;
    struct pw_stream *mic;

    // One lock for both rings. It is held for a memcpy of a few kilobytes every
    // ten milliseconds; a lock-free queue here would buy nothing and cost the
    // ability to read the code.
    pthread_mutex_t lock;
    struct ring to_client;
    struct ring from_client;

    uint32_t mic_frame;      // bytes per frame on the microphone stream
};

static void speaker_process(void *data) {
    struct wr_audio *a = data;

    struct pw_buffer *b = pw_stream_dequeue_buffer(a->speaker);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size > 0) {
        pthread_mutex_lock(&a->lock);
        ring_write(&a->to_client, (const uint8_t *)d->data + d->chunk->offset,
                   d->chunk->size);
        pthread_mutex_unlock(&a->lock);
    }

    pw_stream_queue_buffer(a->speaker, b);
}

static void mic_process(void *data) {
    struct wr_audio *a = data;

    struct pw_buffer *b = pw_stream_dequeue_buffer(a->mic);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        uint32_t want = d->maxsize;
        if (b->requested) {
            uint64_t asked = b->requested * a->mic_frame;
            if (asked < want) want = (uint32_t)asked;
        }
        want -= want % a->mic_frame;   // whole frames, or the channels swap

        pthread_mutex_lock(&a->lock);
        size_t got = ring_read(&a->from_client, d->data, want);
        pthread_mutex_unlock(&a->lock);

        // Silence rather than a short buffer. A source that answers with less
        // than it was asked for reads as a broken device, and the client's
        // microphone genuinely is silent between packets.
        if (got < want) memset((uint8_t *)d->data + got, 0, want - got);

        d->chunk->offset = 0;
        d->chunk->stride = (int32_t)a->mic_frame;
        d->chunk->size = want;
    }

    pw_stream_queue_buffer(a->mic, b);
}

// What was asked for and what was agreed are two different things, and the
// difference is measured in bytes per frame. Getting it wrong does not fail: it
// plays back at the wrong speed, which is how an 880 Hz tone arrives at 440.
static bool negotiated_frame(void *data, uint32_t id, const struct spa_pod *param,
                             const char *which, struct wr_audio *a, size_t *frame) {
    (void)data;
    if (!param || id != SPA_PARAM_Format) return false;

    struct spa_audio_info info = { 0 };
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return false;
    if (info.media_type != SPA_MEDIA_TYPE_audio ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw) return false;
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0) return false;

    size_t width = 2;   // everything here is S16, and nothing else is offered
    *frame = width * info.info.raw.channels;

    pthread_mutex_lock(&a->lock);
    struct ring *r = (which[0] == 's') ? &a->to_client : &a->from_client;
    r->frame = *frame;
    r->used = r->read = 0;
    pthread_mutex_unlock(&a->lock);

    fprintf(stderr, "wayrdp: %s stream agreed %u Hz, %u channel%s\n", which,
            info.info.raw.rate, info.info.raw.channels,
            info.info.raw.channels == 1 ? "" : "s");
    return true;
}

static void speaker_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct wr_audio *a = data;
    size_t frame = 0;
    negotiated_frame(data, id, param, "speaker", a, &frame);
}

static void mic_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct wr_audio *a = data;
    size_t frame = 0;
    if (negotiated_frame(data, id, param, "microphone", a, &frame))
        a->mic_frame = (uint32_t)frame;
}

static const struct pw_stream_events speaker_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = speaker_param_changed,
    .process = speaker_process,
};

static const struct pw_stream_events mic_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = mic_param_changed,
    .process = mic_process,
};

// --- lifetime ---------------------------------------------------------------

struct wr_audio *wr_audio_open(const char **error) {
    struct wr_audio *a = calloc(1, sizeof(*a));
    if (!a) { *error = "out of memory"; return NULL; }

    pthread_mutex_init(&a->lock, NULL);
    // The inbound frame size is not known until the client picks a format;
    // one sample of one channel is the smallest it can ever be.
    if (!ring_init(&a->to_client, RING_BYTES, WR_SPEAKER_FRAME) ||
        !ring_init(&a->from_client, RING_BYTES, 2)) {
        *error = "out of memory";
        wr_audio_close(a);
        return NULL;
    }

    pw_init(NULL, NULL);

    a->loop = pw_thread_loop_new("wayrdp-audio", NULL);
    if (!a->loop) {
        *error = "could not create the audio loop";
        wr_audio_close(a);
        return NULL;
    }
    if (pw_thread_loop_start(a->loop) < 0) {
        *error = "could not start the audio loop";
        wr_audio_close(a);
        return NULL;
    }
    return a;
}

void wr_audio_close(struct wr_audio *a) {
    if (!a) return;

    wr_speaker_stop(a);
    wr_mic_stop(a);

    if (a->loop) {
        pw_thread_loop_stop(a->loop);
        pw_thread_loop_destroy(a->loop);
    }
    free(a->to_client.data);
    free(a->from_client.data);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

// --- desktop -> client ------------------------------------------------------

bool wr_speaker_start(struct wr_audio *a, const char **error) {
    if (!a) { *error = "no audio"; return false; }
    if (a->speaker) return true;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        // The whole trick: capture the sink rather than a source, and PipeWire
        // routes the default output's monitor here without anyone choosing it.
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_NODE_NAME, "wayrdp",
        PW_KEY_NODE_DESCRIPTION, "Remote desktop",
        PW_KEY_NODE_LATENCY, "480/48000",
        NULL);
    if (!props) { *error = "out of memory"; return false; }

    pw_thread_loop_lock(a->loop);

    a->speaker = pw_stream_new_simple(pw_thread_loop_get_loop(a->loop),
                                      "wayrdp speaker", props,
                                      &speaker_events, a);
    if (!a->speaker) {
        pw_thread_loop_unlock(a->loop);
        *error = "could not create the capture stream";
        return false;
    }

    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(
        &b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16,
                                 .rate = WR_SPEAKER_RATE,
                                 .channels = WR_SPEAKER_CHANNELS));

    int rc = pw_stream_connect(a->speaker, PW_DIRECTION_INPUT, PW_ID_ANY,
                               PW_STREAM_FLAG_AUTOCONNECT |
                               PW_STREAM_FLAG_MAP_BUFFERS,
                               params, 1);
    pw_thread_loop_unlock(a->loop);

    if (rc < 0) {
        wr_speaker_stop(a);
        *error = "could not connect to the default output";
        return false;
    }
    return true;
}

void wr_speaker_stop(struct wr_audio *a) {
    if (!a || !a->speaker) return;

    pw_thread_loop_lock(a->loop);
    pw_stream_destroy(a->speaker);
    a->speaker = NULL;
    pw_thread_loop_unlock(a->loop);

    pthread_mutex_lock(&a->lock);
    a->to_client.used = a->to_client.read = 0;
    pthread_mutex_unlock(&a->lock);
}

size_t wr_speaker_read(struct wr_audio *a, void *buf, size_t bytes) {
    if (!a) return 0;
    pthread_mutex_lock(&a->lock);
    size_t got = ring_read(&a->to_client, buf, bytes);
    pthread_mutex_unlock(&a->lock);
    return got;
}

// --- client -> desktop ------------------------------------------------------

bool wr_mic_start(struct wr_audio *a, uint32_t rate, uint32_t channels,
                  const char **error) {
    if (!a) { *error = "no audio"; return false; }
    if (a->mic) return true;
    if (rate == 0 || channels == 0 || channels > 8) {
        *error = "the client asked for a microphone format nothing can produce";
        return false;
    }

    a->mic_frame = 2 * channels;

    pthread_mutex_lock(&a->lock);
    a->from_client.frame = a->mic_frame;
    a->from_client.used = a->from_client.read = 0;
    pthread_mutex_unlock(&a->lock);

    // media.class Audio/Source is what makes this a device rather than a
    // playing stream: without it the samples would go to the speakers, which is
    // the opposite of a microphone.
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, "wayrdp-microphone",
        PW_KEY_NODE_DESCRIPTION, "Remote microphone",
        NULL);
    if (!props) { *error = "out of memory"; return false; }

    pw_thread_loop_lock(a->loop);

    a->mic = pw_stream_new_simple(pw_thread_loop_get_loop(a->loop),
                                  "Remote microphone", props,
                                  &mic_events, a);
    if (!a->mic) {
        pw_thread_loop_unlock(a->loop);
        *error = "could not create the microphone node";
        return false;
    }

    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(
        &b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16,
                                 .rate = rate,
                                 .channels = channels));

    // No AUTOCONNECT: a source waits to be chosen. Connecting it to whatever
    // happens to be around would put the remote microphone into the speakers.
    int rc = pw_stream_connect(a->mic, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                               PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    pw_thread_loop_unlock(a->loop);

    if (rc < 0) {
        wr_mic_stop(a);
        *error = "could not publish the microphone";
        return false;
    }
    return true;
}

void wr_mic_stop(struct wr_audio *a) {
    if (!a || !a->mic) return;

    pw_thread_loop_lock(a->loop);
    pw_stream_destroy(a->mic);
    a->mic = NULL;
    pw_thread_loop_unlock(a->loop);

    pthread_mutex_lock(&a->lock);
    a->from_client.used = a->from_client.read = 0;
    pthread_mutex_unlock(&a->lock);
}

void wr_mic_write(struct wr_audio *a, const void *buf, size_t bytes) {
    if (!a || bytes == 0) return;
    pthread_mutex_lock(&a->lock);
    ring_write(&a->from_client, buf, bytes);
    pthread_mutex_unlock(&a->lock);
}
