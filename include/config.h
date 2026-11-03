#pragma once

#include <stdbool.h>

typedef struct Config {
	char username[32];
	char token[48];
	bool hardcore;
	bool unofficial;
	int scale;
	int frameskip; /* -1 auto, 0 off, 1..5 skip that many */
	int accuracy;  /* 0 fast, 1 normal, 2 hardware */
	char romm_url[192];
	char romm_user[64];
	char romm_token[768];
	char romm_refresh[768];
} Config;

extern Config g_cfg;

void config_load(void);
void config_save(void);
