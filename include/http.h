#pragma once

#include "rc_client.h"

bool http_init(void);
void http_exit(void);
void http_server_call(const rc_api_request_t* request,
	rc_client_server_callback_t callback, void* callback_data, rc_client_t* client);
void http_poll(void);
