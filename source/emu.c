#include "emu.h"
#include "app.h"
#include "audio.h"
#include "config.h"
#include "fastgba.h"
#include "fs.h"
#include "guest.h"
#include "video.h"

#include <3ds.h>
#include <mgba/core/core.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>
#include "rc_consoles.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static struct mCore* s_core = NULL;
static mColor* s_pixels = NULL;
static uint8_t* s_rom = NULL;
static size_t s_romSize = 0;
static char s_path[512];
static char s_savePath[512];
static struct VFile* s_savVf = NULL;
static bool s_loaded = false;
static bool s_config = false;
static int s_fast = 0;
static int s_vw = GBA_WIDTH;
static int s_vh = GBA_HEIGHT;
static int s_stride = GBA_WIDTH;

static int emu_is_gb(void) {
	if (s_fast || guest_kind()) return 0;
	return s_core && s_core->platform && s_core->platform(s_core) == mPLATFORM_GB;
}

static int path_ext(const char* path, const char* ext) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && strcasecmp(dot, ext) == 0;
}

static int path_is_gba(const char* path) {
	return path_ext(path, ".gba") || path_ext(path, ".agb");
}

static int path_is_nes(const char* path) {
	return path_ext(path, ".nes") || path_ext(path, ".unf") ||
		path_ext(path, ".unif") || path_ext(path, ".fds");
}

static int path_is_mini(const char* path) {
	return path_ext(path, ".min");
}

static int rom_is_gbc(const char* path) {
	const char* dot = strrchr(path, '.');
	if (dot && strcasecmp(dot, ".gbc") == 0) return 1;
	if (!dot || strcasecmp(dot, ".gb") != 0) return 0;
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	int flag = 0;
	if (fseek(f, 0x143, SEEK_SET) == 0) flag = fgetc(f);
	fclose(f);
	return flag == 0x80 || flag == 0xC0;
}

static void free_rom_copy(void) {
	s_rom = NULL;
	s_romSize = 0;
}

bool emu_init(void) {
	s_pixels = linearMemAlign(256 * 224 * sizeof(mColor), 0x80);
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
	return s_loaded && (s_core || s_fast || guest_kind());
}

const uint16_t* emu_pixels(void) {
	if (guest_kind()) return guest_pixels();
	if (s_fast) return fastgba_pixels();
	return s_pixels;
}

int emu_stride(void) {
	if (guest_kind()) return guest_stride();
	if (s_fast) return fastgba_stride();
	return s_stride;
}

int emu_width(void) {
	return s_vw;
}

int emu_height(void) {
	return s_vh;
}

int emu_ra_console(void) {
	if (guest_kind() == GUEST_NES) return RC_CONSOLE_NINTENDO;
	if (guest_kind() == GUEST_MINI) return RC_CONSOLE_POKEMON_MINI;
	if (!emu_is_gb()) return RC_CONSOLE_GAMEBOY_ADVANCE;
	if (rom_is_gbc(s_path)) return RC_CONSOLE_GAMEBOY_COLOR;
	return RC_CONSOLE_GAMEBOY;
}

int emu_guest(void) {
	return guest_kind();
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
	if (s_fast || s_core || guest_kind()) emu_flush_save();
	if (guest_kind()) guest_unload();
	if (s_fast) {
		fastgba_unload();
		s_fast = 0;
		video_set_dyn(0);
	}
	video_set_jit(0, 0);
	if (s_core) {
		/* The core closes the save VFile inside deinit. Closing it again
		   frees the buffer before the write is safe to trust. */
		s_savVf = NULL;
		if (s_config) {
			mCoreConfigDeinit(&s_core->config);
			s_config = false;
		}
		s_core->deinit(s_core);
		s_core = NULL;
	} else if (s_savVf) {
		s_savVf->close(s_savVf);
		s_savVf = NULL;
	}
	free_rom_copy();
	s_loaded = false;
	s_path[0] = 0;
}

