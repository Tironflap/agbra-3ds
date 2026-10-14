#pragma once

#include <stdbool.h>

typedef struct Config {
	char username[32];
	char token[48];
	bool hardcore;
	bool unofficial;
	int scale;
} Config;

extern Config g_cfg;

void config_load(void);
void config_save(void);
