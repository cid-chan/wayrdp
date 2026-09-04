// rdp.h — the protocol half.

#ifndef WAYRDP_RDP_H
#define WAYRDP_RDP_H

#include <stdbool.h>

#include "audio.h"
#include "config.h"
#include "wayland.h"

struct wr_server;

// `audio` may be NULL: a session with no sound server is still a session, and
// refusing to serve the screen over it would be the wrong trade.
struct wr_server *wr_server_new(struct wr_wayland *wayland,
                                struct wr_audio *audio,
                                const struct wr_config *config,
                                const char **error);

// Blocks until the listener fails or the process is asked to stop.
bool wr_server_run(struct wr_server *server, const volatile bool *running);

void wr_server_free(struct wr_server *server);

#endif
