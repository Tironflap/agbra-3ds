#include "emu.h"
#include "app.h"
#include "audio.h"
#include "fs.h"

#include <3ds.h>
#include <mgba/core/core.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static struct mCore* s_core = NULL;
static mColor* s_pixels = NULL;
static uint8_t* s_rom = NULL;
static size_t s_romSize = 0;
static char s_path[512];
static char s_savePath[512];
static bool s_loaded = false;

static void free_rom_copy(void) {
	free(s_rom);
	s_rom = NULL;
	s_romSize = 0;
}

bool emu_init(void) {
	s_pixels = linearMemAlign((size_t)GBA_WIDTH * GBA_HEIGHT * sizeof(mColor), 0x80);
	return s_pixels != NULL;
}

void emu_exit(void) {
	emu_unload();
	if (s_pixels) {
		linearFree(s_pixels);
		s_pixels = NULL;
	}
}

bool emu_loaded(void) {
	return s_loaded && s_core;
}

const uint16_t* emu_pixels(void) {
	return (const uint16_t*)s_pixels;
}

int emu_stride(void) {
	return GBA_WIDTH;
}

const uint8_t* emu_rom_data(size_t* size) {
	if (size) *size = s_romSize;
	return s_rom;
}

const char* emu_rom_path(void) {
	return s_path;
}

void emu_unload(void) {
	if (s_core) {
		emu_flush_save();
		s_core->deinit(s_core);
		s_core = NULL;
	}
	free_rom_copy();
	s_loaded = false;
	s_path[0] = 0;
}

static bool cache_rom(const char* path) {
	free_rom_copy();
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 32 * 1024 * 1024) {
		fclose(f);
		return false;
	}
	s_rom = malloc((size_t)sz);
	if (!s_rom) {
		fclose(f);
		return false;
	}
	if (fread(s_rom, 1, (size_t)sz, f) != (size_t)sz) {
		free_rom_copy();
		fclose(f);
		return false;
	}
	fclose(f);
	s_romSize = (size_t)sz;
	return true;
}

bool emu_load(const char* path) {
	emu_unload();
	strncpy(s_path, path, sizeof(s_path) - 1);
	s_path[sizeof(s_path) - 1] = 0;

	s_core = mCoreFind(path);
	if (!s_core) {
		app_set_status("Not a GBA ROM");
		return false;
	}
	if (!s_core->init(s_core)) {
		s_core = NULL;
		app_set_status("mGBA init failed");
		return false;
	}

	s_core->setVideoBuffer(s_core, s_pixels, GBA_WIDTH);
	s_core->setAudioBufferSize(s_core, 1024);
	unsigned rate = s_core->audioSampleRate(s_core);
	if (rate) audio_set_rate((float)rate);

	if (!mCoreLoadFile(s_core, path)) {
		app_set_status("Failed to load ROM");
		emu_unload();
		return false;
	}

	if (fs_exists(AGBRA_BIOS_PATH)) {
		struct VFile* bios = VFileOpen(AGBRA_BIOS_PATH, O_RDONLY);
		if (bios) s_core->loadBIOS(s_core, bios, 0);
	}

	char stem[256];
	app_stem(path, stem, sizeof(stem));
	snprintf(s_savePath, sizeof(s_savePath), AGBRA_SAVES_DIR "/%s.sav", stem);
	struct VFile* sav = VFileOpen(s_savePath, O_CREAT | O_RDWR);
	if (sav) s_core->loadSave(s_core, sav);

	cache_rom(path);
	s_core->reset(s_core);
	s_loaded = true;
	return true;
}

void emu_run_frame(void) {
	if (s_core) s_core->runFrame(s_core);
}

void emu_reset(void) {
	if (s_core) s_core->reset(s_core);
}

void emu_set_keys(uint32_t keys) {
	if (s_core) s_core->setKeys(s_core, keys);
}

uint8_t emu_read8(uint32_t address) {
	if (!s_core) return 0;
	return (uint8_t)s_core->busRead8(s_core, address);
}

void emu_read(uint32_t address, uint8_t* buf, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) buf[i] = emu_read8(address + i);
}

void emu_audio_pump(void) {
	if (!s_core) return;
	struct mAudioBuffer* buf = s_core->getAudioBuffer(s_core);
	if (!buf) return;
	int16_t tmp[2048];
	size_t n = mAudioBufferRead(buf, tmp, 1024);
	if (n) audio_push(tmp, n);
}

void emu_flush_save(void) {
	/* mGBA writeback happens through the VFile kept open by loadSave. */
}

bool emu_save_state(int slot) {
	if (!s_core || g_app.hardcore) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
	if (!vf) return false;
	bool ok = mCoreSaveStateNamed(s_core, vf, 0);
	vf->close(vf);
	return ok;
}

bool emu_load_state(int slot) {
	if (!s_core || g_app.hardcore) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) return false;
	bool ok = mCoreLoadStateNamed(s_core, vf, 0);
	vf->close(vf);
	return ok;
}
