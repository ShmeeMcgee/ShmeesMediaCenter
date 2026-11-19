// Shmees VMX Probe: runs, one at a time, each vector and cache instruction FFmpeg's PowerPC code uses, exactly as
// FFmpeg's compiled code encodes it, to find which the Xbox 360's VMX128 unit lacks. VMX128 dropped some of VMX's
// integer instructions and nothing public says which.
//
// Each instruction's name goes on screen, into the log and into vmxprobe.txt (flushed) before it runs. If one isn't
// there the console may freeze: turn it off and on, start the probe again, and it marks that one missing and carries on.
// It writes nothing outside its own folder.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/text.h"

typedef struct {
	const char *name;
	const char *operands;
	const char *users;
	int suspect;
	void (*run)(void *buffer);
} Probe;

#include "probes.h"

enum {
	COUNT = sizeof PROBES / sizeof PROBES[0],
	BUFFERS = 2,
	BEGIN_TIMEOUT_MS = 5000,
	CLEAR_ARGB = 0xFF101418,
	LIME = 0xFF96E63C,
	WHITE = 0xFFF2F5F7,
	DIM = 0xFF9AA4AE,
	RED = 0xFFE5484D,
	// Frames between instructions: long enough to read the name on screen if it freezes.
	FRAMES_EACH = 20,
};

typedef enum {
	UNTESTED,
	PASSED,
	MISSING,
} Result;

static Result results[COUNT];
static const char *STATE = "game:/vmxprobe.txt";
// 256 bytes on a 128-byte line, for the cache-line instructions.
static uint8_t buffer[512] __attribute__((aligned(128)));

static int index_of(const char *name)
{
	for (int i = 0; i < (int)COUNT; i++)
		if (!strcmp(PROBES[i].name, name))
			return i;
	return -1;
}

// "ok <name>", "missing <name>" and "trying <name>" lines; a "trying" with no result after it is one that never came
// back.
static void load_state(void)
{
	FILE *f = fopen(STATE, "rb");
	if (!f)
		return;
	char line[128];
	char trying[64] = "";
	while (fgets(line, sizeof line, f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = 0;
		char *space = strchr(line, ' ');
		if (!space)
			continue;
		*space = 0;
		const int i = index_of(space + 1);
		if (i < 0)
			continue;
		if (!strcmp(line, "ok")) {
			results[i] = PASSED;
			if (!strcmp(trying, space + 1))
				trying[0] = 0;
		} else if (!strcmp(line, "missing")) {
			results[i] = MISSING;
			if (!strcmp(trying, space + 1))
				trying[0] = 0;
		} else if (!strcmp(line, "trying")) {
			snprintf(trying, sizeof trying, "%s", space + 1);
		}
	}
	fclose(f);
	const int i = trying[0] ? index_of(trying) : -1;
	if (i >= 0) {
		results[i] = MISSING;
		xsdk_log_write("probe: %s didn't come back last time: marked missing", trying);
		FILE *a = fopen(STATE, "ab");
		if (a) {
			fprintf(a, "missing %s\n", trying);
			fflush(a);
			fsync(fileno(a));
			fclose(a);
		}
	}
}

static void note(const char *what, const char *name)
{
	FILE *f = fopen(STATE, "ab");
	if (!f) {
		xsdk_log_write("probe: can't write %s", STATE);
		return;
	}
	fprintf(f, "%s %s\n", what, name);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

static void draw(uint32_t width, int current, bool done)
{
	char line[256];
	xsdk_text_draw(80, 50, 3, LIME, "SHMEES VMX PROBE");
	xsdk_text_draw(80, 120, 1, DIM,
		"Tries each vector instruction the video decoder can use. If the console freezes, hold the power\n"
		"button to turn it off, turn it on, and start this probe again: it skips the one that froze.");
	int passed = 0, missing = 0;
	for (int i = 0; i < (int)COUNT; i++) {
		passed += results[i] == PASSED;
		missing += results[i] == MISSING;
	}
	if (done)
		snprintf(line, sizeof line, "Done: %d of %d ran, %d missing.  Press Back to exit.", passed, (int)COUNT, missing);
	else
		snprintf(line, sizeof line, "Testing %s %s  (%d of %d)", PROBES[current].name, PROBES[current].operands,
			current + 1, (int)COUNT);
	xsdk_text_draw(80, 180, 2, done ? LIME : WHITE, line);
	// A grid of every instruction: white passed, red missing, dim not yet.
	const int columns = 6;
	const int cell = (int)(width - 160) / columns;
	for (int i = 0; i < (int)COUNT; i++) {
		const int x = 80 + (i % columns) * cell;
		const int y = 250 + (i / columns) * 30;
		const uint32_t argb = results[i] == PASSED ? WHITE : results[i] == MISSING ? RED : i == current ? LIME : 0xFF5C6670;
		snprintf(line, sizeof line, "%s%s", results[i] == MISSING ? "x " : "", PROBES[i].name);
		xsdk_text_draw(x, y, 1, argb, line);
	}
}

static void summary(void)
{
	char line[1024];
	size_t n = 0;
	line[0] = 0;
	for (int i = 0; i < (int)COUNT; i++)
		if (results[i] == MISSING)
			n += (size_t)snprintf(line + n, sizeof line - n, "%s ", PROBES[i].name);
	xsdk_log_write("probe: done; missing: %s", n ? line : "none");
	FILE *f = fopen("game:/vmxprobe-result.txt", "wb");
	if (f) {
		for (int i = 0; i < (int)COUNT; i++)
			fprintf(f, "%s %s (%s)\n", results[i] == PASSED ? "ok     " : results[i] == MISSING ? "MISSING" : "untried",
				PROBES[i].name, PROBES[i].users);
		fclose(f);
	}
}

int main(void)
{
	load_state();
	if (!NT_SUCCESS(xsdk_gpu_open()) || !NT_SUCCESS(xsdk_text_open())) {
		xsdk_log_write("probe: the GPU didn't open");
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	if (!NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, 1, &width, &height))) {
		xsdk_log_write("probe: the display didn't open");
		return 1;
	}
	xsdk_log_write("probe: %d instructions", (int)COUNT);
	int current = 0;
	while (current < (int)COUNT && results[current] != UNTESTED)
		current++;
	bool done = current >= (int)COUNT;
	if (done)
		summary();
	for (uint32_t frame = 0;; frame++) {
		INPUT_STATE input = { 0 };
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK && done)
			break;
		uint32_t address = 0;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS)))
			break;
		xsdk_gpu_set_target(width, height);
		draw(width, current, done);
		xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
		xsdk_present_end();
		// The name is on screen (and in the file) for a while before the instruction runs.
		if (!done && frame % FRAMES_EACH == FRAMES_EACH - 1) {
			const Probe *p = &PROBES[current];
			note("trying", p->name);
			xsdk_log_write("probe: trying %s %s (used by %s)", p->name, p->operands, p->users);
			memset(buffer, 0x5A, sizeof buffer);
			p->run(buffer + 128);
			results[current] = PASSED;
			note("ok", p->name);
			xsdk_log_write("probe: %s ran", p->name);
			while (current < (int)COUNT && results[current] != UNTESTED)
				current++;
			if (current >= (int)COUNT) {
				done = true;
				summary();
			}
		}
	}
	xsdk_present_close();
	xsdk_text_close();
	xsdk_gpu_close();
	return 0;
}
