// Shmees Media Center: what's in a video file, read from its MP4/MOV boxes without decoding anything: the codecs, the
// picture size, the frame rate and the length. The groundwork for playback, and what the details page shows.
#ifndef SMC_PROBE_H
#define SMC_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool ok;
	// "H.264 High@4.1", "HEVC", "MPEG-4 Visual" and so on; empty with no video track.
	char video[40];
	char audio[40];
	uint16_t width;
	uint16_t height;
	// Frames a second times 1000.
	uint32_t fps_milli;
	uint32_t duration_s;
	uint32_t audio_rate;
	uint16_t audio_channels;
	// Where the index (moov) sits: before the media ("fast start") reads with one seek.
	bool moov_first;
	char note[96];
} ProbeInfo;

// Reads what it can; containers it doesn't parse yet get a note saying so.
void probe_file(const char *path, ProbeInfo *info);

#endif
