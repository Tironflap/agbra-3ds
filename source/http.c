#include "http.h"
#include "app.h"
#include "rc_version.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#define HTTP_MAX_JOBS 8
#define HTTP_BODY_MAX (256 * 1024)
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x80000

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
	volatile int running;
	volatile int inUse;
} HttpJob;

static HttpJob s_jobs[HTTP_MAX_JOBS];
static LightLock s_lock;
static u32* s_soc;
static int s_net;
static Thread s_thread;
static volatile int s_run;

static int parse_url(const char* url, char* host, size_t hostLen, char* path, size_t pathLen, int* https) {
	const char* p = url;
	*https = 1;
	if (strncmp(p, "https://", 8) == 0) p += 8;
	else if (strncmp(p, "http://", 7) == 0) {
		*https = 0;
		p += 7;
	} else {
		return 0;
	}
	const char* slash = strchr(p, '/');
	size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
	const char* colon = memchr(p, ':', hlen);
	if (colon) hlen = (size_t)(colon - p);
	if (!hlen || hlen >= hostLen) return 0;
	memcpy(host, p, hlen);
	host[hlen] = 0;
	if (!slash) {
		memcpy(path, "/", 2);
		return 1;
	}
	if (strlen(slash) >= pathLen) return 0;
	memcpy(path, slash, strlen(slash) + 1);
	return 1;
}

static int net_send_all(mbedtls_ssl_context* ssl, const unsigned char* data, size_t len) {
	size_t off = 0;
	while (off < len) {
		int n = mbedtls_ssl_write(ssl, data + off, len - off);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (n <= 0) return n;
		off += (size_t)n;
	}
	return 0;
}

static int plain_send_all(mbedtls_net_context* net, const unsigned char* data, size_t len) {
	size_t off = 0;
	while (off < len) {
		int n = mbedtls_net_send(net, data + off, len - off);
		if (n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (n <= 0) return -1;
		off += (size_t)n;
	}
	return 0;
}

static int append_bytes(char** buf, size_t* len, size_t* cap, const char* src, size_t n) {
	if (*len + n + 1 > HTTP_BODY_MAX) n = HTTP_BODY_MAX - *len;
	if (!n) return 0;
	if (*len + n + 1 > *cap) {
		size_t ncap = *cap ? *cap * 2 : 2048;
		while (ncap < *len + n + 1) ncap *= 2;
		if (ncap > HTTP_BODY_MAX + 1) ncap = HTTP_BODY_MAX + 1;
		char* grown = realloc(*buf, ncap);
		if (!grown) return -1;
		*buf = grown;
		*cap = ncap;
	}
	memcpy(*buf + *len, src, n);
	*len += n;
	(*buf)[*len] = 0;
	return 0;
}

static int http_download(HttpJob* job) {
	char host[128], path[640];
	int https = 1;
	if (!s_net || !parse_url(job->url, host, sizeof(host), path, sizeof(path), &https)) return -1;

	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	int handshake = 0;
	char port[8];
	snprintf(port, sizeof(port), "%d", https ? 443 : 80);

	mbedtls_net_init(&net);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);

	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)"agbra", 5) != 0) {
		mbedtls_net_free(&net);
		return -2;
	}
	if (mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP) != 0) {
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_net_free(&net);
		return -2;
	}
	struct timeval tv;
	tv.tv_sec = 8;
	tv.tv_usec = 0;
	setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (https) {
		if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
				MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
			goto fail;
		}
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
		if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto fail;
		if (mbedtls_ssl_set_hostname(&ssl, host) != 0) goto fail;
		mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
		int ret;
		while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
			if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
		}
		handshake = 1;
	}

	const char* ct = job->contentType[0] ? job->contentType : "application/x-www-form-urlencoded";
	size_t postLen = job->post ? strlen(job->post) : 0;
	char header[1024];
	int hlen = snprintf(header, sizeof(header),
		"%s %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: AGB-RA/1.0 rcheevos/" RCHEEVOS_VERSION_STRING "\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n",
		job->post ? "POST" : "GET", path, host);
	if (hlen < 0 || (size_t)hlen >= sizeof(header)) goto fail;
	if (job->post) {
		int extra = snprintf(header + hlen, sizeof(header) - (size_t)hlen,
			"Content-Type: %s\r\nContent-Length: %u\r\n", ct, (unsigned)postLen);
		if (extra < 0 || (size_t)hlen + (size_t)extra >= sizeof(header)) goto fail;
		hlen += extra;
	}
	if ((size_t)hlen + 3 >= sizeof(header)) goto fail;
	memcpy(header + hlen, "\r\n", 3);
	hlen += 2;

	if (https) {
		if (net_send_all(&ssl, (unsigned char*)header, (size_t)hlen) != 0) goto fail;
		if (postLen && net_send_all(&ssl, (unsigned char*)job->post, postLen) != 0) goto fail;
	} else {
		if (plain_send_all(&net, (unsigned char*)header, (size_t)hlen) != 0) goto fail;
		if (postLen && plain_send_all(&net, (unsigned char*)job->post, postLen) != 0) goto fail;
	}

	char* raw = NULL;
	size_t rawLen = 0, rawCap = 0;
	unsigned char tmp[1024];
	int contentLength = -1;
	size_t headerEnd = 0;
	u64 started = osGetTime();
	for (;;) {
		if (osGetTime() - started > 12000) break;
		int n;
		if (https) {
			n = mbedtls_ssl_read(&ssl, tmp, sizeof(tmp));
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) break;
			if (n < 0) break;
		} else {
			n = mbedtls_net_recv(&net, tmp, sizeof(tmp));
			if (n == MBEDTLS_ERR_SSL_WANT_READ) continue;
			if (n <= 0) break;
		}
		if (append_bytes(&raw, &rawLen, &rawCap, (char*)tmp, (size_t)n) != 0) break;
		if (!headerEnd) {
			char* split = strstr(raw, "\r\n\r\n");
			if (split) {
				headerEnd = (size_t)(split + 4 - raw);
				char* line = raw;
				while (line && line < raw + headerEnd) {
					char* eol = strstr(line, "\r\n");
					if (!eol || eol >= raw + headerEnd) break;
					if (strncasecmp(line, "Content-Length:", 15) == 0) {
						contentLength = atoi(line + 15);
					}
					line = eol + 2;
				}
			}
		}
		if (headerEnd && contentLength >= 0 && rawLen >= headerEnd + (size_t)contentLength) break;
		if (rawLen >= HTTP_BODY_MAX) break;
	}

	job->status = 0;
	job->body = NULL;
	job->bodyLen = 0;
	if (raw) {
		if (strncmp(raw, "HTTP/", 5) == 0) {
			char* sp = strchr(raw, ' ');
			if (sp) job->status = atoi(sp + 1);
		}
		char* body = strstr(raw, "\r\n\r\n");
		if (body) {
			body += 4;
			size_t blen = rawLen - (size_t)(body - raw);
			memmove(raw, body, blen);
			raw[blen] = 0;
			job->body = raw;
			job->bodyLen = blen;
		} else {
			job->body = raw;
			job->bodyLen = rawLen;
		}
	}

	if (https && handshake) mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	mbedtls_net_free(&net);
	return 0;

