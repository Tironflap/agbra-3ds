#include "ui.h"
#include "app.h"
#include "audio.h"
#include "config.h"
#include "emu.h"
#include "fs.h"
#include "ra.h"
#include "video.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static RomEntry s_roms[FS_MAX_ROMS];
static int s_romCount = 0;
static bool s_scanned = false;

static const char* MENU_ITEMS[] = {
	"Resume",
	"Achievements",
	"Reset game",
	"Toggle hardcore",
	"Scale mode",
	"Frameskip",
	"Accuracy",
	"Close ROM",
	"Quit"
};
#define MENU_N 9

static const char* SCALE_NAME[] = { "1x centered", "Fit 360x240", "Fill 400x240" };
static const char* SKIP_NAME[] = { "Auto", "Off", "1", "2", "3", "4", "5" };
static const char* ACC_NAME[] = { "Fast", "Normal", "Hardware" };

static const char* frameskip_label(void) {
	int s = g_cfg.frameskip + 1;
	if (s < 0) s = 0;
	if (s > 6) s = 6;
	return SKIP_NAME[s];
}

static const char* accuracy_label(void) {
	int a = g_cfg.accuracy;
	if (a < 0 || a > 2) a = 1;
	return ACC_NAME[a];
}

static void cycle_frameskip(void) {
	if (g_cfg.frameskip >= 5) g_cfg.frameskip = -1;
	else g_cfg.frameskip++;
	config_save();
	emu_apply_options();
	app_set_status("Frameskip %s", frameskip_label());
}

static void cycle_accuracy(void) {
	g_cfg.accuracy = (g_cfg.accuracy + 1) % 3;
	config_save();
	emu_apply_options();
	app_set_status("Accuracy %s", accuracy_label());
}

static void scan_if_needed(void) {
	if (s_scanned) return;
	s_romCount = fs_scan_roms(s_roms, FS_MAX_ROMS);
	s_scanned = true;
	if (g_app.browserIndex >= s_romCount) g_app.browserIndex = 0;
}

static void cls(void) {
	consoleClear();
	printf("\x1b[0;0H");
}

static void header(void) {
	printf("\x1b[33m%s\x1b[0m  v%s\n", AGBRA_NAME, AGBRA_VERSION);
	if (ra_logged_in()) {
		printf("RA  \x1b[32m%s\x1b[0m  %" PRIu32 " pts  %s\n",
			ra_user_name(), ra_user_score(),
			g_app.hardcore ? "HARDCORE" : "softcore");
	} else {
		printf("RA  \x1b[31mnot signed in\x1b[0m\n");
	}
	printf("--------------------------------\n");
}

static void footer_status(void) {
	printf("--------------------------------\n");
	if (g_app.status[0]) printf("%s\n", g_app.status);
}

static void load_rom_at(int index) {
	if (index < 0 || index >= s_romCount || g_app.loading) return;
	strncpy(g_app.romPath, s_roms[index].path, sizeof(g_app.romPath) - 1);
	g_app.romPath[sizeof(g_app.romPath) - 1] = 0;
	strncpy(g_app.romName, s_roms[index].name, sizeof(g_app.romName) - 1);
	g_app.romName[sizeof(g_app.romName) - 1] = 0;
	emu_load_begin(s_roms[index].path);
	g_app.loading = true;
	app_set_status("%s", emu_load_phase());
}

static void do_login(void) {
	char* user = app_swkbd("RetroAchievements username", g_cfg.username, false, 31);
	if (!user) return;
	char userCopy[32];
	strncpy(userCopy, user, sizeof(userCopy) - 1);
	userCopy[sizeof(userCopy) - 1] = 0;
	char* pass = app_swkbd("Password (not stored)", NULL, true, 63);
	if (!pass) return;
	ra_login(userCopy, pass);
}

static void draw_browser(void) {
	scan_if_needed();
	cls();
	header();
	if (s_romCount == 0) {
		printf("No ROMs found.\n\nPlace games in:\n  sdmc:/roms/gba\n  sdmc:/roms/gb\n\nBIOS (optional):\n  sdmc:/3ds/agbra/gba_bios.bin\n  sdmc:/3ds/agbra/gb_bios.bin\n  sdmc:/3ds/agbra/gbc_bios.bin\n");
	} else {
		const int rows = 18;
		if (g_app.browserIndex < g_app.browserTop) g_app.browserTop = g_app.browserIndex;
		if (g_app.browserIndex >= g_app.browserTop + rows) g_app.browserTop = g_app.browserIndex - rows + 1;
		for (int i = 0; i < rows && g_app.browserTop + i < s_romCount; i++) {
			int idx = g_app.browserTop + i;
			printf("%s%s\x1b[0m\n", idx == g_app.browserIndex ? "\x1b[47;30m> " : "  ", s_roms[idx].name);
		}
	}
	footer_status();
	printf("\nA load  X login  Y refresh  START quit");
}

