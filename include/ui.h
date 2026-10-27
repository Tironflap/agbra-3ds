#pragma once

#include <3ds.h>

void ui_init(void);
void ui_draw(void);
void ui_handle(u32 down, u32 held);
/* Drop the ROM list and show the in-game panel on the bottom screen. */
void ui_show_playing(void);