static bool cache_rom(const char* path) {
	/* RetroAchievements hashes the file on the card. Keeping a second
	   copy of the cart in romBuffer used to alias unmapped memory and
	   held 16MB for the whole session. */
	(void)path;
	free_rom_copy();
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
		if (path_is_gba(s_path)) {
			char stem[256];
			app_stem(s_path, stem, sizeof(stem));
			snprintf(s_savePath, sizeof(s_savePath), AGBRA_SAVES_DIR "/%s.sav", stem);
			/* gpSP recompiles ARM7 to ARM11. The mGBA interpreter is what
			   holds an old 3DS near 15 fps. If the dynarec cannot map
			   executable pages, this returns false and mGBA still loads. */
			if (fastgba_load(s_path, s_savePath)) {
				s_fast = 1;
				s_vw = GBA_WIDTH;
				s_vh = GBA_HEIGHT;
				s_stride = fastgba_stride();
				video_set_skip(0);
				video_set_dyn(1);
				video_set_jit(0, 0);
				s_loaded = true;
				s_step = 0;
				snprintf(s_phase, sizeof(s_phase), "ready");
				return 2;
			}
			{
				extern int gpsp_jit_result;
				video_set_jit((unsigned)gpsp_jit_result, 1);
				app_set_status("Dynarec unavailable (%08lx) — interpreter",
					(unsigned long)gpsp_jit_result);
			}
		}
		if (path_is_nes(s_path) || path_is_mini(s_path)) {
			char stem[256];
			int mini = path_is_mini(s_path);
			app_stem(s_path, stem, sizeof(stem));
			snprintf(s_savePath, sizeof(s_savePath),
				AGBRA_SAVES_DIR "/%s.%s", stem, mini ? "eep" : "sav");
			if (guest_load(mini ? GUEST_MINI : GUEST_NES, s_path, s_savePath)) {
				s_vw = guest_width();
				s_vh = guest_height();
				s_stride = guest_stride();
				video_set_skip(0);
				video_set_dyn(0);
				s_loaded = true;
				s_step = 0;
				snprintf(s_phase, sizeof(s_phase), "ready");
				return 2;
			}
			snprintf(s_phase, sizeof(s_phase), mini ? "fail: pokemon mini" : "fail: nes");
			s_step = 0;
			return 0;
		}
		s_core = mCoreFind(s_path);
		if (!s_core) {
			snprintf(s_phase, sizeof(s_phase), "fail: not a ROM");
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
		/* Reset looks up "gba.forceGbp" in this table. Uninitialized, it data-aborts. */
		mCoreInitConfig(s_core, "agbra");
		s_config = true;
		s_core->opts.useBios = false;
		s_step = 3;
		snprintf(s_phase, sizeof(s_phase), "next: video buffer");
		return 1;
	case 3:
		if (emu_is_gb()) {
			s_vw = 160;
			s_vh = 144;
			s_stride = 160;
		} else {
			s_vw = GBA_WIDTH;
			s_vh = GBA_HEIGHT;
			s_stride = GBA_WIDTH;
		}
		s_core->setVideoBuffer(s_core, s_pixels, (size_t)s_stride);
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
	case 7: {
		const char* biosPath = NULL;
		if (emu_is_gb()) biosPath = rom_is_gbc(s_path) ? AGBRA_GBC_BIOS_PATH : AGBRA_GB_BIOS_PATH;
		else biosPath = AGBRA_BIOS_PATH;
		if (biosPath && fs_exists(biosPath)) {
			struct VFile* bios = VFileOpen(biosPath, O_RDONLY);
			if (bios && !s_core->loadBIOS(s_core, bios, 0)) bios->close(bios);
		}
		s_step = 8;
		snprintf(s_phase, sizeof(s_phase), "next: save");
		return 1;
	}
	case 8: {
		char stem[256];
		uint8_t* data = NULL;
		size_t sz = 0;
		app_stem(s_path, stem, sizeof(stem));
		if (emu_is_gb()) {
			const char* dot = strrchr(s_path, '.');
			if (!dot) dot = "";
			snprintf(s_savePath, sizeof(s_savePath), AGBRA_SAVES_DIR "/%s%s.sav", stem, dot);
		} else {
			snprintf(s_savePath, sizeof(s_savePath), AGBRA_SAVES_DIR "/%s.sav", stem);
		}
		int sfd = open(s_savePath, O_RDONLY);
		if (sfd >= 0) {
			off_t n = lseek(sfd, 0, SEEK_END);
			if (n > 0 && n <= 128 * 1024) {
				data = malloc((size_t)n);
				if (data) {
					lseek(sfd, 0, SEEK_SET);
					if (read(sfd, data, (size_t)n) == n) sz = (size_t)n;
					else {
						free(data);
						data = NULL;
					}
				}
			}
			close(sfd);
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
		emu_apply_options();
		s_core->reset(s_core);
		if (s_core->currentVideoSize) {
			unsigned w = 0, h = 0;
			s_core->currentVideoSize(s_core, &w, &h);
			if (w >= 1 && h >= 1 && w <= 256 && h <= 224) {
				s_vw = (int)w;
				s_vh = (int)h;
			}
		}
		{
			unsigned rate = s_core->audioSampleRate(s_core);
			if (rate) audio_set_rate((float)rate);
		}
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

static int s_liveSkip = 0;
static bool s_old3ds = true;
static bool s_known3ds = false;

static void emu_check_3ds(void) {
	if (s_known3ds) return;
	bool newer = false;
	APT_CheckNew3DS(&newer);
	s_old3ds = !newer;
	s_known3ds = true;
}

int emu_target_ms(void) {
	emu_check_3ds();
	/* 60 Hz fits on a New 3DS. An old 3DS locks to just over 30 when it
	   cannot finish a frame inside one vblank. */
	return s_old3ds ? 32 : 16;
}

static void emu_push_frameskip(int skip) {
	if (skip < 0) skip = 0;
	if (skip > 9) skip = 9;
	if (!s_core || !s_config || skip == s_liveSkip) {
		s_liveSkip = skip;
		return;
	}
	s_liveSkip = skip;
	s_core->opts.frameskip = skip;
	mCoreConfigSetIntValue(&s_core->config, "frameskip", skip);
	if (s_core->reloadConfigOption) s_core->reloadConfigOption(s_core, "frameskip", &s_core->config);
	video_set_skip(skip);
}

void emu_apply_options(void) {
	if (!s_core || !s_config) return;
	emu_check_3ds();
	int skip = g_cfg.frameskip;
	/* Auto starts at 0. Skipping four frames made the picture update at
	   about 10 Hz while the counter still claimed ~50. */
	if (skip < 0) skip = 0;
	if (skip > 9) skip = 9;
	s_liveSkip = -1;
	emu_push_frameskip(skip);

	/* Hardware keeps idle loops. An old 3DS detects them so busy-wait
	   games (Pokemon and most menus) are not stuck interpreting spin. */
	const char* idle = "remove";
	if (g_cfg.accuracy >= 2) idle = "ignore";
	else if (g_cfg.accuracy <= 0 || s_old3ds) idle = "detect";
	mCoreConfigSetValue(&s_core->config, "idleOptimization", idle);
	if (emu_is_gb()) mCoreConfigSetIntValue(&s_core->config, "sgb.borders", 0);
	/* opts is zeroed. loadConfig copies volume straight through, so 0 mutes the GBA. */
	s_core->opts.volume = 0x100;
	s_core->opts.mute = false;
	if (s_core->loadConfig) s_core->loadConfig(s_core, &s_core->config);
}

void emu_note_frame_ms(int ms) {
	/* The dynarec is paced by vblank. Auto-skip during the first
	   compiled blocks would stick and leave the picture at 30 Hz. */
	if (s_fast || guest_kind()) return;
	if (g_cfg.frameskip >= 0) return;
	if (ms < 1) ms = 1;
	/* score is the emulated rate this batch would hold if we didn't sleep.
	   A higher skip only sticks when it actually gets the game faster and
	   still clears 30 emulated fps. A failed try is pinned so it cannot
	   oscillate every other frame. */
	static int pinSkip = -1;
	static int pinLeft = 0;
	static int prevSkip = 0;
	static int prevScore = 0;
	static int changed = 0;
	static int cool = 0;
	int frames = (s_liveSkip < 0 ? 0 : s_liveSkip) + 1;
	int score = frames * 1000 / ms;
	if (pinLeft > 0) pinLeft--;

	if (changed) {
		changed = 0;
		int better = score >= prevScore + 4 && score >= 30;
		if (!better) {
			pinSkip = s_liveSkip;
			pinLeft = 90;
			emu_push_frameskip(prevSkip);
			return;
		}
		prevScore = score;
		prevSkip = s_liveSkip;
		cool = 0;
		return;
	}
	prevScore = score;
	prevSkip = s_liveSkip;

	if (s_liveSkip == 0 && ms > 16 && ms <= 30) {
		if (!(pinSkip == 1 && pinLeft > 0)) {
			changed = 1;
			emu_push_frameskip(1);
		}
		return;
	}
	if (ms > 32 && s_liveSkip < 3) {
		int next = s_liveSkip + 1;
		if (pinSkip == next && pinLeft > 0) return;
		changed = 1;
		cool = 0;
		emu_push_frameskip(next);
		return;
	}
	if (ms < 26 && s_liveSkip > 0) {
		if (++cool >= 45) {
			cool = 0;
			changed = 1;
			emu_push_frameskip(s_liveSkip - 1);
		}
	} else {
		cool = 0;
	}
}

int emu_frameskip(void) {
	if (s_fast || guest_kind()) return 0;
	return s_liveSkip < 0 ? 0 : s_liveSkip;
}

int emu_is_fast(void) {
	/* NES and Pokemon Mini also finish a frame inside one refresh.
	   A second vblank wait would cap them at 30 Hz. */
	return s_fast || guest_kind();
}

bool emu_run_frame(void) {
	if (guest_kind()) {
		guest_run();
		s_vw = guest_width();
		s_vh = guest_height();
		s_stride = guest_stride();
		return true;
	}
	if (s_fast) {
		fastgba_run();
		return true;
	}
	if (!s_core) return false;
	/* The core's own frameskip drops scanline draws. The frontend batches
	   skip+1 calls and presents once, so a second skip counter here only
	   hid the frame the core had just rendered. */
	s_core->runFrame(s_core);
	return true;
}

void emu_reset(void) {
	if (guest_kind()) {
		guest_reset();
		return;
	}
	if (s_fast) {
		fastgba_reset();
		return;
	}
	if (s_core) s_core->reset(s_core);
}

void emu_set_keys(uint32_t keys) {
	if (guest_kind()) {
		guest_set_keys(keys);
		return;
	}
	if (s_fast) {
		fastgba_set_keys(keys);
		return;
	}
	if (s_core) s_core->setKeys(s_core, keys);
}

uint8_t emu_read8(uint32_t address) {
	if (guest_kind()) return guest_read8(address);
	if (s_fast) return fastgba_read8(address);
	if (!s_core) return 0;
	return (uint8_t)s_core->busRead8(s_core, address);
}

void emu_read(uint32_t address, uint8_t* buf, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) buf[i] = emu_read8(address + i);
}

void emu_audio_pump(void) {
	if (s_fast || guest_kind() || !s_core) return;
	struct mAudioBuffer* buf = s_core->getAudioBuffer(s_core);
	if (!buf) return;
	int16_t tmp[2048];
	size_t n = mAudioBufferRead(buf, tmp, 1024);
	if (n) audio_push(tmp, n);
}

static void flush_save(int sync) {
	if (guest_kind()) {
		guest_flush_save(s_savePath, sync);
		return;
	}
	if (s_fast) {
		fastgba_flush_save(s_savePath, sync);
		return;
	}
	if (!s_savVf || !s_savePath[0]) return;
	size_t n = (size_t)s_savVf->size(s_savVf);
	if (!n || n > 128 * 1024) return;
	void* p = s_savVf->map(s_savVf, n, 0);
	if (!p) return;
	fs_mkdir_p(AGBRA_SAVES_DIR);
	int fd = open(s_savePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) {
		size_t off = 0;
		while (off < n) {
			ssize_t wr = write(fd, (uint8_t*)p + off, n - off);
			if (wr <= 0) break;
			off += (size_t)wr;
		}
		/* fsync on the SD card stalls the emulator for a frame or two.
		   The periodic write leaves it in the cache. Pause and exit sync. */
		if (sync) fsync(fd);
		close(fd);
	}
	s_savVf->unmap(s_savVf, p, n);
}

void emu_flush_save(void) {
	flush_save(1);
}

void emu_flush_save_soft(void) {
	flush_save(0);
}

bool emu_save_state(int slot) {
	if (g_app.hardcore) return false;
	if (guest_kind()) {
		char stem[256], path[512];
		const char* tag = guest_kind() == GUEST_MINI ? "min" : "nes";
		app_stem(s_path, stem, sizeof(stem));
		snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.%s.ss%d", stem, tag, slot);
		return guest_save_state(path);
	}
	if (s_fast) {
		char stem[256], path[512];
		app_stem(s_path, stem, sizeof(stem));
		snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
		return fastgba_save_state(path);
	}
	if (!s_core) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!vf) return false;
	bool ok = mCoreSaveStateNamed(s_core, vf, 0);
	if (ok) {
		size_t n = (size_t)vf->size(vf);
		void* p = n ? vf->map(vf, n, 0) : NULL;
		int fd = p ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
		if (fd >= 0) {
			ok = write(fd, p, n) == (ssize_t)n;
			close(fd);
		} else {
			ok = false;
		}
		if (p) vf->unmap(vf, p, n);
	}
	vf->close(vf);
	return ok;
}

bool emu_load_state(int slot) {
	if (g_app.hardcore) return false;
	if (guest_kind()) {
		char stem[256], path[512];
		const char* tag = guest_kind() == GUEST_MINI ? "min" : "nes";
		app_stem(s_path, stem, sizeof(stem));
		snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.%s.ss%d", stem, tag, slot);
		return guest_load_state(path);
	}
	if (s_fast) {
		char stem[256], path[512];
		app_stem(s_path, stem, sizeof(stem));
		snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
		return fastgba_load_state(path);
	}
	if (!s_core) return false;
	char stem[256], path[512];
	app_stem(s_path, stem, sizeof(stem));
	snprintf(path, sizeof(path), AGBRA_STATES_DIR "/%s.ss%d", stem, slot);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	off_t n = lseek(fd, 0, SEEK_END);
	if (n <= 0) {
		close(fd);
		return false;
	}
	uint8_t* data = malloc((size_t)n);
	if (!data) {
		close(fd);
		return false;
	}
	lseek(fd, 0, SEEK_SET);
	bool readOk = read(fd, data, (size_t)n) == n;
	close(fd);
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
