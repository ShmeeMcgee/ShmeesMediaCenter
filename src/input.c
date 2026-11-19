// Shmees Media Center: the controller (input.h).

#include "input.h"

#include <stdlib.h>

#include "xsdk/kernel.h"

enum {
	PADS = 4,
	// XInput's own dead zones.
	LEFT_DEAD_ZONE = 7849,
	RIGHT_DEAD_ZONE = 8689,
	TRIGGER_THRESHOLD = 30,
	// At 60 frames a second: the first repeat after a third of a second, then 8 a second, 15 once held over a second.
	REPEAT_DELAY = 20,
	REPEAT_SLOW = 7,
	REPEAT_FAST = 4,
	FAST_AFTER = 60,
	REPEATING = IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT | IN_LT | IN_RT,
};

static uint32_t previous;
static uint32_t held_frames[16];

static uint32_t from_pad(const INPUT_GAMEPAD *pad, float *rx, float *ry)
{
	uint32_t bits = 0;
	const uint16_t b = pad->buttons;
	if (b & INPUT_GAMEPAD_DPAD_UP || pad->thumb_ly > LEFT_DEAD_ZONE * 2)
		bits |= IN_UP;
	if (b & INPUT_GAMEPAD_DPAD_DOWN || pad->thumb_ly < -LEFT_DEAD_ZONE * 2)
		bits |= IN_DOWN;
	if (b & INPUT_GAMEPAD_DPAD_LEFT || pad->thumb_lx < -LEFT_DEAD_ZONE * 2)
		bits |= IN_LEFT;
	if (b & INPUT_GAMEPAD_DPAD_RIGHT || pad->thumb_lx > LEFT_DEAD_ZONE * 2)
		bits |= IN_RIGHT;
	if (b & INPUT_GAMEPAD_A)
		bits |= IN_A;
	if (b & INPUT_GAMEPAD_B)
		bits |= IN_B;
	if (b & INPUT_GAMEPAD_X)
		bits |= IN_X;
	if (b & INPUT_GAMEPAD_Y)
		bits |= IN_Y;
	if (b & INPUT_GAMEPAD_LEFT_SHOULDER)
		bits |= IN_LB;
	if (b & INPUT_GAMEPAD_RIGHT_SHOULDER)
		bits |= IN_RB;
	if (pad->left_trigger > TRIGGER_THRESHOLD)
		bits |= IN_LT;
	if (pad->right_trigger > TRIGGER_THRESHOLD)
		bits |= IN_RT;
	if (b & INPUT_GAMEPAD_START)
		bits |= IN_START;
	if (b & INPUT_GAMEPAD_BACK)
		bits |= IN_BACK;
	if (b & INPUT_GAMEPAD_LEFT_THUMB)
		bits |= IN_LS;
	if (b & INPUT_GAMEPAD_RIGHT_THUMB)
		bits |= IN_RS;
	const int x = pad->thumb_rx;
	const int y = pad->thumb_ry;
	if (abs(x) > RIGHT_DEAD_ZONE)
		*rx += (float)(x - (x > 0 ? RIGHT_DEAD_ZONE : -RIGHT_DEAD_ZONE)) / (32767.0f - RIGHT_DEAD_ZONE);
	if (abs(y) > RIGHT_DEAD_ZONE)
		*ry += (float)(y - (y > 0 ? RIGHT_DEAD_ZONE : -RIGHT_DEAD_ZONE)) / (32767.0f - RIGHT_DEAD_ZONE);
	return bits;
}

void input_poll(Input *input)
{
	uint32_t held = 0;
	float rx = 0.0f;
	float ry = 0.0f;
	bool connected = false;
	for (uint32_t i = 0; i < PADS; i++) {
		INPUT_STATE state = { 0 };
		if (XamInputGetState(i, 0, &state) != ERROR_SUCCESS)
			continue;
		connected = true;
		held |= from_pad(&state.gamepad, &rx, &ry);
	}
	uint32_t pressed = held & ~previous;
	for (uint32_t bit = 0; bit < 16; bit++) {
		const uint32_t mask = 1u << bit;
		if (!(held & mask)) {
			held_frames[bit] = 0;
			continue;
		}
		const uint32_t n = ++held_frames[bit];
		if (!(mask & REPEATING) || n <= REPEAT_DELAY)
			continue;
		const uint32_t every = n > FAST_AFTER ? REPEAT_FAST : REPEAT_SLOW;
		if ((n - REPEAT_DELAY) % every == 0)
			pressed |= mask;
	}
	previous = held;
	input->pressed = pressed;
	input->held = held;
	input->right_x = rx > 1.0f ? 1.0f : rx < -1.0f ? -1.0f : rx;
	input->right_y = ry > 1.0f ? 1.0f : ry < -1.0f ? -1.0f : ry;
	input->connected = connected;
	input->idle_frames = (held || rx != 0.0f || ry != 0.0f) ? 0 : input->idle_frames + 1;
}
