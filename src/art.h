// Shmees Media Center: posters and backdrops. A thread decodes and scales image files; the UI thread turns them into
// textures a few a frame, and the least recently drawn go once too many are held.
#ifndef SMC_ART_H
#define SMC_ART_H

#include <stdint.h>

#include "gfx.h"

typedef enum {
	ART_POSTER = 0,
	ART_BACKDROP = 1,
} ArtKind;

typedef struct {
	uint32_t held;
	uint32_t held_bytes;
	uint32_t queued;
	uint32_t loaded;
	uint32_t failed;
} ArtStats;

void art_open(void);
void art_close(void);
// The image for a file, or NULL while it loads or if it can't. Asking keeps it from being let go this frame.
const GfxImage *art_get(const char *path, ArtKind kind);
// Once a frame on the UI thread: makes textures of what was decoded and lets go of the oldest beyond the limit.
void art_update(void);
void art_stats(ArtStats *stats);

#endif