fail:
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	mbedtls_net_free(&net);
	return -2;
}

static void http_thread(void* arg) {
	(void)arg;
	while (s_run) {
		HttpJob* job = NULL;
		LightLock_Lock(&s_lock);
		for (int i = 0; i < HTTP_MAX_JOBS; i++) {
			if (s_jobs[i].inUse && !s_jobs[i].done && !s_jobs[i].running && s_jobs[i].url[0]) {
				s_jobs[i].running = 1;
				job = &s_jobs[i];
				break;
			}
		}
		LightLock_Unlock(&s_lock);
		if (!job) {
			svcSleepThread(20000000LL);
			continue;
		}
		http_download(job);
		LightLock_Lock(&s_lock);
		job->done = 1;
		LightLock_Unlock(&s_lock);
	}
}

bool http_init(void) {
	memset(s_jobs, 0, sizeof(s_jobs));
	LightLock_Init(&s_lock);
	acInit();
	s_soc = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!s_soc) return false;
	if (R_FAILED(socInit(s_soc, SOC_BUFFERSIZE))) {
		free(s_soc);
		s_soc = NULL;
		return false;
	}
	s_net = 1;
	s_run = 1;
	s_thread = threadCreate(http_thread, NULL, 96 * 1024, 0x30, -1, false);
	return true;
}

void http_exit(void) {
	s_run = 0;
	if (s_thread) {
		if (R_SUCCEEDED(threadJoin(s_thread, 15000000000LL))) threadFree(s_thread);
		s_thread = NULL;
	}
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		free(s_jobs[i].post);
		free(s_jobs[i].body);
	}
	if (s_net) socExit();
	free(s_soc);
	s_soc = NULL;
	s_net = 0;
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
	HttpJob done[HTTP_MAX_JOBS];
	int nDone = 0;
	LightLock_Lock(&s_lock);
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		if (!s_jobs[i].inUse || !s_jobs[i].done) continue;
		done[nDone++] = s_jobs[i];
		memset(&s_jobs[i], 0, sizeof(s_jobs[i]));
	}
	LightLock_Unlock(&s_lock);

	/* No worker (thread failed to start): one request, still capped at 12s. */
	if (!s_thread) {
		HttpJob* pending = NULL;
		LightLock_Lock(&s_lock);
		for (int i = 0; i < HTTP_MAX_JOBS; i++) {
			if (s_jobs[i].inUse && !s_jobs[i].done && !s_jobs[i].running && s_jobs[i].url[0]) {
				s_jobs[i].running = 1;
				pending = &s_jobs[i];
				break;
			}
		}
		LightLock_Unlock(&s_lock);
		if (pending) {
			http_download(pending);
			LightLock_Lock(&s_lock);
			pending->done = 1;
			LightLock_Unlock(&s_lock);
		}
	}

	for (int i = 0; i < nDone; i++) {
		HttpJob* job = &done[i];
		rc_api_server_response_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.body = job->body;
		resp.body_length = job->bodyLen;
		resp.http_status_code = job->status > 0 ? job->status : 0;
		if (job->callback) job->callback(&resp, job->callback_data);
		free(job->post);
		free(job->body);
	}
}
