#include "video.h"
#include "app.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

static uint16_t* s_top = NULL;
static PrintConsole* s_console = NULL;
static int s_fps = 0;
static int s_fpsFrames = 0;
static int s_skip = 0;
static int s_dyn = 0;
static int s_jitShow = 0;
static unsigned s_jit = 0;
static u64 s_fpsT = 0;
static u64 s_presentMs = 0;

void video_set_skip(int skip) {
	s_skip = skip;
}

void video_set_dyn(int on) {
	s_dyn = on ? 1 : 0;
}

void video_set_jit(unsigned code, int show) {
	s_jit = code;
	s_jitShow = show ? 1 : 0;
}

/* RGB565 */
#define RGB(r, g, b) ((uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3)))

void video_init(void) {
	gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
	gfxSetDoubleBuffering(GFX_TOP, true);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	/* consoleInit turns bottom double buffering off and remembers the
	   visible page. Drawing the menu there made the lower half blink. */
	s_console = consoleInit(GFX_BOTTOM, NULL);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	for (int i = 0; i < 2; i++) {
		uint16_t* fb = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		if (fb) {
			memset(fb, 0, TOP_WIDTH * TOP_HEIGHT * sizeof(uint16_t));
			GSPGPU_FlushDataCache(fb, TOP_WIDTH * TOP_HEIGHT * sizeof(uint16_t));
		}
		gfxScreenSwapBuffers(GFX_TOP, false);
	}
}

void video_console_target(void) {
	if (!s_console) return;
	s_console->frameBuffer = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
}

