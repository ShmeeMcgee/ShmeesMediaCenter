// Shmees Media Center: settings (settings.h).

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static char path[LIB_PATH];

static void defaults(Settings *s)
{
	memset(s, 0, sizeof *s);
	s->startup_animation = true;
	s->scan_on_start = true;
	s->autoplay_next = true;
	s->show_titles = true;
	s->volume = 100;
	// The hard disk's data partition (Hdd1:) and the first USB drive (Usb0:), as xsdk names them.
	static const char *const paths[] = { "hdd:/Movies", "hdd:/TV Shows", "usb0:/Movies", "usb0:/TV Shows" };
	for (uint32_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
		snprintf(s->roots[i].path, sizeof s->roots[i].path, "%s", paths[i]);
		s->roots[i].enabled = true;
	}
	s->root_count = sizeof paths / sizeof paths[0];
}

static bool parse_bool(const char *v)
{
	return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes");
}

void settings_load(Settings *s, const char *data_dir)
{
	snprintf(path, sizeof path, "%s/settings.txt", data_dir);
	defaults(s);
	FILE *f = fopen(path, "rb");
	if (!f) {
		settings_save(s);
		return;
	}
	bool roots_reset = false;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = 0;
		if (line[0] == '#' || !line[0])
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		const char *key = line;
		const char *value = eq + 1;
		if (!strcmp(key, "startup_animation"))
			s->startup_animation = parse_bool(value);
		else if (!strcmp(key, "scan_on_start"))
			s->scan_on_start = parse_bool(value);
		else if (!strcmp(key, "autoplay_next"))
			s->autoplay_next = parse_bool(value);
		else if (!strcmp(key, "show_titles"))
			s->show_titles = parse_bool(value);
		else if (!strcmp(key, "vector_unit"))
			s->vector = parse_bool(value);
		else if (!strcmp(key, "volume")) {
			const long v = strtol(value, NULL, 10);
			s->volume = v < 0 ? 0 : v > 100 ? 100 : (uint32_t)v;
		} else if (!strcmp(key, "folder")) {
			// folder=on|hdd:/Movies
			if (!roots_reset) {
				s->root_count = 0;
				roots_reset = true;
			}
			const char *bar = strchr(value, '|');
			if (!bar || s->root_count == LIB_MAX_ROOTS)
				continue;
			LibraryRoot *r = &s->roots[s->root_count++];
			memset(r, 0, sizeof *r);
			char flag[8] = { 0 };
			const size_t n = (size_t)(bar - value) < sizeof flag - 1 ? (size_t)(bar - value) : sizeof flag - 1;
			memcpy(flag, value, n);
			r->enabled = parse_bool(flag);
			snprintf(r->path, sizeof r->path, "%s", bar + 1);
		}
	}
	fclose(f);
}

void settings_save(const Settings *s)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		smc_log("settings: can't write %s: %d", path, errno);
		return;
	}
	fprintf(f, "# Shmees Media Center settings. Edit with the console off or the app closed.\n");
	fprintf(f, "# folder=on|<path> lines are the library folders: hdd: is Hdd1, usb0: the first USB drive.\n");
	fprintf(f, "startup_animation=%d\n", s->startup_animation);
	fprintf(f, "scan_on_start=%d\n", s->scan_on_start);
	fprintf(f, "autoplay_next=%d\n", s->autoplay_next);
	fprintf(f, "show_titles=%d\n", s->show_titles);
	fprintf(f, "vector_unit=%d\n", s->vector);
	fprintf(f, "volume=%u\n", (unsigned)s->volume);
	for (uint32_t i = 0; i < s->root_count; i++)
		fprintf(f, "folder=%s|%s\n", s->roots[i].enabled ? "on" : "off", s->roots[i].path);
	fclose(f);
}