static void draw_menu(void) {
	cls();
	header();
	printf("%s\n\n", g_app.romName[0] ? g_app.romName : "(no ROM)");
	for (int i = 0; i < MENU_N; i++) {
		printf("%s%s\x1b[0m", i == g_app.menuIndex ? "\x1b[47;30m> " : "  ", MENU_ITEMS[i]);
		if (i == 3) printf("  [%s]", g_app.hardcore ? "ON" : "off");
		if (i == 4) printf("  [%s]", SCALE_NAME[g_app.scale]);
		if (i == 5) printf("  [%s]", frameskip_label());
		if (i == 6) printf("  [%s]", accuracy_label());
		printf("\n");
	}
	footer_status();
	printf("\nA select  B resume  L+R+Down in-game");
}

static void draw_cheevos(void) {
	cls();
	header();
	const char* title = ra_game_title();
	uint32_t unlocked = 0, total = 0, points = 0;
	ra_summary(&unlocked, &total, &points);
	printf("%s\n%" PRIu32 " / %" PRIu32 "   %" PRIu32 " pts\n", title ? title : g_app.romName, unlocked, total, points);
	char rp[120];
	ra_rich_presence(rp, sizeof(rp));
	if (rp[0]) printf("%s\n", rp);
	printf("--------------------------------\n");
	int n = ra_list_count();
	if (n == 0) {
		printf("No achievements loaded.\nSign in and load a supported game.\n");
	} else {
		const int rows = 14;
		if (g_app.cheevoIndex < 0) g_app.cheevoIndex = 0;
		if (g_app.cheevoIndex >= n) g_app.cheevoIndex = n - 1;
		int top = g_app.cheevoIndex - rows / 2;
		if (top < 0) top = 0;
		if (top > n - rows) top = n > rows ? n - rows : 0;
		for (int i = 0; i < rows && top + i < n; i++) {
			int idx = top + i;
			const char* mark = ra_list_unlocked(idx) ? "\x1b[32m*" : "\x1b[31m.";
			printf("%s%s\x1b[0m %s%2d  %s\x1b[0m\n",
				idx == g_app.cheevoIndex ? "\x1b[47;30m" : "",
				mark, idx == g_app.cheevoIndex ? "\x1b[47;30m" : "",
				ra_list_points(idx), ra_list_title(idx));
		}
	}
	footer_status();
	printf("\nB back");
}

static void draw_settings(void) {
	cls();
	header();
	printf("Hardcore    %s\n", g_app.hardcore ? "ON" : "off");
	printf("Unofficial  %s\n", g_app.unofficial ? "ON" : "off");
	printf("Scale       %s\n", SCALE_NAME[g_app.scale]);
	printf("Frameskip   %s\n", frameskip_label());
	printf("Accuracy    %s\n", accuracy_label());
	printf("GBA BIOS    %s\n", fs_exists(AGBRA_BIOS_PATH) ? "found" : "missing (HLE)");
	printf("GB BIOS     %s\n", fs_exists(AGBRA_GB_BIOS_PATH) ? "found" : "missing (HLE)");
	printf("GBC BIOS    %s\n", fs_exists(AGBRA_GBC_BIOS_PATH) ? "found" : "missing (HLE)");
	footer_status();
	printf("\nA hardcore  X unofficial  Y scale\nL frameskip  R accuracy  B back");
}

void ui_init(void) {
}

static void draw_playing(void) {
	cls();
	header();
	printf("%s\n", g_app.romName);
	if (ra_game_title()) printf("%s\n", ra_game_title());
	uint32_t u = 0, t = 0, p = 0;
	ra_summary(&u, &t, &p);
	if (t) printf("%" PRIu32 " / %" PRIu32 " cheevos   %" PRIu32 " pts\n", u, t, p);
	char rp[120];
	ra_rich_presence(rp, sizeof(rp));
	if (rp[0]) printf("%s\n", rp);
	printf("\nL+R+Down  menu\n");
	if (!g_app.hardcore) printf("X save state  Y load  ZL turbo\n");
	else printf("Hardcore: states and turbo locked\n");
	footer_status();
}

void ui_show_playing(void) {
	s_romCount = 0;
	s_scanned = false;
	g_app.browserIndex = 0;
	draw_playing();
	gfxFlushBuffers();
	gfxSwapBuffers();
}

