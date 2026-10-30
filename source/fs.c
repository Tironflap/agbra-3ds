#include "fs.h"
#include "app.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool fs_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

void fs_mkdir_p(const char* path) {
	char tmp[512];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	size_t n = strlen(tmp);
	if (n && tmp[n - 1] == '/') tmp[n - 1] = 0;
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

static int ends_with_ci(const char* name, const char* ext) {
	size_t n = strlen(name), e = strlen(ext);
	if (n < e) return 0;
	for (size_t i = 0; i < e; i++) {
		if (tolower((unsigned char)name[n - e + i]) != ext[i]) return 0;
	}
	return 1;
}

static int is_rom(const char* name) {
	return ends_with_ci(name, ".gba") || ends_with_ci(name, ".agb") ||
		ends_with_ci(name, ".gb") || ends_with_ci(name, ".gbc") ||
		ends_with_ci(name, ".mb") || ends_with_ci(name, ".bin") ||
		ends_with_ci(name, ".zip");
}

static int cmp_rom(const void* a, const void* b) {
	const RomEntry* ra = a;
	const RomEntry* rb = b;
	return strcasecmp(ra->name, rb->name);
}

static int scan_dir(const char* dir, RomEntry* list, int max, int count) {
	DIR* d = opendir(dir);
	if (!d) return count;
	struct dirent* ent;
	while ((ent = readdir(d)) && count < max) {
		if (ent->d_name[0] == '.') continue;
		if (!is_rom(ent->d_name)) continue;
		strncpy(list[count].name, ent->d_name, sizeof(list[count].name) - 1);
		list[count].name[sizeof(list[count].name) - 1] = 0;
		snprintf(list[count].path, sizeof(list[count].path), "%s/%s", dir, ent->d_name);
		count++;
	}
	closedir(d);
	return count;
}

int fs_scan_roms(RomEntry* list, int max) {
	int count = 0;
	count = scan_dir(AGBRA_ROMS_DIR, list, max, count);
	count = scan_dir(AGBRA_GB_ROMS_DIR, list, max, count);
	count = scan_dir("sdmc:/roms/gbc", list, max, count);
	count = scan_dir("sdmc:/gba", list, max, count);
	count = scan_dir("sdmc:/gb", list, max, count);
	count = scan_dir("sdmc:/roms", list, max, count);
	count = scan_dir("sdmc:/3ds/agbra/roms", list, max, count);
	if (count > 1) qsort(list, (size_t)count, sizeof(RomEntry), cmp_rom);
	return count;
}
