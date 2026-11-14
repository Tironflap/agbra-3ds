#pragma once

void overlay_draw(void);
void overlay_push(const char* title, const char* detail);
/* Persistent lines while a measured goal, primed challenge, or
 * leaderboard tracker is active. NULL clears that line. */
void overlay_set_progress(const char* title, const char* detail);
void overlay_set_challenge(const char* title);
void overlay_set_leaderboard(const char* text);
void overlay_clear_trackers(void);
