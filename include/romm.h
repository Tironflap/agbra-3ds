#pragma once

#include <stddef.h>

typedef enum {
	ROMM_NEED_LOGIN = 0,
	ROMM_BUSY,
	ROMM_PLATFORMS,
	ROMM_ROMS,
	ROMM_ERROR
} RommPhase;

typedef struct RommRow {
	int id;
	char name[72];
	char file[120];
	char slug[12];
	unsigned bytes;
	int romCount;
} RommRow;

/* url is the server root. secret is a password, or a client token that starts with rmm_. */
void romm_login(const char* url, const char* user, const char* secret);
int romm_ready(void);
RommPhase romm_phase(void);
void romm_load_platforms(void);
void romm_open(int index);
void romm_page(int dir);
int romm_count(void);
const RommRow* romm_at(int index);
int romm_offset(void);
int romm_total(void);
const char* romm_platform_name(void);
/* overwrite replaces a file already on the card. */
void romm_download(int index, int overwrite);
int romm_busy(void);
/* Fills buf while a download is running. Returns 0 otherwise. */
int romm_progress(char* buf, size_t n);
const char* romm_message(void);
