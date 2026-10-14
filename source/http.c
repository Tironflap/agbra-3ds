#include "http.h"
#include "app.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_JOBS 8
#define HTTP_BODY_MAX (512 * 1024)

typedef struct HttpJob {
	char url[768];
	char* post;
	char contentType[96];
	rc_client_server_callback_t callback;
	void* callback_data;
	char* body;
	size_t bodyLen;
	int status;
	volatile int done;
	volatile int inUse;
} HttpJob;

static HttpJob s_jobs[HTTP_MAX_JOBS];
static LightLock s_lock;
static Thread s_thread;
static volatile bool s_run = false;

static int http_download(HttpJob* job) {
	httpcContext ctx;
	Result ret = httpcOpenContext(&ctx, job->post ? HTTPC_METHOD_POST : HTTPC_METHOD_GET, job->url, 1);
	if (R_FAILED(ret)) return -1;

	httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
	httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
	httpcAddRequestHeaderField(&ctx, "User-Agent", AGBRA_USER_AGENT);
	httpcAddRequestHeaderField(&ctx, "Connection", "Keep-Alive");
	if (job->post) {
		const char* ct = job->contentType[0] ? job->contentType : "application/x-www-form-urlencoded";
		httpcAddRequestHeaderField(&ctx, "Content-Type", ct);
		u32 len = (u32)strlen(job->post);
		httpcAddPostDataRaw(&ctx, (u32*)job->post, len);
	}

	ret = httpcBeginRequest(&ctx);
	if (R_FAILED(ret)) {
		httpcCloseContext(&ctx);
		return -2;
	}

	u32 status = 0;
	httpcGetResponseStatusCode(&ctx, &status);
	job->status = (int)status;

	u32 size = 0;
	httpcGetDownloadSizeState(&ctx, NULL, &size);
	if (size > HTTP_BODY_MAX) size = HTTP_BODY_MAX;
	if (size == 0) size = 64 * 1024;

	char* buf = malloc(size + 1);
	if (!buf) {
		httpcCloseContext(&ctx);
		return -3;
	}

	u32 total = 0;
	while (total < HTTP_BODY_MAX) {
		if (total >= size) {
			size *= 2;
			if (size > HTTP_BODY_MAX) size = HTTP_BODY_MAX;
			char* nbuf = realloc(buf, size + 1);
			if (!nbuf) break;
			buf = nbuf;
		}
		u32 got = 0;
		ret = httpcDownloadData(&ctx, (u8*)buf + total, size - total, &got);
		total += got;
		if (ret != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
	}
	buf[total] = 0;
	job->body = buf;
	job->bodyLen = total;
	httpcCloseContext(&ctx);
	return 0;
}

static void http_thread(void* arg) {
	(void)arg;
	while (s_run) {
		HttpJob* job = NULL;
		LightLock_Lock(&s_lock);
		for (int i = 0; i < HTTP_MAX_JOBS; i++) {
			if (s_jobs[i].inUse && !s_jobs[i].done && !s_jobs[i].body && s_jobs[i].url[0]) {
				job = &s_jobs[i];
				break;
			}
		}
		LightLock_Unlock(&s_lock);

		if (!job) {
			svcSleepThread(5 * 1000 * 1000);
			continue;
		}
		http_download(job);
		job->done = 1;
	}
}

bool http_init(void) {
	memset(s_jobs, 0, sizeof(s_jobs));
	LightLock_Init(&s_lock);
	acInit();
	httpcInit(0x4000);
	s_run = true;
	s_thread = threadCreate(http_thread, NULL, 64 * 1024, 0x30, -1, false);
	return true;
}

void http_exit(void) {
	s_run = false;
	if (s_thread) {
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
	}
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		free(s_jobs[i].post);
		free(s_jobs[i].body);
	}
	httpcExit();
	acExit();
}

void http_server_call(const rc_api_request_t* request,
	rc_client_server_callback_t callback, void* callback_data, rc_client_t* client) {
	(void)client;
	LightLock_Lock(&s_lock);
	HttpJob* job = NULL;
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		if (!s_jobs[i].inUse) {
			job = &s_jobs[i];
			break;
		}
	}
	if (!job) {
		LightLock_Unlock(&s_lock);
		rc_api_server_response_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.http_status_code = 0;
		callback(&resp, callback_data);
		return;
	}
	memset(job, 0, sizeof(*job));
	job->inUse = 1;
	strncpy(job->url, request->url ? request->url : "", sizeof(job->url) - 1);
	if (request->post_data) job->post = strdup(request->post_data);
	if (request->content_type) strncpy(job->contentType, request->content_type, sizeof(job->contentType) - 1);
	job->callback = callback;
	job->callback_data = callback_data;
	LightLock_Unlock(&s_lock);
}

void http_poll(void) {
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		HttpJob* job = &s_jobs[i];
		if (!job->inUse || !job->done) continue;
		rc_api_server_response_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.body = job->body;
		resp.body_length = job->bodyLen;
		resp.http_status_code = job->status > 0 ? job->status : 0;
		if (job->callback) job->callback(&resp, job->callback_data);
		free(job->post);
		free(job->body);
		memset(job, 0, sizeof(*job));
	}
}
