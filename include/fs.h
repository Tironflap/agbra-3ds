#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FS_MAX_ROMS 512

typedef struct {
	char name[256];
	char path[512];
} RomEntry;

int fs_scan_roms(RomEntry* list, int max);
bool fs_exists(const char* path);
void fs_mkdir_p(const char* path);
