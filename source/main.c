#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "app.h"
#include "audio.h"
#include "emu.h"
#include "fs.h"
#include "http.h"
#include "input.h"
#include "overlay.h"
#include "ra.h"
#include "ui.h"
#include "video.h"

#include <3ds.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* These symbols belong to libmgba's 3DS port. Defining them here stops
   ctru-heap.c from being linked: that file opens the SD archive again,
   which is what kills the fs sysmodule (prefetch abort, process "fs").
   The library is built with FIXED_ROM_BUFFER, so the ROM is copied into
   romBuffer instead of being mapped from the file. */
uint32_t* romBuffer = NULL;
size_t romBufferSize = 0;
FS_Archive sdmcArchive;
u32 __stacksize__ = 384 * 1024;

void userAppInit(void) {
	/* 16MB covers retail GBA carts and leaves RAM for audio and the core.
	   A 32MB reservation was crashing the app once the ROM started. */
	romBuffer = malloc(0x01000000);
	if (romBuffer) romBufferSize = 0x01000000;
	else {
		romBuffer = malloc(0x00800000);
		if (romBuffer) romBufferSize = 0x00800000;
	}
}

static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args) {
	(void)logger;
	(void)category;
	(void)level;
	(void)format;
	(void)args;
}

static struct mLogger s_quiet = { quiet_log };

#include "config.h"

App g_app;

void app_set_status(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_app.status, sizeof(g_app.status), fmt, ap);
	va_end(ap);
}

void app_toast(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_app.toast, sizeof(g_app.toast), fmt, ap);
	va_end(ap);
	g_app.toastFrames = 180;
	overlay_push(AGBRA_NAME, g_app.toast);
}

void app_ensure_dirs(void) {
	fs_mkdir_p(AGBRA_DIR);
	fs_mkdir_p(AGBRA_SAVES_DIR);
	fs_mkdir_p(AGBRA_STATES_DIR);
	fs_mkdir_p(AGBRA_ROMS_DIR);
}

void app_basename(const char* path, char* out, size_t outLen) {
	const char* slash = strrchr(path, '/');
	const char* name = slash ? slash + 1 : path;
	strncpy(out, name, outLen - 1);
	out[outLen - 1] = 0;
}

void app_stem(const char* path, char* out, size_t outLen) {
	app_basename(path, out, outLen);
	char* dot = strrchr(out, '.');
	if (dot) *dot = 0;
}

char* app_swkbd(const char* hint, const char* initial, bool password, size_t maxLen) {
	static char buf[256];
	if (maxLen >= sizeof(buf)) maxLen = sizeof(buf) - 1;
	memset(buf, 0, sizeof(buf));
	SwkbdState swkbd;
	swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, (int)maxLen);
	swkbdSetHintText(&swkbd, hint);
	if (initial) swkbdSetInitialText(&swkbd, initial);
	if (password) swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
	swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 1);
	SwkbdButton button = swkbdInputText(&swkbd, buf, maxLen + 1);
	if (button != SWKBD_BUTTON_CONFIRM || !buf[0]) return NULL;
	return buf;
}

