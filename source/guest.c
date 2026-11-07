#include "guest.h"
#include "app.h"
#include "audio.h"

#include <libretro.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Symbols are prefixed so FCEUmm, PokeMini, and gpSP can live in one binary. */

void nes_retro_set_environment(retro_environment_t);
void nes_retro_set_video_refresh(retro_video_refresh_t);
void nes_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void nes_retro_set_input_poll(retro_input_poll_t);
void nes_retro_set_input_state(retro_input_state_t);
void nes_retro_init(void);
void nes_retro_deinit(void);
bool nes_retro_load_game(const struct retro_game_info*);
void nes_retro_unload_game(void);
void nes_retro_run(void);
void nes_retro_reset(void);
void nes_retro_get_system_av_info(struct retro_system_av_info*);
size_t nes_retro_serialize_size(void);
bool nes_retro_serialize(void*, size_t);
bool nes_retro_unserialize(const void*, size_t);
void* nes_retro_get_memory_data(unsigned);
size_t nes_retro_get_memory_size(unsigned);

void pm_retro_set_environment(retro_environment_t);
void pm_retro_set_video_refresh(retro_video_refresh_t);
void pm_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void pm_retro_set_input_poll(retro_input_poll_t);
void pm_retro_set_input_state(retro_input_state_t);
void pm_retro_init(void);
void pm_retro_deinit(void);
bool pm_retro_load_game(const struct retro_game_info*);
void pm_retro_unload_game(void);
void pm_retro_run(void);
void pm_retro_reset(void);
void pm_retro_get_system_av_info(struct retro_system_av_info*);
size_t pm_retro_serialize_size(void);
bool pm_retro_serialize(void*, size_t);
bool pm_retro_unserialize(const void*, size_t);
void* pm_retro_get_memory_data(unsigned);
size_t pm_retro_get_memory_size(unsigned);

typedef uint8_t (*nes_read_fn)(uint32_t);
extern nes_read_fn nes_ARead[0x10000];
extern uint8_t nes_RAM[0x800];

extern uint8_t pm_PM_BIOS[4096];
extern uint8_t pm_PM_RAM[8192];
extern int pm_PokeMini_EEPROMWritten;
int pm_PokeMini_SaveEEPROMFile(const char* filename);

typedef struct GuestApi {
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	bool (*load_game)(const struct retro_game_info*);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	void (*get_system_av_info)(struct retro_system_av_info*);
	size_t (*serialize_size)(void);
	bool (*serialize)(void*, size_t);
	bool (*unserialize)(const void*, size_t);
	void* (*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} GuestApi;

static GuestApi s_api;
static int s_kind = GUEST_NONE;
static int s_inited = 0;
static uint32_t s_keys = 0;
static const uint16_t* s_frame = NULL;
static int s_stride = 256;
static int s_width = 256;
static int s_height = 240;
static char s_savePath[512];
static uint8_t* s_rom = NULL;

static void quiet_log(enum retro_log_level level, const char* fmt, ...) {
	(void)level;
	(void)fmt;
}

static void poll_cb(void) {}

static int16_t state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void)port;
	(void)index;
	if (device != RETRO_DEVICE_JOYPAD) return 0;
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_A: return (s_keys & 0x01) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_B: return (s_keys & 0x02) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_SELECT: return (s_keys & 0x04) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_START: return (s_keys & 0x08) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (s_keys & 0x10) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_LEFT: return (s_keys & 0x20) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_UP: return (s_keys & 0x40) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_DOWN: return (s_keys & 0x80) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_R: return (s_keys & 0x100) ? 1 : 0;
	case RETRO_DEVICE_ID_JOYPAD_L: return (s_keys & 0x200) ? 1 : 0;
	default: return 0;
	}
}

static void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data || !width || !height || pitch < 2) return;
	s_frame = data;
	s_width = (int)width;
	s_height = (int)height;
	s_stride = (int)(pitch / sizeof(uint16_t));
	if (s_stride < (int)width) s_stride = (int)width;
}

static size_t audio_cb(const int16_t* data, size_t frames) {
	if (data && frames) audio_push(data, frames);
	return frames;
}

