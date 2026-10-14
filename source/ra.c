#include "ra.h"
#include "app.h"
#include "config.h"
#include "emu.h"
#include "http.h"
#include "overlay.h"

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RA_MAX_LIST 256

static rc_client_t* s_client = NULL;
static rc_client_achievement_list_t* s_list = NULL;
static const rc_client_achievement_t* s_flat[RA_MAX_LIST];
static int s_flatCount = 0;

static uint32_t RC_CCONV ra_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
	(void)client;
	if (!emu_loaded() || !buffer || !num_bytes) return 0;
	const rc_memory_regions_t* regions = rc_console_memory_regions(RC_CONSOLE_GAMEBOY_ADVANCE);
	for (uint32_t i = 0; i < num_bytes; i++) {
		uint32_t addr = address + i;
		uint32_t real = addr;
		if (regions) {
			for (uint32_t r = 0; r < regions->num_regions; r++) {
				const rc_memory_region_t* reg = &regions->region[r];
				if (addr >= reg->start_address && addr <= reg->end_address) {
					real = reg->real_address + (addr - reg->start_address);
					break;
				}
			}
		}
		buffer[i] = emu_read8(real);
	}
	return num_bytes;
}

static void RC_CCONV ra_log(const char* message, const rc_client_t* client) {
	(void)client;
	(void)message;
}

static void RC_CCONV ra_event(const rc_client_event_t* event, rc_client_t* client) {
	(void)client;
	if (!event) return;
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		if (event->achievement) {
			char line[160];
			snprintf(line, sizeof(line), "+%" PRIu32 "  %s", event->achievement->points, event->achievement->title);
			overlay_push("Achievement", line);
			app_toast("%s", line);
			ra_refresh_list();
		}
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		overlay_push("Mastery", "All achievements earned");
		app_toast("Game mastered");
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		if (event->leaderboard) overlay_push("Leaderboard", event->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		if (event->leaderboard) overlay_push("Submitted", event->leaderboard->title);
		break;
	case RC_CLIENT_EVENT_RESET:
		emu_reset();
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		app_set_status("RA server error");
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		app_set_status("RA disconnected — will retry");
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		app_set_status("RA reconnected");
		break;
	default:
		break;
	}
}

static void RC_CCONV login_cb(int result, const char* error_message, rc_client_t* client, void* userdata) {
	(void)userdata;
	if (result != RC_OK) {
		app_set_status("Login failed: %s", error_message ? error_message : rc_error_str(result));
		return;
	}
	const rc_client_user_t* user = rc_client_get_user_info(client);
	if (user) {
		strncpy(g_cfg.username, user->username, sizeof(g_cfg.username) - 1);
		if (user->token) strncpy(g_cfg.token, user->token, sizeof(g_cfg.token) - 1);
		config_save();
		app_set_status("Signed in as %s  (%u pts)", user->display_name ? user->display_name : user->username, user->score);
		app_toast("Welcome, %s", user->display_name ? user->display_name : user->username);
	}
}

static void RC_CCONV load_cb(int result, const char* error_message, rc_client_t* client, void* userdata) {
	(void)userdata;
	(void)client;
	if (result != RC_OK) {
		app_set_status("RA: %s", error_message ? error_message : rc_error_str(result));
		return;
	}
	const rc_client_game_t* game = rc_client_get_game_info(s_client);
	rc_client_user_game_summary_t sum;
	memset(&sum, 0, sizeof(sum));
	rc_client_get_user_game_summary(s_client, &sum);
	if (game) {
		app_set_status("%s  %u/%u", game->title, sum.num_unlocked_achievements, sum.num_core_achievements);
		overlay_push(game->title, "RetroAchievements loaded");
	}
	ra_refresh_list();
}

bool ra_init(void) {
	s_client = rc_client_create(ra_read_memory, http_server_call);
	if (!s_client) return false;
	rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_INFO, ra_log);
	rc_client_set_event_handler(s_client, ra_event);
	rc_client_set_hardcore_enabled(s_client, g_cfg.hardcore ? 1 : 0);
	rc_client_set_unofficial_enabled(s_client, g_cfg.unofficial ? 1 : 0);
	return true;
}

void ra_exit(void) {
	if (s_list) {
		rc_client_destroy_achievement_list(s_list);
		s_list = NULL;
	}
	if (s_client) {
		rc_client_unload_game(s_client);
		rc_client_destroy(s_client);
		s_client = NULL;
	}
}

void ra_idle(void) {
	if (s_client) rc_client_idle(s_client);
}

