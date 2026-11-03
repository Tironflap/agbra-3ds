#include "fastgba.h"
#include "audio.h"
#include "fs.h"

#include <libretro.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_init(void);
void retro_deinit(void);
bool retro_load_game(const struct retro_game_info* game);
void retro_unload_game(void);
void retro_run(void);
void retro_reset(void);
void retro_get_system_av_info(struct retro_system_av_info* info);
size_t retro_serialize_size(void);
bool retro_serialize(void* data, size_t size);
bool retro_unserialize(const void* data, size_t size);

extern int dynarec_enable;
extern uint8_t gamepak_backup[];
extern uint32_t backup_type;
extern uint32_t backup_type_reset;
extern uint32_t flash_bank_cnt;
extern uint32_t eeprom_size;
extern uint8_t* memory_map_read[];

#define BACKUP_SRAM 0u
#define BACKUP_FLASH 1u
#define BACKUP_EEPROM 2u
#define BACKUP_UNKN 3u
#define FLASH_SIZE_128KB 2u
#define EEPROM_8_KBYTE 16u

static int s_inited = 0;
static int s_game = 0;
static uint32_t s_keys = 0;
static const uint16_t* s_frame = NULL;
static int s_stride = 240;
static size_t s_loadedBytes = 0;
static char s_savePath[512];

static size_t save_bytes(void) {
	uint32_t kind = backup_type_reset;
	if (kind == BACKUP_UNKN) kind = backup_type;
	if (kind == BACKUP_FLASH) return flash_bank_cnt == FLASH_SIZE_128KB ? 128u * 1024u : 64u * 1024u;
	if (kind == BACKUP_EEPROM) return eeprom_size == EEPROM_8_KBYTE ? 8u * 1024u : 512u;
	if (kind == BACKUP_SRAM) return 32u * 1024u;
	if (s_loadedBytes) return s_loadedBytes;
	return 32u * 1024u;
}

static void load_save_file(const char* path) {
	s_loadedBytes = 0;
	if (!path || !path[0]) return;
	int fd = open(path, O_RDONLY);
	if (fd < 0) return;
	off_t n = lseek(fd, 0, SEEK_END);
	if (n <= 0 || n > 128 * 1024) {
		close(fd);
		return;
	}
	lseek(fd, 0, SEEK_SET);
	ssize_t got = read(fd, gamepak_backup, (size_t)n);
	close(fd);
	if (got > 0) s_loadedBytes = (size_t)got;
}

static bool env_cb(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return data && *(enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (!data) return false;
		*(const char**)data = "sdmc:/3ds/agbra";
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (!data) return false;
		*(const char**)data = "sdmc:/3ds/agbra/saves";
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool*)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* var = data;
		if (!var || !var->key) return false;
		if (!strcmp(var->key, "gpsp_drc")) { var->value = "enabled"; return true; }
		if (!strcmp(var->key, "gpsp_sound_rate")) { var->value = "32768"; return true; }
		if (!strcmp(var->key, "gpsp_frameskip")) { var->value = "disabled"; return true; }
		if (!strcmp(var->key, "gpsp_color_correction")) { var->value = "disabled"; return true; }
		if (!strcmp(var->key, "gpsp_frame_mixing")) { var->value = "disabled"; return true; }
		if (!strcmp(var->key, "gpsp_bios")) { var->value = "auto"; return true; }
		if (!strcmp(var->key, "gpsp_boot_mode")) { var->value = "game"; return true; }
		if (!strcmp(var->key, "gpsp_serial")) { var->value = "disabled"; return true; }
		if (!strcmp(var->key, "gpsp_rtc")) { var->value = "auto"; return true; }
		if (!strcmp(var->key, "gpsp_sprlim")) { var->value = "disabled"; return true; }
		return false;
	}
	default:
		return false;
	}
}

static void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
	(void)width;
	(void)height;
	if (!data || pitch < 2) return;
	s_frame = data;
	s_stride = (int)(pitch / 2);
}

static size_t audio_cb(const int16_t* data, size_t frames) {
	if (data && frames) audio_push(data, frames);
	return frames;
}

static void poll_cb(void) {}

static int16_t state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void)port;
	(void)device;
	(void)index;
	uint32_t bit = 0;
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_A: bit = 0x001; break;
	case RETRO_DEVICE_ID_JOYPAD_B: bit = 0x002; break;
	case RETRO_DEVICE_ID_JOYPAD_SELECT: bit = 0x004; break;
	case RETRO_DEVICE_ID_JOYPAD_START: bit = 0x008; break;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: bit = 0x010; break;
	case RETRO_DEVICE_ID_JOYPAD_LEFT: bit = 0x020; break;
	case RETRO_DEVICE_ID_JOYPAD_UP: bit = 0x040; break;
	case RETRO_DEVICE_ID_JOYPAD_DOWN: bit = 0x080; break;
	case RETRO_DEVICE_ID_JOYPAD_R: bit = 0x100; break;
	case RETRO_DEVICE_ID_JOYPAD_L: bit = 0x200; break;
	default: return 0;
	}
	return (s_keys & bit) ? 1 : 0;
}

