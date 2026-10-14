#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ra_init(void);
void ra_exit(void);
void ra_idle(void);
void ra_frame(void);
void ra_login(const char* user, const char* pass);
void ra_login_token(const char* user, const char* token);
void ra_logout(void);
void ra_load_game(const char* path, const uint8_t* rom, size_t romSize);
void ra_unload_game(void);
void ra_reset(void);
bool ra_logged_in(void);
bool ra_game_loaded(void);
bool ra_hardcore(void);
void ra_set_hardcore(bool on);
const char* ra_user_name(void);
uint32_t ra_user_score(void);
const char* ra_game_title(void);
void ra_rich_presence(char* buf, size_t bufLen);
int ra_summary(uint32_t* unlocked, uint32_t* total, uint32_t* points);
int ra_list_count(void);
const char* ra_list_title(int index);
bool ra_list_unlocked(int index);
int ra_list_points(int index);
void ra_refresh_list(void);
void ra_poll_http(void);
