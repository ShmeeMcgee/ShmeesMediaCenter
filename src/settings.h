// Shmees Media Center: settings, kept in game:/data/settings.txt as key=value lines a person can also edit by hand.
#ifndef SMC_SETTINGS_H
#define SMC_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

typedef struct {
	bool startup_animation;
	bool scan_on_start;
	bool autoplay_next;
	// Posters' titles under them in grids.
	bool show_titles;
	// FFmpeg's AltiVec code for decoding (experimental: only once the VMX probe cleared it).
	bool vector;
	// 0 to 100.
	uint32_t volume;
	LibraryRoot roots[LIB_MAX_ROOTS];
	uint32_t root_count;
} Settings;

void settings_load(Settings *settings, const char *data_dir);
void settings_save(const Settings *settings);

#endif
