#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
	GUEST_NONE = 0,
	GUEST_NES = 1,
	GUEST_MINI = 2
};

bool guest_load(int kind, const char* romPath, const char* savePath);
void guest_unload(void);
int guest_kind(void);
void guest_run(void);
void guest_reset(void);
void guest_set_keys(uint32_t keys);
uint8_t guest_read8(uint32_t address);
const uint16_t* guest_pixels(void);
int guest_stride(void);
int guest_width(void);
int guest_height(void);
bool guest_save_state(const char* path);
bool guest_load_state(const char* path);
void guest_flush_save(const char* path, int sync);
