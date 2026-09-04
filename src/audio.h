// audio.h — the sound half, in both directions.
//
// Outbound is what the desktop is playing, taken from the default sink's
// monitor. Inbound is the client's microphone, published as a source node so an
// application on the remote desktop can pick "Remote microphone" the way it
// picks any other one.
//
// Nothing here knows what RDP is. Two ring buffers are the seam: PipeWire's
// thread writes one and reads the other, the protocol half does the opposite,
// and a stall on either side costs audio and nothing else.

#ifndef WAYRDP_AUDIO_H
#define WAYRDP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 16-bit little-endian PCM at 48 kHz, stereo, going out. It is the one format
// every RDP client has, and the one PipeWire needs no help with, so there is no
// resampler and no codec anywhere in this path.
#define WR_SPEAKER_RATE      48000
#define WR_SPEAKER_CHANNELS  2
#define WR_SPEAKER_FRAME     (2 * WR_SPEAKER_CHANNELS)   // bytes per frame

struct wr_audio;

// Starts PipeWire's thread. Neither stream exists yet: they are created when a
// client asks for that direction, so a session nobody is watching holds no
// capture stream and shows no microphone.
struct wr_audio *wr_audio_open(const char **error);
void wr_audio_close(struct wr_audio *a);

// --- desktop -> client ------------------------------------------------------

bool wr_speaker_start(struct wr_audio *a, const char **error);
void wr_speaker_stop(struct wr_audio *a);

// Copies out at most `bytes` and returns how many there were. Zero is the
// normal answer on a silent desktop.
size_t wr_speaker_read(struct wr_audio *a, void *buf, size_t bytes);

// --- client -> desktop ------------------------------------------------------

// The rate and channel count are the ones the client agreed to; the node is
// published with exactly that format so nothing has to resample.
bool wr_mic_start(struct wr_audio *a, uint32_t rate, uint32_t channels,
                  const char **error);
void wr_mic_stop(struct wr_audio *a);

// Safe to call from whichever thread the channel runs on.
void wr_mic_write(struct wr_audio *a, const void *buf, size_t bytes);

#endif