static bool env_cb(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (data) ((struct retro_log_callback*)data)->log = quiet_log;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return data && *(enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (data) *(const char**)data = AGBRA_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data) *(const char**)data = AGBRA_SAVES_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* var = data;
		if (!var || !var->key) return false;
		if (strcmp(var->key, "pokemini_video_scale") == 0) {
			var->value = "3x";
			return true;
		}
		if (strcmp(var->key, "pokemini_60hz_mode") == 0) {
			var->value = "enabled";
			return true;
		}
		if (strcmp(var->key, "pokemini_lcdfilter") == 0) {
			var->value = "none";
			return true;
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool*)data = false;
		return true;
	default:
		return false;
	}
}

static void bind_api(int kind) {
	memset(&s_api, 0, sizeof(s_api));
	if (kind == GUEST_NES) {
		s_api.set_environment = nes_retro_set_environment;
		s_api.set_video_refresh = nes_retro_set_video_refresh;
		s_api.set_audio_sample_batch = nes_retro_set_audio_sample_batch;
		s_api.set_input_poll = nes_retro_set_input_poll;
		s_api.set_input_state = nes_retro_set_input_state;
		s_api.init = nes_retro_init;
		s_api.deinit = nes_retro_deinit;
		s_api.load_game = nes_retro_load_game;
		s_api.unload_game = nes_retro_unload_game;
		s_api.run = nes_retro_run;
		s_api.reset = nes_retro_reset;
		s_api.get_system_av_info = nes_retro_get_system_av_info;
		s_api.serialize_size = nes_retro_serialize_size;
		s_api.serialize = nes_retro_serialize;
		s_api.unserialize = nes_retro_unserialize;
		s_api.get_memory_data = nes_retro_get_memory_data;
		s_api.get_memory_size = nes_retro_get_memory_size;
	} else {
		s_api.set_environment = pm_retro_set_environment;
		s_api.set_video_refresh = pm_retro_set_video_refresh;
		s_api.set_audio_sample_batch = pm_retro_set_audio_sample_batch;
		s_api.set_input_poll = pm_retro_set_input_poll;
		s_api.set_input_state = pm_retro_set_input_state;
		s_api.init = pm_retro_init;
		s_api.deinit = pm_retro_deinit;
		s_api.load_game = pm_retro_load_game;
		s_api.unload_game = pm_retro_unload_game;
		s_api.run = pm_retro_run;
		s_api.reset = pm_retro_reset;
		s_api.get_system_av_info = pm_retro_get_system_av_info;
		s_api.serialize_size = pm_retro_serialize_size;
		s_api.serialize = pm_retro_serialize;
		s_api.unserialize = pm_retro_unserialize;
		s_api.get_memory_data = pm_retro_get_memory_data;
		s_api.get_memory_size = pm_retro_get_memory_size;
	}
}

static void load_nes_save(void) {
	if (!s_api.get_memory_data || !s_savePath[0]) return;
	uint8_t* ram = s_api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = s_api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!ram || !n || n > 128 * 1024) return;
	int fd = open(s_savePath, O_RDONLY);
	if (fd < 0) return;
	off_t sz = lseek(fd, 0, SEEK_END);
	if (sz > 0 && (size_t)sz <= n) {
		lseek(fd, 0, SEEK_SET);
		if (read(fd, ram, (size_t)sz) == sz && (size_t)sz < n)
			memset(ram + sz, 0, n - (size_t)sz);
	}
	close(fd);
}

void guest_unload(void) {
	if (s_inited && s_api.unload_game) s_api.unload_game();
	if (s_inited && s_api.deinit) s_api.deinit();
	free(s_rom);
	s_rom = NULL;
	s_inited = 0;
	s_kind = GUEST_NONE;
	s_frame = NULL;
	s_keys = 0;
	memset(&s_api, 0, sizeof(s_api));
}