static void tick_play(u32 down, u32 held) {
	if (input_menu_combo(held) && (down & (KEY_DOWN | KEY_START | KEY_SELECT))) {
		g_app.resumeScreen = APP_PLAYING;
		g_app.screen = APP_MENU;
		g_app.paused = true;
		return;
	}

	if (!g_app.hardcore) {
		if (down & KEY_X) {
			if (emu_save_state(1)) app_toast("State saved (slot 1)");
			else app_toast("Save state failed");
		}
		if (down & KEY_Y) {
			if (emu_load_state(1)) app_toast("State loaded (slot 1)");
			else app_toast("Load state failed");
		}
		g_app.fastForward = (held & KEY_ZL) != 0;
	} else {
		g_app.fastForward = false;
	}

	emu_set_keys(input_gba_keys(held));

	if (g_app.raDelay > 0) {
		if (--g_app.raDelay == 0) {
			size_t sz = 0;
			const uint8_t* rom = emu_rom_data(&sz);
			ra_load_game(g_app.romPath, rom, sz);
			emu_release_rom();
		}
	} else {
		emu_release_rom();
	}

	int bursts = g_app.fastForward ? 4 : 1;
	bool drew = false;
	u64 t0 = osGetTime();
	for (int i = 0; i < bursts; i++) {
		if (emu_run_frame()) drew = true;
		ra_frame();
		video_emu_frame();
	}
	emu_audio_pump();

	if (drew) {
		video_present(emu_pixels(), emu_stride());
		overlay_draw();
		video_flush_nosync();
	}
	int work = (int)(osGetTime() - t0);

	if (!g_app.fastForward) {
		static int acc = 0;
		static int accN = 0;
		static u64 deadline = 0;
		acc += work;
		if (++accN >= emu_frameskip() + 1) {
			emu_note_frame_ms(acc / accN);
			acc = 0;
			accN = 0;
		}
		int target = emu_target_ms();
		u64 now = osGetTime();
		if (deadline == 0 || now + 100 < deadline) deadline = now;
		if (now < deadline) {
			int slack = (int)(deadline - now);
			if (slack >= 6) svcSleepThread((u64)(slack - 2) * 1000000ULL);
			while (osGetTime() < deadline) {
			}
		} else if (now > deadline + (u64)target) {
			deadline = now;
		}
		deadline += (u64)target;
		int fps = video_fps();
		if (fps >= 35 && fps <= 60) audio_set_rate(32768.0f * (float)fps / 60.0f);
	}
}

static int s_netTick = 0;

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	mLogSetDefaultLogger(&s_quiet);

	video_init();
	/* gfxInit brings apt up. The 804 MHz request is ignored before that,
	   which leaves a New 3DS at old-3DS speed and the game in slow motion. */
	osSetSpeedupEnable(true);
	ui_init();
	audio_init();
	http_init();
	app_ensure_dirs();
	config_load();

	memset(&g_app, 0, sizeof(g_app));
	g_app.running = true;
	g_app.screen = APP_BROWSER;
	g_app.hardcore = g_cfg.hardcore;
	g_app.unofficial = g_cfg.unofficial;
	g_app.scale = (ScaleMode)g_cfg.scale;
	app_set_status("Put .gba files in sdmc:/roms/gba");

	if (!emu_init()) {
		app_set_status("mGBA failed to init. Run ./fetch-deps.sh then rebuild.");
	}
	ra_init();
	if (g_cfg.username[0] && g_cfg.token[0]) {
		ra_login_token(g_cfg.username, g_cfg.token);
	}

	while (aptMainLoop() && g_app.running) {
		hidScanInput();
		u32 down = hidKeysDown();
		u32 held = hidKeysHeld();

		if ((down & KEY_START) && g_app.screen == APP_BROWSER) {
			break;
		}

		bool playing = g_app.screen == APP_PLAYING && emu_loaded() && !g_app.paused && !g_app.loading;
		if (!g_app.loading && (!playing || (s_netTick++ & 7) == 0)) {
			http_poll();
			ra_poll_http();
			ra_idle();
		}

		if (g_app.toastFrames > 0) g_app.toastFrames--;

		if (g_app.loading) {
			int step = emu_load_pump();
			app_set_status("%s", emu_load_phase());
			video_log(emu_load_phase());
			if (step == 2) {
				g_app.loading = false;
				g_app.paused = false;
				g_app.screen = APP_PLAYING;
				g_app.raDelay = ra_logged_in() ? 90 : 0;
				audio_clear();
				video_wipe();
				ui_show_playing();
			} else if (step == 0) {
				g_app.loading = false;
			}
		}

		if (g_app.screen == APP_PLAYING && emu_loaded() && !g_app.paused) {
			tick_play(down, held);
		} else {
			if (g_app.screen == APP_PLAYING && emu_loaded()) {
				video_present(emu_pixels(), emu_stride());
				overlay_draw();
			}
			ui_handle(down, held);
			ui_draw();
		}
	}

	emu_flush_save();
	ra_exit();
	emu_exit();
	http_exit();
	audio_exit();
	video_exit();
	config_save();
	return 0;
}
