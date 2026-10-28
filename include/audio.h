#pragma once

#include <stddef.h>
#include <stdint.h>

bool audio_init(void);
bool audio_ready(void);
void audio_exit(void);
void audio_set_rate(float hz);
void audio_push(const int16_t* samples, size_t frames);
void audio_clear(void);
