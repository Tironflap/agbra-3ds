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

extern uint32_t* romBuffer;
extern size_t romBufferSize;


static struct mCore* s_core = NULL;
static mColor* s_pixels = NULL;
static uint8_t* s_rom = NULL;
static size_t s_romSize = 0;
static char s_path[512];
static char s_savePath[512];
static struct VFile* s_savVf = NULL;
static bool s_loaded = false;

static void free_rom_copy(void) {
	/* Points at romBuffer, which lives until the process exits. */
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
	return s_pixels;
}

int emu_stride(void) {
	return GBA_WIDTH;
}

const uint8_t* emu_rom_data(size_t* size) {
	if (size) *size = s_romSize;
	return s_rom;
}

void emu_release_rom(void) {
	free_rom_copy();
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
	if (s_savVf) {
		s_savVf->close(s_savVf);
		s_savVf = NULL;
	}
	free_rom_copy();
	s_loaded = false;
	s_path[0] = 0;
}

static bool cache_rom(const char* path) {
	free_rom_copy();
	if (!romBuffer) return false;
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long sz = ftell(f);
	fclose(f);
	if (sz <= 0 || (size_t)sz > romBufferSize) return false;
	/* Bytes already sit in romBuffer. Hash that, don't read the ROM twice. */
	s_rom = (uint8_t*)romBuffer;
	s_romSize = (size_t)sz;
	return true;
}

static int s_step = 0;
static char s_phase[64];

void emu_load_begin(const char* path) {
	emu_unload();
	strncpy(s_path, path, sizeof(s_path) - 1);
	s_path[sizeof(s_path) - 1] = 0;
	s_step = 1;
	snprintf(s_phase, sizeof(s_phase), "next: find core");
}

const char* emu_load_phase(void) {
	return s_phase;
}

int emu_load_pump(void) {
	switch (s_step) {
	case 1:
		s_core = mCoreFind(s_path);
		if (!s_core) {
			snprintf(s_phase, sizeof(s_phase), "fail: not a GBA ROM");
			s_step = 0;
			return 0;
		}
		s_step = 2;
		snprintf(s_phase, sizeof(s_phase), "next: init");
		return 1;
	case 2:
		if (!s_core->init) {
			snprintf(s_phase, sizeof(s_phase), "fail: null init");
			s_step = 0;
			return 0;
		}
		if (!s_core->init(s_core)) {
			s_core = NULL;
			snprintf(s_phase, sizeof(s_phase), "fail: init");
			s_step = 0;
			return 0;
		}
		s_step = 3;
		snprintf(s_phase, sizeof(s_phase), "next: video buffer");
		return 1;
	case 3:
		s_core->setVideoBuffer(s_core, s_pixels, GBA_WIDTH);
		s_step = 4;
		snprintf(s_phase, sizeof(s_phase), "next: audio buffer");
		return 1;
	case 4:
		s_core->setAudioBufferSize(s_core, 1024);
		s_step = 5;
		snprintf(s_phase, sizeof(s_phase), "next: audio rate");
		return 1;
	case 5: {
		unsigned rate = s_core->audioSampleRate(s_core);
		if (rate) audio_set_rate((float)rate);
		s_step = 6;
		snprintf(s_phase, sizeof(s_phase), "next: load file");
		return 1;
	}
	case 6:
		if (!mCoreLoadFile(s_core, s_path)) {
			snprintf(s_phase, sizeof(s_phase), "fail: load file");
			emu_unload();
			s_step = 0;
			return 0;
		}
		s_step = 7;
		snprintf(s_phase, sizeof(s_phase), "next: bios");
		return 1;
	case 7:
		if (fs_exists(AGBRA_BIOS_PATH)) {
			struct VFile* bios = VFileOpen(AGBRA_BIOS_PATH, O_RDONLY);
			if (bios) s_core->loadBIOS(s_core, bios, 0);
		}
		s_step = 8;
		snprintf(s_phase, sizeof(s_phase), "next: save");
		return 1;
	case 8: {
		char stem[256];
		uint8_t* data = NULL;
		size_t sz = 0;
		app_stem(s_path, stem, sizeof(stem));
		snprintf(s_savePath, sizeof(s_savePath), AGBRA_SAVES_DIR "/%s.sav", stem);
		FILE* sf = fopen(s_savePath, "rb");
		if (sf) {
			fseek(sf, 0, SEEK_END);
			long n = ftell(sf);
			if (n > 0 && n <= 128 * 1024) {
				data = malloc((size_t)n);
				if (data) {
					fseek(sf, 0, SEEK_SET);
					if (fread(data, 1, (size_t)n, sf) == (size_t)n) sz = (size_t)n;
					else {
						free(data);
						data = NULL;
					}
				}
			}
			fclose(sf);
		}
		if (s_savVf) {
			s_savVf->close(s_savVf);
			s_savVf = NULL;
		}
		s_savVf = VFileMemChunk(data, sz);
		free(data);
		if (s_savVf) s_core->loadSave(s_core, s_savVf);
		s_step = 9;
		snprintf(s_phase, sizeof(s_phase), "next: cache rom");
		return 1;
	}
	case 9:
		cache_rom(s_path);
		s_step = 10;
		snprintf(s_phase, sizeof(s_phase), "next: reset");
		return 1;
	case 10: {
		bool new3ds = false;
		APT_CheckNew3DS(&new3ds);
		/* Old 3DS can't draw every GBA frame and still hit full speed. */
		s_core->opts.frameskip = new3ds ? 0 : 1;
		s_core->reset(s_core);
		s_loaded = true;
		s_step = 0;
		snprintf(s_phase, sizeof(s_phase), "ready");
		return 2;
	}
	default:
		snprintf(s_phase, sizeof(s_phase), "fail: bad step");
		s_step = 0;
		return 0;
	}
}

bool emu_load(const char* path) {
	int r;
	emu_load_begin(path);
	do {
		r = emu_load_pump();
	} while (r == 1);
	return r == 2;
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
	if (!s_savVf || !s_savePath[0]) return;
	size_t n = (size_t)s_savVf->size(s_savVf);
	if (!n) return;
	void* p = s_savVf->map(s_savVf, n, 0);
	if (!p) return;
	FILE* f = fopen(s_savePath, "wb");
	if (f) {
		fwrite(p, 1, n, f);
		fclose(f);
	}
	s_savVf->unmap(s_savVf, p, n);
}

bool emu_save_state(int slot) {
	if (!s_core || g_app.hardcore) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf) return false;
	bool ok = mCoreSaveStateNamed(s_core, vf, 0);
	if (ok) {
		size_t n = (size_t)vf->size(vf);
		void* p = n ? vf->map(vf, n, 0) : NULL;
		FILE* f = p ? fopen(path, "wb") : NULL;
		if (f) {
			ok = fwrite(p, 1, n, f) == n;
			fclose(f);
		} else {
			ok = false;
		}
		if (p) vf->unmap(vf, p, n);
	}
	vf->close(vf);
	return ok;
}

bool emu_load_state(int slot) {
	if (!s_core || g_app.hardcore) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	if (n <= 0) {
		fclose(f);
		return false;
	}
	uint8_t* data = malloc((size_t)n);
	if (!data) {
		fclose(f);
		return false;
	}
	fseek(f, 0, SEEK_SET);
	bool readOk = fread(data, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	if (!readOk) {
		free(data);
		return false;
	}
	struct VFile* vf = VFileMemChunk(data, (size_t)n);
	free(data);
	if (!vf) return false;
	bool ok = mCoreLoadStateNamed(s_core, vf, 0);
	vf->close(vf);
	return ok;
}
