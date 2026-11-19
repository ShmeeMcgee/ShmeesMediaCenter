// Shmees Media Center: posters and backdrops (art.h).

#include "art.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"
#include "log.h"
#include "threads.h"

enum {
	SLOTS = 96,
	// Posters fit 320x480 (2:3), backdrops 1280x720.
	POSTER_W = 320,
	POSTER_H = 480,
	BACKDROP_W = 1280,
	BACKDROP_H = 720,
	MAX_POSTERS = 72,
	MAX_BACKDROPS = 4,
	UPLOADS_PER_FRAME = 2,
};

typedef enum {
	SLOT_FREE,
	SLOT_QUEUED,
	SLOT_DECODING,
	SLOT_DECODED,
	SLOT_READY,
	SLOT_FAILED,
} SlotState;

typedef struct {
	char path[LIB_PATH];
	uint8_t kind;
	SlotState state;
	uint32_t last_used;
	uint32_t requested;
	uint8_t *rgba;
	uint32_t width;
	uint32_t height;
	GfxImage image;
} Slot;

static Slot slots[SLOTS];
static uint32_t frame;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool running;
static uint32_t loaded_count;
static uint32_t failed_count;

// The newest queued request first: what the viewer just moved to matters most.
static Slot *next_request(void)
{
	Slot *best = NULL;
	for (uint32_t i = 0; i < SLOTS; i++)
		if (slots[i].state == SLOT_QUEUED && (!best || slots[i].requested > best->requested))
			best = &slots[i];
	return best;
}

static void *decode_main(void *argument)
{
	(void)argument;
	pthread_mutex_lock(&lock);
	while (running) {
		Slot *slot = next_request();
		if (!slot) {
			pthread_cond_wait(&wake, &lock);
			continue;
		}
		slot->state = SLOT_DECODING;
		char path[LIB_PATH];
		memcpy(path, slot->path, sizeof path);
		const ArtKind kind = (ArtKind)slot->kind;
		pthread_mutex_unlock(&lock);

		uint32_t w = 0;
		uint32_t h = 0;
		uint8_t *rgba = kind == ART_BACKDROP ? gfx_decode_file(path, BACKDROP_W, BACKDROP_H, &w, &h)
											 : gfx_decode_file(path, POSTER_W, POSTER_H, &w, &h);

		pthread_mutex_lock(&lock);
		// The slot may have been let go while decoding; it's only reused once it leaves SLOT_DECODING.
		if (rgba) {
			slot->rgba = rgba;
			slot->width = w;
			slot->height = h;
			slot->state = SLOT_DECODED;
		} else {
			slot->state = SLOT_FAILED;
			failed_count++;
			smc_log("art: can't decode %s", path);
		}
	}
	pthread_mutex_unlock(&lock);
	return NULL;
}

void art_open(void)
{
	memset(slots, 0, sizeof slots);
	running = true;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if (pthread_create(&thread, &attr, decode_main, NULL) != 0) {
		smc_log("art: can't start the decode thread");
		running = false;
	} else {
		threads_place(thread, HW_IO);
	}
	pthread_attr_destroy(&attr);
}

void art_close(void)
{
	pthread_mutex_lock(&lock);
	const bool was = running;
	running = false;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	if (was)
		pthread_join(thread, NULL);
	for (uint32_t i = 0; i < SLOTS; i++) {
		free(slots[i].rgba);
		if (slots[i].image.valid)
			gfx_image_release(&slots[i].image);
	}
	memset(slots, 0, sizeof slots);
}

// The least recently used slot not in use this frame and not being decoded, to reuse.
static Slot *victim(void)
{
	Slot *best = NULL;
	for (uint32_t i = 0; i < SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state == SLOT_FREE)
			return s;
		if (s->state == SLOT_DECODING || s->last_used + 1 >= frame)
			continue;
		if (!best || s->last_used < best->last_used)
			best = s;
	}
	return best;
}

static void empty(Slot *s)
{
	free(s->rgba);
	s->rgba = NULL;
	if (s->image.valid)
		gfx_image_release(&s->image);
	s->state = SLOT_FREE;
	s->path[0] = 0;
}

const GfxImage *art_get(const char *path, ArtKind kind)
{
	if (!path || !path[0])
		return NULL;
	pthread_mutex_lock(&lock);
	Slot *found = NULL;
	for (uint32_t i = 0; i < SLOTS; i++)
		if (slots[i].state != SLOT_FREE && slots[i].kind == kind && !strcmp(slots[i].path, path)) {
			found = &slots[i];
			break;
		}
	if (!found) {
		found = victim();
		if (found) {
			empty(found);
			strncpy(found->path, path, sizeof found->path - 1);
			found->path[sizeof found->path - 1] = 0;
			found->kind = (uint8_t)kind;
			found->state = SLOT_QUEUED;
			found->requested = frame;
			pthread_cond_signal(&wake);
		}
	}
	const GfxImage *image = NULL;
	if (found) {
		found->last_used = frame;
		if (found->state == SLOT_QUEUED)
			found->requested = frame;
		if (found->state == SLOT_READY)
			image = &found->image;
	}
	pthread_mutex_unlock(&lock);
	return image;
}

void art_update(void)
{
	frame++;
	pthread_mutex_lock(&lock);
	uint32_t uploads = 0;
	uint32_t posters = 0;
	uint32_t backdrops = 0;
	for (uint32_t i = 0; i < SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state == SLOT_DECODED && uploads < UPLOADS_PER_FRAME) {
			if (gfx_image_from_rgba(&s->image, s->rgba, s->width, s->height)) {
				s->state = SLOT_READY;
				loaded_count++;
			} else {
				s->state = SLOT_FAILED;
				failed_count++;
			}
			free(s->rgba);
			s->rgba = NULL;
			uploads++;
		}
		// Requests nobody has asked for in a while are dropped before they're decoded.
		if (s->state == SLOT_QUEUED && s->last_used + 30 < frame)
			empty(s);
		if (s->state == SLOT_READY)
			*(s->kind == ART_BACKDROP ? &backdrops : &posters) += 1;
	}
	// Over the limits: let the oldest go.
	while (posters > MAX_POSTERS || backdrops > MAX_BACKDROPS) {
		const uint8_t kind = backdrops > MAX_BACKDROPS ? ART_BACKDROP : ART_POSTER;
		Slot *oldest = NULL;
		for (uint32_t i = 0; i < SLOTS; i++) {
			Slot *s = &slots[i];
			if (s->state == SLOT_READY && s->kind == kind && s->last_used + 1 < frame &&
				(!oldest || s->last_used < oldest->last_used))
				oldest = s;
		}
		if (!oldest)
			break;
		empty(oldest);
		*(kind == ART_BACKDROP ? &backdrops : &posters) -= 1;
	}
	pthread_mutex_unlock(&lock);
}

void art_stats(ArtStats *stats)
{
	memset(stats, 0, sizeof *stats);
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < SLOTS; i++) {
		if (slots[i].state == SLOT_READY) {
			stats->held++;
			stats->held_bytes += slots[i].image.texture.size_bytes;
		}
		if (slots[i].state == SLOT_QUEUED || slots[i].state == SLOT_DECODING)
			stats->queued++;
	}
	stats->loaded = loaded_count;
	stats->failed = failed_count;
	pthread_mutex_unlock(&lock);
}