void video_present_bottom(void) {
	u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	/* 240x320 in memory, RGB565. Both screens use the same pixel size. */
	if (fb) GSPGPU_FlushDataCache(fb, 240 * 320 * sizeof(u16));
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void video_exit(void) {
	gfxExit();
}

static inline void plot(int x, int y, uint16_t c) {
	if ((unsigned)x >= TOP_WIDTH || (unsigned)y >= TOP_HEIGHT) return;
	s_top[x * TOP_HEIGHT + (TOP_HEIGHT - 1 - y)] = c;
}

/* Gather a few source rows, then write framebuffer columns.
   The 3DS top buffer is column-major, and the ARM11 data cache is 16KB
   with no L2 on an old 3DS. Walking dest x in the inner loop misses
   every pixel. A 16-row tile stays hot while the columns stream out. */
static void blit_rect(const uint16_t* src, int sw, int sh, int stride,
                       int dw, int dh, int ox, int oy, const int* sx, const int* sy) {
	static uint16_t tile[16][TOP_WIDTH];
	if (dw > TOP_WIDTH) dw = TOP_WIDTH;
	if (dh > TOP_HEIGHT) dh = TOP_HEIGHT;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;
	if (ox + dw > TOP_WIDTH) dw = TOP_WIDTH - ox;
	if (oy + dh > TOP_HEIGHT) dh = TOP_HEIGHT - oy;
	(void)sw;
	(void)sh;
	for (int y0 = 0; y0 < dh; y0 += 16) {
		int rows = dh - y0;
		if (rows > 16) rows = 16;
		for (int r = 0; r < rows; r++) {
			int srcY = sy ? sy[y0 + r] : y0 + r;
			const uint16_t* row = src + (size_t)srcY * (size_t)stride;
			if (!sx) {
				memcpy(tile[r], row, (size_t)dw * sizeof(uint16_t));
			} else {
				for (int x = 0; x < dw; x++) tile[r][x] = row[sx[x]];
			}
		}
		int base = TOP_HEIGHT - 1 - (oy + y0);
		for (int x = 0; x < dw; x++) {
			uint16_t* d = s_top + (ox + x) * TOP_HEIGHT + base;
			int r = 0;
			for (; r + 3 < rows; r += 4) {
				d[0] = tile[r][x];
				d[-1] = tile[r + 1][x];
				d[-2] = tile[r + 2][x];
				d[-3] = tile[r + 3][x];
				d -= 4;
			}
			for (; r < rows; r++) *d-- = tile[r][x];
		}
	}
}

void video_present(const uint16_t* pixels, int width, int height, int stride) {
	s_top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	if (!s_top) return;

	if (width < 1) width = GBA_WIDTH;
	if (height < 1) height = GBA_HEIGHT;
	if (stride < width) stride = width;

	static int clearedMode = -1;
	static int clearedW = -1;
	static int clearedFrames = 0;
	int mode = (int)g_app.scale;
	if (clearedMode != mode || clearedW != width) {
		clearedMode = mode;
		clearedW = width;
		clearedFrames = 0;
	}
	if (clearedFrames < 2) {
		memset(s_top, 0, TOP_WIDTH * TOP_HEIGHT * 2);
		clearedFrames++;
	}
	if (!pixels) return;

	if (mode != (int)SCALE_1X) {
		int dw = width;
		int dh = height;
		int ox = (TOP_WIDTH - dw) / 2;
		int oy = (TOP_HEIGHT - dh) / 2;
		if (mode == (int)SCALE_FILL) {
			dw = TOP_WIDTH;
			dh = TOP_HEIGHT;
			ox = 0;
			oy = 0;
		} else if (mode == (int)SCALE_FIT) {
			dw = 360;
			dh = 240;
			ox = (TOP_WIDTH - dw) / 2;
			oy = 0;
		}
		/* One divide per row and per column, not per pixel. The ARM11 has no divider. */
		static int sxTab[TOP_WIDTH];
		static int syTab[TOP_HEIGHT];
		static int tabW = -1;
		static int tabH = -1;
		static int tabSrcW = -1;
		static int tabSrcH = -1;
		if (tabW != dw || tabSrcW != width) {
			for (int x = 0; x < dw; x++) sxTab[x] = x * width / dw;
			tabW = dw;
			tabSrcW = width;
		}
		if (tabH != dh || tabSrcH != height) {
			for (int y = 0; y < dh; y++) syTab[y] = y * height / dh;
			tabH = dh;
			tabSrcH = height;
		}
		blit_rect(pixels, width, height, stride, dw, dh, ox, oy, sxTab, syTab);
	} else {
		int ox = (TOP_WIDTH - width) / 2;
		int oy = (TOP_HEIGHT - height) / 2;
		blit_rect(pixels, width, height, stride, width, height, ox, oy, NULL, NULL);
	}

	if (s_fpsT) {
		char hud[32];
		if (s_dyn)
			snprintf(hud, sizeof(hud), "%d fps  dyn", s_fps);
		else if (s_jitShow)
			snprintf(hud, sizeof(hud), "%d fps  nodyn %08lx", s_fps, (unsigned long)s_jit);
		else
			snprintf(hud, sizeof(hud), "%d fps  skip %d", s_fps, s_skip < 0 ? 0 : s_skip);
		video_overlay_text(8, 4, hud, 0xFFE0);
	}
}

void video_overlay_bar(int x, int y, int w, int h, uint16_t color) {
	if (!s_top) s_top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	if (!s_top) return;
	for (int yy = 0; yy < h; yy++) {
		for (int xx = 0; xx < w; xx++) plot(x + xx, y + yy, color);
	}
}

/* Tiny 6x8 font, ASCII 32..127, column-major 6 columns of 8 bits. */
static const uint8_t FONT6[96][6] = {
	{0,0,0,0,0,0},{0,0,0x5F,0,0,0},{0,7,0,7,0,0},{0x14,0x7F,0x14,0x7F,0x14,0},
	{0x24,0x2A,0x7F,0x2A,0x12,0},{0x23,0x13,0x08,0x64,0x62,0},{0x36,0x49,0x55,0x22,0x50,0},
	{0,5,3,0,0,0},{0,0x1C,0x22,0x41,0,0},{0,0x41,0x22,0x1C,0,0},{0x14,0x08,0x3E,0x08,0x14,0},
	{0x08,0x08,0x3E,0x08,0x08,0},{0,0x50,0x30,0,0,0},{0x08,0x08,0x08,0x08,0x08,0},
	{0,0x60,0x60,0,0,0},{0x20,0x10,0x08,0x04,0x02,0},{0x3E,0x51,0x49,0x45,0x3E,0},
	{0,0x42,0x7F,0x40,0,0},{0x42,0x61,0x51,0x49,0x46,0},{0x21,0x41,0x45,0x4B,0x31,0},
	{0x18,0x14,0x12,0x7F,0x10,0},{0x27,0x45,0x45,0x45,0x39,0},{0x3C,0x4A,0x49,0x49,0x30,0},
	{0x01,0x71,0x09,0x05,0x03,0},{0x36,0x49,0x49,0x49,0x36,0},{0x06,0x49,0x49,0x29,0x1E,0},
	{0,0x36,0x36,0,0,0},{0,0x56,0x36,0,0,0},{0x08,0x14,0x22,0x41,0,0},
	{0x14,0x14,0x14,0x14,0x14,0},{0,0x41,0x22,0x14,0x08,0},{0x02,0x01,0x51,0x09,0x06,0},
	{0x32,0x49,0x79,0x41,0x3E,0},{0x7E,0x11,0x11,0x11,0x7E,0},{0x7F,0x49,0x49,0x49,0x36,0},
	{0x3E,0x41,0x41,0x41,0x22,0},{0x7F,0x41,0x41,0x22,0x1C,0},{0x7F,0x49,0x49,0x49,0x41,0},
	{0x7F,0x09,0x09,0x09,0x01,0},{0x3E,0x41,0x49,0x49,0x7A,0},{0x7F,0x08,0x08,0x08,0x7F,0},
	{0,0x41,0x7F,0x41,0,0},{0x20,0x40,0x41,0x3F,0x01,0},{0x7F,0x08,0x14,0x22,0x41,0},
	{0x7F,0x40,0x40,0x40,0x40,0},{0x7F,0x02,0x0C,0x02,0x7F,0},{0x7F,0x04,0x08,0x10,0x7F,0},
	{0x3E,0x41,0x41,0x41,0x3E,0},{0x7F,0x09,0x09,0x09,0x06,0},{0x3E,0x41,0x51,0x21,0x5E,0},
	{0x7F,0x09,0x19,0x29,0x46,0},{0x46,0x49,0x49,0x49,0x31,0},{0x01,0x01,0x7F,0x01,0x01,0},
	{0x3F,0x40,0x40,0x40,0x3F,0},{0x1F,0x20,0x40,0x20,0x1F,0},{0x3F,0x40,0x38,0x40,0x3F,0},
	{0x63,0x14,0x08,0x14,0x63,0},{0x07,0x08,0x70,0x08,0x07,0},{0x61,0x51,0x49,0x45,0x43,0},
	{0x7F,0x41,0x41,0,0,0},{0x02,0x04,0x08,0x10,0x20,0},{0,0x41,0x41,0x7F,0,0},
	{0x04,0x02,0x01,0x02,0x04,0},{0x40,0x40,0x40,0x40,0x40,0},{0,0x01,0x02,0x04,0,0},
	{0x20,0x54,0x54,0x54,0x78,0},{0x7F,0x48,0x44,0x44,0x38,0},{0x38,0x44,0x44,0x44,0x20,0},
	{0x38,0x44,0x44,0x48,0x7F,0},{0x38,0x54,0x54,0x54,0x18,0},{0x08,0x7E,0x09,0x01,0x02,0},
	{0x0C,0x52,0x52,0x52,0x3E,0},{0x7F,0x08,0x04,0x04,0x78,0},{0,0x44,0x7D,0x40,0,0},
	{0x20,0x40,0x44,0x3D,0,0},{0x7F,0x10,0x28,0x44,0,0},{0,0x41,0x7F,0x40,0,0},
	{0x7C,0x04,0x18,0x04,0x78,0},{0x7C,0x08,0x04,0x04,0x78,0},{0x38,0x44,0x44,0x44,0x38,0},
	{0x7C,0x14,0x14,0x14,0x08,0},{0x08,0x14,0x14,0x18,0x7C,0},{0x7C,0x08,0x04,0x04,0x08,0},
	{0x48,0x54,0x54,0x54,0x20,0},{0x04,0x3F,0x44,0x40,0x20,0},{0x3C,0x40,0x40,0x20,0x7C,0},
	{0x1C,0x20,0x40,0x20,0x1C,0},{0x3C,0x40,0x30,0x40,0x3C,0},{0x44,0x28,0x10,0x28,0x44,0},
	{0x0C,0x50,0x50,0x50,0x3C,0},{0x44,0x64,0x54,0x4C,0x44,0},{0x08,0x36,0x41,0,0,0},
	{0,0,0x7F,0,0,0},{0,0x41,0x36,0x08,0,0},{0x08,0x04,0x08,0x10,0x08,0},
	{0x00,0x00,0x00,0x00,0x00,0}
};

void video_overlay_text(int x, int y, const char* text, uint16_t color) {
	if (!s_top) s_top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	if (!s_top || !text) return;
	int cx = x;
	for (const char* p = text; *p; p++) {
		unsigned c = (unsigned char)*p;
		if (c == '\n') { y += 9; cx = x; continue; }
		if (c < 32 || c > 127) c = '?';
		const uint8_t* g = FONT6[c - 32];
		for (int col = 0; col < 6; col++) {
			uint8_t bits = g[col];
			for (int row = 0; row < 8; row++) {
				if (bits & (1u << row)) plot(cx + col, y + row, color);
			}
		}
		cx += 6;
	}
}

#define LOG_LINES 18
#define LOG_LEN 70
static char s_log[LOG_LINES][LOG_LEN];
static int s_logCount = 0;
static bool s_logDraw = true;

void video_log(const char* msg) {
	if (!msg || !s_logDraw) return;
	if (s_logCount < LOG_LINES) {
		strncpy(s_log[s_logCount], msg, LOG_LEN - 1);
		s_log[s_logCount][LOG_LEN - 1] = 0;
		s_logCount++;
	} else {
		memmove(s_log[0], s_log[1], (LOG_LINES - 1) * LOG_LEN);
		strncpy(s_log[LOG_LINES - 1], msg, LOG_LEN - 1);
		s_log[LOG_LINES - 1][LOG_LEN - 1] = 0;
	}
	/* Only used while a ROM is loading. Drawing this over a running game
	   leaves text in the margins of one buffer, which then flashes. */
	gfxSetDoubleBuffering(GFX_TOP, true);
	s_top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	if (!s_top) return;
	memset(s_top, 0, TOP_WIDTH * TOP_HEIGHT * 2);
	video_overlay_text(6, 6, "AGB-RA", 0xFFE0);
	for (int i = 0; i < s_logCount; i++) {
		video_overlay_text(6, 18 + i * 10, s_log[i], 0xFFFF);
	}
	if (s_top) GSPGPU_FlushDataCache(s_top, TOP_WIDTH * TOP_HEIGHT * sizeof(uint16_t));
	gfxScreenSwapBuffers(GFX_TOP, false);
}

void video_wipe(void) {
	s_logDraw = false;
	s_logCount = 0;
	for (int i = 0; i < 2; i++) {
		s_top = (uint16_t*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		if (s_top) {
			memset(s_top, 0, TOP_WIDTH * TOP_HEIGHT * 2);
			GSPGPU_FlushDataCache(s_top, TOP_WIDTH * TOP_HEIGHT * 2);
		}
		/* Top only. Swapping the bottom here flashes a stale console page. */
		gfxScreenSwapBuffers(GFX_TOP, false);
		gspWaitForVBlank();
	}
	s_top = NULL;
}

void video_flush_nosync(void) {
	/* Swap the top screen only. Flushing and flipping the bottom buffer
	   every emulated frame redrew a stale console page (flicker) and
	   pushed another 230KB through the cache on an old 3DS. */
	if (s_top) GSPGPU_FlushDataCache(s_top, TOP_WIDTH * TOP_HEIGHT * sizeof(uint16_t));
	gfxScreenSwapBuffers(GFX_TOP, false);
}

void video_emu_frame(void) {
	u64 now = osGetTime();
	if (s_fpsT == 0) s_fpsT = now;
	s_fpsFrames++;
	if (now - s_fpsT >= 1000) {
		s_fps = s_fpsFrames;
		s_fpsFrames = 0;
		s_fpsT = now;
	}
}

int video_fps(void) {
	return s_fps;
}

void video_flush(void) {
	video_flush_nosync();
	gspWaitForVBlank();
	s_presentMs = osGetTime();
}