void ui_draw(void) {
	switch (g_app.screen) {
	case APP_BROWSER: draw_browser(); break;
	case APP_LOGIN: draw_browser(); break;
	case APP_MENU: draw_menu(); break;
	case APP_CHEEVOS: draw_cheevos(); break;
	case APP_SETTINGS: draw_settings(); break;
	case APP_PLAYING: draw_playing(); break;
	}
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static void handle_browser(u32 down) {
	scan_if_needed();
	if (down & KEY_UP && g_app.browserIndex > 0) g_app.browserIndex--;
	if (down & KEY_DOWN && g_app.browserIndex + 1 < s_romCount) g_app.browserIndex++;
	if (down & KEY_LEFT) g_app.browserIndex = g_app.browserIndex > 10 ? g_app.browserIndex - 10 : 0;
	if (down & KEY_RIGHT && s_romCount) {
		g_app.browserIndex += 10;
		if (g_app.browserIndex >= s_romCount) g_app.browserIndex = s_romCount - 1;
	}
	if (down & KEY_A && s_romCount) load_rom_at(g_app.browserIndex);
	if (down & KEY_X) do_login();
	if (down & KEY_Y) {
		s_scanned = false;
		scan_if_needed();
		app_set_status("Scanned %d ROM(s)", s_romCount);
	}
	if (down & KEY_SELECT) g_app.screen = APP_SETTINGS;
}

static void handle_menu(u32 down) {
	if (down & KEY_UP && g_app.menuIndex > 0) g_app.menuIndex--;
	if (down & KEY_DOWN && g_app.menuIndex + 1 < MENU_N) g_app.menuIndex++;
	if (down & KEY_B) {
		g_app.paused = false;
		g_app.screen = APP_PLAYING;
		return;
	}
	if (!(down & KEY_A)) return;
	switch (g_app.menuIndex) {
	case 0:
		g_app.paused = false;
		g_app.screen = APP_PLAYING;
		break;
	case 1:
		ra_refresh_list();
		g_app.screen = APP_CHEEVOS;
		break;
	case 2:
		emu_reset();
		ra_reset();
		g_app.paused = false;
		g_app.screen = APP_PLAYING;
		app_toast("Reset");
		break;
	case 3:
		ra_set_hardcore(!g_app.hardcore);
		app_set_status("Hardcore %s", g_app.hardcore ? "ON" : "off");
		break;
	case 4:
		g_app.scale = (ScaleMode)((g_app.scale + 1) % 3);
		g_cfg.scale = (int)g_app.scale;
		config_save();
		break;
	case 5:
		cycle_frameskip();
		break;
	case 6:
		cycle_accuracy();
		break;
	case 7:
		emu_flush_save();
		ra_unload_game();
		emu_unload();
		g_app.screen = APP_BROWSER;
		g_app.romName[0] = 0;
		app_set_status("ROM closed");
		break;
	case 8:
		g_app.running = false;
		break;
	}
}

static void handle_cheevos(u32 down) {
	int n = ra_list_count();
	if (down & KEY_UP && g_app.cheevoIndex > 0) g_app.cheevoIndex--;
	if (down & KEY_DOWN && g_app.cheevoIndex + 1 < n) g_app.cheevoIndex++;
	if (down & KEY_B) g_app.screen = emu_loaded() ? APP_MENU : APP_BROWSER;
}

static void handle_settings(u32 down) {
	if (down & KEY_A) {
		g_app.hardcore = !g_app.hardcore;
		ra_set_hardcore(g_app.hardcore);
	}
	if (down & KEY_X) {
		g_app.unofficial = !g_app.unofficial;
		g_cfg.unofficial = g_app.unofficial;
		config_save();
	}
	if (down & KEY_Y) {
		g_app.scale = (ScaleMode)((g_app.scale + 1) % 3);
		g_cfg.scale = (int)g_app.scale;
		config_save();
	}
	if (down & KEY_L) cycle_frameskip();
	if (down & KEY_R) cycle_accuracy();
	if (down & KEY_B) g_app.screen = APP_BROWSER;
}

void ui_handle(u32 down, u32 held) {
	(void)held;
	switch (g_app.screen) {
	case APP_BROWSER:
	case APP_LOGIN:
		handle_browser(down);
		break;
	case APP_MENU:
		handle_menu(down);
		break;
	case APP_CHEEVOS:
		handle_cheevos(down);
		break;
	case APP_SETTINGS:
		handle_settings(down);
		break;
	case APP_PLAYING:
		if (down & KEY_SELECT) {
			g_app.paused = true;
			g_app.screen = APP_MENU;
		}
		break;
	}
}
