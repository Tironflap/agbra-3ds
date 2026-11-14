#include "overlay.h"
#include "app.h"
#include "video.h"

#include <stdio.h>
#include <string.h>

#define RGB(r, g, b) ((uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3)))

static char s_title[64];
static char s_detail[160];
static int s_frames = 0;
static int s_erase = 0;
static char s_progress[48];
static char s_progressDetail[32];
static char s_challenge[48];
static char s_leaderboard[48];
static int s_progressOn = 0;
static int s_challengeOn = 0;
static int s_leaderboardOn = 0;

static void copy_clip(char* dst, size_t n, const char* src) {
	size_t i = 0;
	if (!src) src = "";
	if (n == 0) return;
	while (src[i] && i + 1 < n) {
		dst[i] = (src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
		i++;
	}
	dst[i] = 0;
}

void overlay_push(const char* title, const char* detail) {
	strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
	s_title[sizeof(s_title) - 1] = 0;
	strncpy(s_detail, detail ? detail : "", sizeof(s_detail) - 1);
	s_detail[sizeof(s_detail) - 1] = 0;
	s_frames = 120;
	s_erase = 0;
}

void overlay_set_progress(const char* title, const char* detail) {
	if (!title || !title[0]) {
		s_progressOn = 0;
		return;
	}
	copy_clip(s_progress, sizeof(s_progress), title);
	copy_clip(s_progressDetail, sizeof(s_progressDetail), detail);
	s_progressOn = 1;
}

void overlay_set_challenge(const char* title) {
	if (!title || !title[0]) {
		s_challengeOn = 0;
		return;
	}
	copy_clip(s_challenge, sizeof(s_challenge), title);
	s_challengeOn = 1;
}

void overlay_set_leaderboard(const char* text) {
	if (!text || !text[0]) {
		s_leaderboardOn = 0;
		return;
	}
	copy_clip(s_leaderboard, sizeof(s_leaderboard), text);
	s_leaderboardOn = 1;
}

void overlay_clear_trackers(void) {
	s_progressOn = 0;
	s_challengeOn = 0;
	s_leaderboardOn = 0;
}

static void draw_line(int y, const char* text, uint16_t color) {
	video_overlay_bar(8, y, 384, 12, RGB(12, 14, 18));
	video_overlay_bar(8, y, 4, 12, color);
	video_overlay_text(16, y + 2, text, color);
}

void overlay_draw(void) {
	int y = 8;
	if (s_frames > 0) {
		s_frames--;
		video_overlay_bar(8, 8, 384, 28, RGB(12, 14, 18));
		video_overlay_bar(8, 8, 4, 28, RGB(201, 162, 74));
		video_overlay_text(16, 10, s_title, RGB(201, 162, 74));
		video_overlay_text(16, 20, s_detail, RGB(232, 228, 214));
		if (s_frames == 0) s_erase = 2;
		y = 40;
	} else if (s_erase > 0) {
		/* The bar sits above the GBA picture, so the blit never covers it. */
		s_erase--;
		video_overlay_bar(8, 8, 384, 36, 0);
	}
	if (s_challengeOn) {
		draw_line(y, s_challenge, RGB(220, 96, 72));
		y += 14;
	}
	if (s_progressOn) {
		char line[80];
		if (s_progressDetail[0])
			snprintf(line, sizeof(line), "%s  %s", s_progress, s_progressDetail);
		else
			snprintf(line, sizeof(line), "%s", s_progress);
		draw_line(y, line, RGB(232, 228, 214));
		y += 14;
	}
	if (s_leaderboardOn) draw_line(y, s_leaderboard, RGB(120, 196, 140));
}
