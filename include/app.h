#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGBRA_NAME "AGB-RA"
#define AGBRA_VERSION "1.0.0"
#define AGBRA_DIR "sdmc:/3ds/agbra"
#define AGBRA_ROMS_DIR "sdmc:/roms/gba"
#define AGBRA_GB_ROMS_DIR "sdmc:/roms/gb"
#define AGBRA_BIOS_PATH "sdmc:/3ds/agbra/gba_bios.bin"
#define AGBRA_GB_BIOS_PATH "sdmc:/3ds/agbra/gb_bios.bin"
#define AGBRA_GBC_BIOS_PATH "sdmc:/3ds/agbra/gbc_bios.bin"
#define AGBRA_CFG_PATH "sdmc:/3ds/agbra/config.ini"
#define AGBRA_SAVES_DIR "sdmc:/3ds/agbra/saves"
#define AGBRA_STATES_DIR "sdmc:/3ds/agbra/states"
#define AGBRA_USER_AGENT "AGB-RA/1.0 (Nintendo 3DS)"

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define TOP_WIDTH 400
#define TOP_HEIGHT 240
#define BOT_WIDTH 320
#define BOT_HEIGHT 240

typedef enum {
	APP_BROWSER = 0,
	APP_LOGIN,
	APP_PLAYING,
	APP_MENU,
	APP_CHEEVOS,
	APP_SETTINGS
} AppScreen;

typedef enum {
	SCALE_1X = 0,
	SCALE_FIT,   /* 360x240 nearest */
	SCALE_FILL   /* 400x240 stretch */
} ScaleMode;

typedef struct App {
	AppScreen screen;
	AppScreen resumeScreen;
	bool running;
	bool paused;
	bool hardcore;
	bool unofficial;
	bool fastForward;
	ScaleMode scale;
	int browserIndex;
	int browserTop;
	int cheevoIndex;
	int menuIndex;
	int settingIndex;
	char status[192];
	char toast[160];
	int toastFrames;
	char romName[256];
	char romPath[512];
	int raDelay;
	bool loading;
} App;

extern App g_app;

void app_set_status(const char* fmt, ...);
void app_toast(const char* fmt, ...);
void app_ensure_dirs(void);
char* app_swkbd(const char* hint, const char* initial, bool password, size_t maxLen);
void app_basename(const char* path, char* out, size_t outLen);
void app_stem(const char* path, char* out, size_t outLen);
