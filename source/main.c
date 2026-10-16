#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "app.h"
#include "audio.h"
#include "config.h"
#include "emu.h"
#include "fs.h"
#include "http.h"
#include "input.h"
#include "overlay.h"
#include "ra.h"
#include "ui.h"
#include "video.h"

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

	if (g_app.raDelay > 0 && --g_app.raDelay == 0) {
		video_log("ra: hash + load");
		size_t sz = 0;
		const uint8_t* rom = emu_rom_data(&sz);
		ra_load_game(g_app.romPath, rom, sz);
		video_log("ra: request sent");
	}

	int frames = g_app.fastForward ? 3 : 1;
	for (int i = 0; i < frames; i++) {
		emu_run_frame();
		ra_frame();
	}
	emu_audio_pump();
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	osSetSpeedupEnable(true);

	video_init();
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

		if (!g_app.loading) {
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
			} else if (step == 0) {
				g_app.loading = false;
			}
		}

		if (g_app.screen == APP_PLAYING && emu_loaded() && !g_app.paused) {
			tick_play(down, held);
			video_present(emu_pixels(), emu_stride());
			overlay_draw();
			video_flush();
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
