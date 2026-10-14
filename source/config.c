#include "config.h"
#include "app.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>

Config g_cfg = {
	.username = "",
	.token = "",
	.hardcore = true,
	.unofficial = false,
	.scale = 0,
};

static void trim(char* s) {
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
		s[--n] = 0;
	}
}

void config_load(void) {
	FILE* f = fopen(AGBRA_CFG_PATH, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (line[0] == '#' || line[0] == 0) continue;
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		const char* key = line;
		const char* val = eq + 1;
		if (strcmp(key, "username") == 0) {
			strncpy(g_cfg.username, val, sizeof(g_cfg.username) - 1);
		} else if (strcmp(key, "token") == 0) {
			strncpy(g_cfg.token, val, sizeof(g_cfg.token) - 1);
		} else if (strcmp(key, "hardcore") == 0) {
			g_cfg.hardcore = val[0] != '0';
		} else if (strcmp(key, "unofficial") == 0) {
			g_cfg.unofficial = val[0] == '1';
		} else if (strcmp(key, "scale") == 0) {
			g_cfg.scale = val[0] - '0';
			if (g_cfg.scale < 0 || g_cfg.scale > 2) g_cfg.scale = 1;
		}
	}
	fclose(f);
}

void config_save(void) {
	fs_mkdir_p(AGBRA_DIR);
	FILE* f = fopen(AGBRA_CFG_PATH, "w");
	if (!f) return;
	fprintf(f, "username=%s\n", g_cfg.username);
	fprintf(f, "token=%s\n", g_cfg.token);
	fprintf(f, "hardcore=%d\n", g_cfg.hardcore ? 1 : 0);
	fprintf(f, "unofficial=%d\n", g_cfg.unofficial ? 1 : 0);
	fprintf(f, "scale=%d\n", g_cfg.scale);
	fclose(f);
}
