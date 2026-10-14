#include "input.h"

enum {
	GBA_KEY_A = 0,
	GBA_KEY_B = 1,
	GBA_KEY_SELECT = 2,
	GBA_KEY_START = 3,
	GBA_KEY_RIGHT = 4,
	GBA_KEY_LEFT = 5,
	GBA_KEY_UP = 6,
	GBA_KEY_DOWN = 7,
	GBA_KEY_R = 8,
	GBA_KEY_L = 9
};

uint32_t input_gba_keys(u32 held) {
	uint32_t keys = 0;
	if (held & KEY_A) keys |= 1u << GBA_KEY_A;
	if (held & KEY_B) keys |= 1u << GBA_KEY_B;
	if (held & KEY_START) keys |= 1u << GBA_KEY_START;
	if (held & KEY_SELECT) keys |= 1u << GBA_KEY_SELECT;
	if (held & KEY_DUP) keys |= 1u << GBA_KEY_UP;
	if (held & KEY_DDOWN) keys |= 1u << GBA_KEY_DOWN;
	if (held & KEY_DLEFT) keys |= 1u << GBA_KEY_LEFT;
	if (held & KEY_DRIGHT) keys |= 1u << GBA_KEY_RIGHT;
	if (held & KEY_L) keys |= 1u << GBA_KEY_L;
	if (held & KEY_R) keys |= 1u << GBA_KEY_R;

	circlePosition stick;
	hidCircleRead(&stick);
	const int dead = 40;
	if (stick.dy > dead) keys |= 1u << GBA_KEY_UP;
	if (stick.dy < -dead) keys |= 1u << GBA_KEY_DOWN;
	if (stick.dx < -dead) keys |= 1u << GBA_KEY_LEFT;
	if (stick.dx > dead) keys |= 1u << GBA_KEY_RIGHT;
	return keys;
}

bool input_menu_combo(u32 held) {
	return (held & KEY_L) && (held & KEY_R);
}