bool guest_load(int kind, const char* romPath, const char* savePath) {
	guest_unload();
	if (!romPath || (kind != GUEST_NES && kind != GUEST_MINI)) return false;
	bind_api(kind);
	if (savePath) {
		strncpy(s_savePath, savePath, sizeof(s_savePath) - 1);
		s_savePath[sizeof(s_savePath) - 1] = 0;
	} else {
		s_savePath[0] = 0;
	}

	struct retro_game_info info;
	memset(&info, 0, sizeof(info));
	info.path = romPath;
	if (kind == GUEST_MINI) {
		int fd = open(romPath, O_RDONLY);
		if (fd < 0) return false;
		off_t sz = lseek(fd, 0, SEEK_END);
		if (sz <= 0x2100 || sz > 0x200000) {
			close(fd);
			return false;
		}
		s_rom = malloc((size_t)sz);
		if (!s_rom) {
			close(fd);
			return false;
		}
		lseek(fd, 0, SEEK_SET);
		if (read(fd, s_rom, (size_t)sz) != sz) {
			close(fd);
			free(s_rom);
			s_rom = NULL;
			return false;
		}
		close(fd);
		info.data = s_rom;
		info.size = (size_t)sz;
	}

	s_api.set_environment(env_cb);
	s_api.set_video_refresh(video_cb);
	s_api.set_audio_sample_batch(audio_cb);
	s_api.set_input_poll(poll_cb);
	s_api.set_input_state(state_cb);
	s_api.init();
	s_inited = 1;
	if (!s_api.load_game(&info)) {
		guest_unload();
		return false;
	}
	s_kind = kind;
	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	s_api.get_system_av_info(&av);
	if (av.geometry.base_width) s_width = (int)av.geometry.base_width;
	if (av.geometry.base_height) s_height = (int)av.geometry.base_height;
	if (av.timing.sample_rate > 1000.0)
		audio_set_rate((float)av.timing.sample_rate);
	if (kind == GUEST_NES) load_nes_save();
	free(s_rom);
	s_rom = NULL;
	return true;
}

int guest_kind(void) { return s_kind; }

void guest_run(void) {
	if (s_kind && s_api.run) s_api.run();
}

void guest_reset(void) {
	if (s_kind && s_api.reset) s_api.reset();
}

void guest_set_keys(uint32_t keys) { s_keys = keys; }

uint8_t guest_read8(uint32_t address) {
	if (s_kind == GUEST_NES) {
		if (address < 0x2000) return nes_RAM[address & 0x7FF];
		if (address >= 0x6000 && address <= 0xFFFF && nes_ARead[address])
			return nes_ARead[address](address);
		return 0;
	}
	if (s_kind == GUEST_MINI) {
		if (address < 0x1000) return pm_PM_BIOS[address];
		if (address < 0x2000) return pm_PM_RAM[address - 0x1000];
	}
	return 0;
}

const uint16_t* guest_pixels(void) { return s_frame; }
int guest_stride(void) { return s_stride; }
int guest_width(void) { return s_width; }
int guest_height(void) { return s_height; }

bool guest_save_state(const char* path) {
	if (!s_kind || !path || !s_api.serialize_size || !s_api.serialize) return false;
	size_t n = s_api.serialize_size();
	if (!n || n > 2 * 1024 * 1024) return false;
	void* buf = malloc(n);
	if (!buf) return false;
	bool ok = s_api.serialize(buf, n);
	if (ok) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		ok = fd >= 0 && write(fd, buf, n) == (ssize_t)n;
		if (fd >= 0) close(fd);
	}
	free(buf);
	return ok;
}

bool guest_load_state(const char* path) {
	if (!s_kind || !path || !s_api.serialize_size || !s_api.unserialize) return false;
	size_t expect = s_api.serialize_size();
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	off_t n = lseek(fd, 0, SEEK_END);
	if (n <= 0 || (size_t)n != expect || n > 2 * 1024 * 1024) {
		close(fd);
		return false;
	}
	void* buf = malloc((size_t)n);
	if (!buf) {
		close(fd);
		return false;
	}
	lseek(fd, 0, SEEK_SET);
	bool readOk = read(fd, buf, (size_t)n) == n;
	close(fd);
	bool ok = readOk && s_api.unserialize(buf, (size_t)n);
	free(buf);
	return ok;
}

void guest_flush_save(const char* path, int sync) {
	(void)sync;
	if (!s_kind) return;
	if (s_kind == GUEST_MINI) {
		const char* file = path && path[0] ? path : s_savePath;
		if (file && file[0] && pm_PokeMini_EEPROMWritten) {
			pm_PokeMini_SaveEEPROMFile(file);
			pm_PokeMini_EEPROMWritten = 0;
		}
		return;
	}
	if (!s_api.get_memory_data || !s_savePath[0]) return;
	uint8_t* ram = s_api.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = s_api.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!ram || !n || n > 128 * 1024) return;
	int fd = open(s_savePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return;
	size_t off = 0;
	while (off < n) {
		ssize_t wr = write(fd, ram + off, n - off);
		if (wr <= 0) break;
		off += (size_t)wr;
	}
	if (sync) fsync(fd);
	close(fd);
}
