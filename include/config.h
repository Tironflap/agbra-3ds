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
} Config;

extern Config g_cfg;

void config_load(void);
void config_save(void);
