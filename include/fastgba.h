#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GBA via gpSP's ARM11 dynarec. GB/GBC stay on mGBA. */
bool fastgba_load(const char* romPath, const char* savePath);
void fastgba_unload(void);
bool fastgba_ready(void);
void fastgba_run(void);
void fastgba_reset(void);
void fastgba_set_keys(uint32_t keys);
uint8_t fastgba_read8(uint32_t address);
const uint16_t* fastgba_pixels(void);
int fastgba_stride(void);
bool fastgba_save_state(const char* path);
bool fastgba_load_state(const char* path);
void fastgba_flush_save(const char* path, int sync);
