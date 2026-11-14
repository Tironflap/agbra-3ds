#pragma once

#include "rc_client.h"

#include <stdint.h>

bool http_init(void);
void http_exit(void);
void http_server_call(const rc_api_request_t* request,
	rc_client_server_callback_t callback, void* callback_data, rc_client_t* client);
/* authorization is the header value, for example "Bearer <token>".
   savePath streams the body to that file instead of keeping it in RAM.
   The callback runs on the main thread from http_poll. */
void http_request(const char* url, const char* post, const char* contentType,
	const char* authorization, const char* savePath,
	rc_client_server_callback_t callback, void* callback_data);
void http_stream_progress(uint32_t* got, uint32_t* total, int* active);
void http_poll(void);
/* coreClause is "gpsp/1.1.0" or empty. It is the third segment of the
 * RetroAchievements user agent. */
void http_set_core(const char* coreClause);
