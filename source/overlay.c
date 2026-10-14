#include "overlay.h"
#include "app.h"
#include "video.h"

#include <string.h>

#define RGB(r, g, b) ((uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3)))

static char s_title[64];
static char s_detail[160];
static int s_frames = 0;

void overlay_push(const char* title, const char* detail) {
	strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
	s_title[sizeof(s_title) - 1] = 0;
	strncpy(s_detail, detail ? detail : "", sizeof(s_detail) - 1);
	s_detail[sizeof(s_detail) - 1] = 0;
	s_frames = 200;
}

void overlay_draw(void) {
	if (s_frames <= 0) return;
	s_frames--;
	video_overlay_bar(8, 8, 384, 28, RGB(12, 14, 18));
	video_overlay_bar(8, 8, 4, 28, RGB(201, 162, 74));
	video_overlay_text(16, 10, s_title, RGB(201, 162, 74));
	video_overlay_text(16, 20, s_detail, RGB(232, 228, 214));
}