void ra_frame(void) {
	if (s_client) rc_client_do_frame(s_client);
}

void ra_poll_http(void) {
	http_poll();
}

void ra_login(const char* user, const char* pass) {
	if (!s_client || !user || !pass) return;
	app_set_status("Signing in…");
	rc_client_begin_login_with_password(s_client, user, pass, login_cb, NULL);
}

void ra_login_token(const char* user, const char* token) {
	if (!s_client || !user || !token) return;
	rc_client_begin_login_with_token(s_client, user, token, login_cb, NULL);
}

void ra_logout(void) {
	if (!s_client) return;
	rc_client_logout(s_client);
	g_cfg.token[0] = 0;
	config_save();
	app_set_status("Signed out");
}

void ra_load_game(const char* path, const uint8_t* rom, size_t romSize) {
	if (!s_client) return;
	rc_client_set_hardcore_enabled(s_client, g_app.hardcore ? 1 : 0);
	rc_client_set_unofficial_enabled(s_client, g_app.unofficial ? 1 : 0);
	rc_client_begin_identify_and_load_game(s_client, RC_CONSOLE_GAMEBOY_ADVANCE,
		path, rom, romSize, load_cb, NULL);
}

void ra_unload_game(void) {
	if (s_list) {
		rc_client_destroy_achievement_list(s_list);
		s_list = NULL;
	}
	s_flatCount = 0;
	if (s_client) rc_client_unload_game(s_client);
}

void ra_reset(void) {
	if (s_client) rc_client_reset(s_client);
}

bool ra_logged_in(void) {
	return s_client && rc_client_get_user_info(s_client) != NULL;
}

bool ra_game_loaded(void) {
	return s_client && rc_client_is_game_loaded(s_client);
}

bool ra_hardcore(void) {
	return s_client && rc_client_get_hardcore_enabled(s_client);
}

void ra_set_hardcore(bool on) {
	g_app.hardcore = on;
	g_cfg.hardcore = on;
	if (s_client) rc_client_set_hardcore_enabled(s_client, on ? 1 : 0);
	config_save();
}

const char* ra_user_name(void) {
	if (!s_client) return NULL;
	const rc_client_user_t* u = rc_client_get_user_info(s_client);
	return u ? (u->display_name ? u->display_name : u->username) : NULL;
}

uint32_t ra_user_score(void) {
	if (!s_client) return 0;
	const rc_client_user_t* u = rc_client_get_user_info(s_client);
	return u ? u->score : 0;
}

const char* ra_game_title(void) {
	if (!s_client) return NULL;
	const rc_client_game_t* g = rc_client_get_game_info(s_client);
	return g ? g->title : NULL;
}

void ra_rich_presence(char* buf, size_t bufLen) {
	if (!buf || !bufLen) return;
	buf[0] = 0;
	if (s_client) rc_client_get_rich_presence_message(s_client, buf, bufLen);
}

int ra_summary(uint32_t* unlocked, uint32_t* total, uint32_t* points) {
	if (!s_client) return 0;
	rc_client_user_game_summary_t sum;
	memset(&sum, 0, sizeof(sum));
	rc_client_get_user_game_summary(s_client, &sum);
	if (unlocked) *unlocked = sum.num_unlocked_achievements;
	if (total) *total = sum.num_core_achievements;
	if (points) *points = sum.points_unlocked;
	return 1;
}

void ra_refresh_list(void) {
	if (s_list) {
		rc_client_destroy_achievement_list(s_list);
		s_list = NULL;
	}
	s_flatCount = 0;
	if (!s_client) return;
	s_list = rc_client_create_achievement_list(s_client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
		RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!s_list) return;
	for (uint32_t b = 0; b < s_list->num_buckets && s_flatCount < RA_MAX_LIST; b++) {
		const rc_client_achievement_bucket_t* bucket = &s_list->buckets[b];
		for (uint32_t i = 0; i < bucket->num_achievements && s_flatCount < RA_MAX_LIST; i++) {
			s_flat[s_flatCount++] = bucket->achievements[i];
		}
	}
}

int ra_list_count(void) {
	return s_flatCount;
}

const char* ra_list_title(int index) {
	if (index < 0 || index >= s_flatCount || !s_flat[index]) return "";
	return s_flat[index]->title;
}

bool ra_list_unlocked(int index) {
	if (index < 0 || index >= s_flatCount || !s_flat[index]) return false;
	return s_flat[index]->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
}

int ra_list_points(int index) {
	if (index < 0 || index >= s_flatCount || !s_flat[index]) return 0;
	return (int)s_flat[index]->points;
}
