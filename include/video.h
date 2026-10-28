#pragma once

#include <stdint.h>

void video_init(void);
void video_exit(void);
void video_present(const uint16_t* pixels, int stride);
void video_overlay_text(int x, int y, const char* text, uint16_t color);
void video_overlay_bar(int x, int y, int w, int h, uint16_t color);
void video_flush(void);
void video_flush_nosync(void);
void video_emu_frame(void);
int video_fps(void);
void video_log(const char* msg);
void video_wipe(void);
void video_set_skip(int skip);
