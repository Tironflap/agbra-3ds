#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool emu_init(void);
void emu_exit(void);
bool emu_load(const char* path);
void emu_unload(void);
bool emu_loaded(void);
void emu_run_frame(void);
void emu_reset(void);
void emu_set_keys(uint32_t keys);
uint8_t emu_read8(uint32_t address);
void emu_read(uint32_t address, uint8_t* buf, uint32_t n);
const uint16_t* emu_pixels(void);
int emu_stride(void);
void emu_audio_pump(void);
bool emu_save_state(int slot);
bool emu_load_state(int slot);
void emu_flush_save(void);
const uint8_t* emu_rom_data(size_t* size);
const char* emu_rom_path(void);
