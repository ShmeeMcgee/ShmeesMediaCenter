// Shmees Media Center: the controller. Reads every connected pad, so any of the four drives the UI, and turns the D-pad
// and left stick into presses that repeat while held.
#ifndef SMC_INPUT_H
#define SMC_INPUT_H

#include <stdbool.h>
#include <stdint.h>

enum {
	IN_UP = 1 << 0,
	IN_DOWN = 1 << 1,
	IN_LEFT = 1 << 2,
	IN_RIGHT = 1 << 3,
	IN_A = 1 << 4,
	IN_B = 1 << 5,
	IN_X = 1 << 6,
	IN_Y = 1 << 7,
	IN_LB = 1 << 8,
	IN_RB = 1 << 9,
	IN_LT = 1 << 10,
	IN_RT = 1 << 11,
	IN_START = 1 << 12,
	IN_BACK = 1 << 13,
	IN_LS = 1 << 14,
	IN_RS = 1 << 15,
};

typedef struct {
	// Pressed this frame, the directions and triggers also as they repeat while held.
	uint32_t pressed;
	// Held now.
	uint32_t held;
	// The right stick, -1 to 1, its dead zone taken out: for scrubbing and scrolling.
	float right_x;
	float right_y;
	// Whether any pad is connected.
	bool connected;
	// Frames since the last input of any kind, for hiding the player's overlay.
	uint32_t idle_frames;
} Input;

void input_poll(Input *input);

#endif
