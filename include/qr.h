#pragma once

#include <stddef.h>

/* Outer camera. Returns 1 and fills text, 0 if cancelled, -1 if the
 * camera could not start. */
int qr_scan(char* text, size_t textLen);