static void ensure_init(void) {
	if (s_inited) return;
	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample_batch(audio_cb);
	retro_set_input_poll(poll_cb);
	retro_set_input_state(state_cb);
	retro_init();
	s_inited = 1;
}

bool fastgba_ready(void) {
	return s_game && dynarec_enable;
}

bool fastgba_load(const char* romPath, const char* savePath) {
	struct retro_game_info info;
	struct retro_system_av_info av;
	if (!romPath || !romPath[0]) return false;
	fastgba_unload();
	s_savePath[0] = 0;
	if (savePath) {
		strncpy(s_savePath, savePath, sizeof(s_savePath) - 1);
		s_savePath[sizeof(s_savePath) - 1] = 0;
	}
	s_frame = NULL;
	s_stride = 240;
	s_keys = 0;
	ensure_init();
	memset(&info, 0, sizeof(info));
	info.path = romPath;
	if (!retro_load_game(&info) || !dynarec_enable) {
		fastgba_unload();
		return false;
	}
	/* reset_gba does not wipe the backup, and the first instruction is
	   still ahead of us, so the cart sees this image at boot. */
	load_save_file(s_savePath);
	s_game = 1;
	memset(&av, 0, sizeof(av));
	retro_get_system_av_info(&av);
	if (av.timing.sample_rate > 1.0) audio_set_rate((float)av.timing.sample_rate);
	return true;
}

void fastgba_unload(void) {
	s_frame = NULL;
	s_game = 0;
	s_keys = 0;
	if (!s_inited) return;
	retro_unload_game();
	retro_deinit();
	s_inited = 0;
}

void fastgba_run(void) {
	if (s_game) retro_run();
}

void fastgba_reset(void) {
	if (s_game) retro_reset();
}

void fastgba_set_keys(uint32_t keys) {
	s_keys = keys;
}

uint8_t fastgba_read8(uint32_t address) {
	uint8_t* page;
	if ((address & 0x0F000000u) == 0x0E000000u)
		return gamepak_backup[address & 0xFFFFu];
	page = memory_map_read[(address >> 15) & 8191u];
	if (!page) return 0;
	return page[address & 0x7FFFu];
}

const uint16_t* fastgba_pixels(void) {
	return s_frame;
}

int fastgba_stride(void) {
	return s_stride > 0 ? s_stride : 240;
}

void fastgba_flush_save(const char* path, int sync) {
	size_t n;
	int fd;
	size_t off;
	if (!s_game || !path || !path[0]) return;
	n = save_bytes();
	if (!n || n > 128u * 1024u) return;
	fs_mkdir_p("sdmc:/3ds/agbra/saves");
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return;
	off = 0;
	while (off < n) {
		ssize_t wr = write(fd, gamepak_backup + off, n - off);
		if (wr <= 0) break;
		off += (size_t)wr;
	}
	if (sync) fsync(fd);
	close(fd);
}

bool fastgba_save_state(const char* path) {
	size_t n;
	void* data;
	int fd;
	bool ok;
	if (!s_game || !path) return false;
	n = retro_serialize_size();
	if (!n || n > 1024u * 1024u) return false;
	data = malloc(n);
	if (!data) return false;
	ok = retro_serialize(data, n);
	if (ok) {
		fs_mkdir_p("sdmc:/3ds/agbra/states");
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) ok = false;
		else {
			ok = write(fd, data, n) == (ssize_t)n;
			close(fd);
		}
	}
	free(data);
	return ok;
}

bool fastgba_load_state(const char* path) {
	int fd;
	off_t n;
	void* data;
	bool ok;
	size_t expect;
	if (!s_game || !path) return false;
	fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	n = lseek(fd, 0, SEEK_END);
	expect = retro_serialize_size();
	if (n <= 0 || (size_t)n != expect) {
		close(fd);
		return false;
	}
	data = malloc((size_t)n);
	if (!data) {
		close(fd);
		return false;
	}
	lseek(fd, 0, SEEK_SET);
	ok = read(fd, data, (size_t)n) == n;
	close(fd);
	if (ok) ok = retro_unserialize(data, (size_t)n);
	free(data);
	return ok;
}
