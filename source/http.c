#include "http.h"
#include "app.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

/* libctru's socket.h has no timeout options. The 3DS soc:u service uses
   the same codes as the rest of the Nintendo homebrew stacks. */
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x1005
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 0x1006
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <fcntl.h>
#include <unistd.h>

#define HTTP_MAX_JOBS 8
#define HTTP_BODY_MAX (512 * 1024)
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x80000
#define OFFLINE_MAX 32
#define OFFLINE_PATH AGBRA_DIR "/offline.queue"

typedef struct HttpJob {
	char url[1024];
	char* post;
	char contentType[96];
	char auth[1100];
	char savePath[512];
	char offlineKey[16];
	rc_client_server_callback_t callback;
	void* callback_data;
	char* body;
	size_t bodyLen;
	int status;
	volatile int done;
	volatile int running;
	volatile int inUse;
} HttpJob;

static volatile uint32_t s_progGot;
static volatile uint32_t s_progTotal;
static volatile int s_progOn;

static HttpJob s_jobs[HTTP_MAX_JOBS];
static LightLock s_lock;
static u32* s_soc;
static int s_net;
static Thread s_thread;
static volatile int s_run;
static char s_ua[180];
static char s_coreClause[48];

static void build_user_agent(void) {
	char ver[40];
	char osver[24];
	osver[0] = 0;
	ver[0] = 0;
	if (R_SUCCEEDED(osGetSystemVersionDataString(NULL, NULL, ver, sizeof(ver))) && ver[0]) {
		size_t n = strlen(ver);
		if (n && ver[n - 1] >= 'A' && ver[n - 1] <= 'Z') ver[n - 1] = 0;
		snprintf(osver, sizeof(osver), "%s", ver);
	}
	if (osver[0] && s_coreClause[0])
		snprintf(s_ua, sizeof(s_ua), "AGB-RA/%s (Nintendo 3DS %s) %s", AGBRA_VERSION, osver, s_coreClause);
	else if (osver[0])
		snprintf(s_ua, sizeof(s_ua), "AGB-RA/%s (Nintendo 3DS %s)", AGBRA_VERSION, osver);
	else if (s_coreClause[0])
		snprintf(s_ua, sizeof(s_ua), "AGB-RA/%s (Nintendo 3DS) %s", AGBRA_VERSION, s_coreClause);
	else
		snprintf(s_ua, sizeof(s_ua), "%s", AGBRA_USER_AGENT);
}

void http_set_core(const char* coreClause) {
	LightLock_Lock(&s_lock);
	s_coreClause[0] = 0;
	if (coreClause && coreClause[0]) {
		strncpy(s_coreClause, coreClause, sizeof(s_coreClause) - 1);
		s_coreClause[sizeof(s_coreClause) - 1] = 0;
	}
	build_user_agent();
	LightLock_Unlock(&s_lock);
}

static int parse_url(const char* url, char* host, size_t hostLen, char* path, size_t pathLen, int* https, int* port) {
	const char* p = url;
	*https = 1;
	*port = 443;
	if (strncmp(p, "https://", 8) == 0) p += 8;
	else if (strncmp(p, "http://", 7) == 0) {
		*https = 0;
		*port = 80;
		p += 7;
	} else {
		return 0;
	}
	const char* slash = strchr(p, '/');
	size_t hostportLen = slash ? (size_t)(slash - p) : strlen(p);
	const char* colon = memchr(p, ':', hostportLen);
	size_t hlen = colon ? (size_t)(colon - p) : hostportLen;
	if (!hlen || hlen >= hostLen) return 0;
	memcpy(host, p, hlen);
	host[hlen] = 0;
	if (colon) {
		int parsed = atoi(colon + 1);
		if (parsed > 0 && parsed < 65536) *port = parsed;
	}
	if (!slash) {
		memcpy(path, "/", 2);
		return 1;
	}
	if (strlen(slash) >= pathLen) return 0;
	memcpy(path, slash, strlen(slash) + 1);
	return 1;
}

static int write_all(int fd, const void* buf, size_t len) {
	const unsigned char* p = buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) return -1;
		if (n == 0) return -1;
		off += (size_t)n;
	}
	return 0;
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

static int http_download_hop(HttpJob* job, int hop);
static int http_download(HttpJob* job) {
	return http_download_hop(job, 0);
}

static int http_download_hop(HttpJob* job, int hop) {
	char host[128], path[1000];
	int https = 1;
	int portNum = 80;
	if (!s_net || !parse_url(job->url, host, sizeof(host), path, sizeof(path), &https, &portNum)) return -1;

	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	int handshake = 0;
	char port[8];
	snprintf(port, sizeof(port), "%d", portNum);

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
	tv.tv_sec = job->savePath[0] ? 30 : 8;
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
	char ua[180];
	LightLock_Lock(&s_lock);
	strncpy(ua, s_ua[0] ? s_ua : AGBRA_USER_AGENT, sizeof(ua) - 1);
	ua[sizeof(ua) - 1] = 0;
	LightLock_Unlock(&s_lock);
	char hostHeader[160];
	if ((https && portNum == 443) || (!https && portNum == 80))
		snprintf(hostHeader, sizeof(hostHeader), "%s", host);
	else
		snprintf(hostHeader, sizeof(hostHeader), "%s:%d", host, portNum);
	char header[1800];
	int hlen = snprintf(header, sizeof(header),
		"%s %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: %s\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n",
		job->post ? "POST" : "GET", path, hostHeader, ua);
	if (hlen < 0 || (size_t)hlen >= sizeof(header)) goto fail;
	if (job->post) {
		int extra = snprintf(header + hlen, sizeof(header) - (size_t)hlen,
			"Content-Type: %s\r\nContent-Length: %u\r\n", ct, (unsigned)postLen);
		if (extra < 0 || (size_t)hlen + (size_t)extra >= sizeof(header)) goto fail;
		hlen += extra;
	}
	if (job->auth[0]) {
		int extra = snprintf(header + hlen, sizeof(header) - (size_t)hlen,
			"Authorization: %s\r\n", job->auth);
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
	int streaming = job->savePath[0] != 0;
	char location[768];
	location[0] = 0;
	u64 started = osGetTime();
	u64 lastData = started;
	for (;;) {
		u64 now = osGetTime();
		if (streaming) {
			if (now - lastData > 30000) break;
		} else if (now - started > 12000) break;
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
		lastData = osGetTime();
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
					} else if (strncasecmp(line, "Location:", 9) == 0) {
						const char* v = line + 9;
						while (*v == ' ') v++;
						size_t ln = (size_t)(eol - v);
						if (ln >= sizeof(location)) ln = sizeof(location) - 1;
						memcpy(location, v, ln);
						location[ln] = 0;
					}
					line = eol + 2;
				}
			}
		}
		if (streaming && headerEnd) break;
		if (!streaming && headerEnd && contentLength >= 0 && rawLen >= headerEnd + (size_t)contentLength) break;
		if (!streaming && rawLen >= HTTP_BODY_MAX) break;
		if (streaming && !headerEnd && rawLen > 16384) break;
	}

	job->status = 0;
	job->body = NULL;
	job->bodyLen = 0;
	if (raw && strncmp(raw, "HTTP/", 5) == 0) {
		char* sp = strchr(raw, ' ');
		if (sp) job->status = atoi(sp + 1);
	}

	if (location[0] && job->status >= 300 && job->status < 400 && hop < 2) {
		char next[1024];
		next[0] = 0;
		if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0)
			strncpy(next, location, sizeof(next) - 1);
		else if (location[0] == '/')
			snprintf(next, sizeof(next), "%s://%s:%d%s", https ? "https" : "http", host, portNum, location);
		free(raw);
		if (https && handshake) mbedtls_ssl_close_notify(&ssl);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_net_free(&net);
		if (!next[0]) return -2;
		strncpy(job->url, next, sizeof(job->url) - 1);
		job->url[sizeof(job->url) - 1] = 0;
		if (job->post) {
			free(job->post);
			job->post = NULL;
		}
		return http_download_hop(job, hop + 1);
	}

	if (streaming) {
		int httpOk = job->status >= 200 && job->status < 300;
		const char* bodyStart = (raw && headerEnd && rawLen >= headerEnd) ? raw + headerEnd : NULL;
		size_t bodyHave = bodyStart ? rawLen - headerEnd : 0;
		if (contentLength >= 0 && bodyHave > (size_t)contentLength) bodyHave = (size_t)contentLength;
		if (!httpOk) {
			size_t n = bodyHave > 180 ? 180 : bodyHave;
			if (n && bodyStart) {
				job->body = malloc(n + 1);
				if (job->body) {
					memcpy(job->body, bodyStart, n);
					job->body[n] = 0;
					job->bodyLen = n;
				}
			}
		} else {
			char part[540];
			int pl = snprintf(part, sizeof(part), "%s.part", job->savePath);
			int fd = (pl > 0 && (size_t)pl < sizeof(part)) ? open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
			size_t got = 0;
			int failed = fd < 0;
			if (!failed) {
				s_progOn = 1;
				s_progTotal = contentLength > 0 ? (uint32_t)contentLength : 0;
				s_progGot = 0;
				if (bodyHave && write_all(fd, bodyStart, bodyHave) != 0) failed = 1;
				got = failed ? 0 : bodyHave;
				s_progGot = (uint32_t)got;
			}
			unsigned char chunk[4096];
			u64 last = osGetTime();
			while (!failed && (contentLength < 0 || got < (size_t)contentLength)) {
				if (got >= 64u * 1024u * 1024u) break;
				if (osGetTime() - last > 30000) { failed = 1; break; }
				int n;
				if (https) {
					n = mbedtls_ssl_read(&ssl, chunk, sizeof(chunk));
					if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
					if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) break;
					if (n < 0) { failed = 1; break; }
				} else {
					n = mbedtls_net_recv(&net, chunk, sizeof(chunk));
					if (n == MBEDTLS_ERR_SSL_WANT_READ) continue;
					if (n <= 0) break;
				}
				last = osGetTime();
				size_t take = (size_t)n;
				if (contentLength >= 0 && got + take > (size_t)contentLength)
					take = (size_t)contentLength - got;
				if (take && write_all(fd, chunk, take) != 0) { failed = 1; break; }
				got += take;
				s_progGot = (uint32_t)got;
			}
			if (fd >= 0) {
				fsync(fd);
				close(fd);
			}
			s_progOn = 0;
			if (!failed && contentLength > 0 && got < (size_t)contentLength) failed = 1;
			if (!failed && got == 0) failed = 1;
			if (failed) {
				if (pl > 0) unlink(part);
				job->status = 0;
				job->body = strdup("Save failed");
				job->bodyLen = job->body ? strlen(job->body) : 0;
			} else {
				unlink(job->savePath);
				if (rename(part, job->savePath) != 0) {
					unlink(part);
					job->status = 0;
					job->body = strdup("Save failed");
					job->bodyLen = job->body ? strlen(job->body) : 0;
				} else {
					job->bodyLen = got;
				}
			}
		}
		free(raw);
	} else if (raw) {
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
	build_user_agent();
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
	/* Core 1, so TLS and achievement uploads do not steal the emulator. */
	s_thread = threadCreate(http_thread, NULL, 128 * 1024, 0x30, 1, false);
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

void http_request(const char* url, const char* post, const char* contentType,
	const char* authorization, const char* savePath,
	rc_client_server_callback_t callback, void* callback_data) {
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
		if (callback) {
			rc_api_server_response_t resp;
			memset(&resp, 0, sizeof(resp));
			callback(&resp, callback_data);
		}
		return;
	}
	memset(job, 0, sizeof(*job));
	job->inUse = 1;
	if (url) strncpy(job->url, url, sizeof(job->url) - 1);
	if (post) job->post = strdup(post);
	if (contentType) strncpy(job->contentType, contentType, sizeof(job->contentType) - 1);
	if (authorization) strncpy(job->auth, authorization, sizeof(job->auth) - 1);
	if (savePath) strncpy(job->savePath, savePath, sizeof(job->savePath) - 1);
	job->callback = callback;
	job->callback_data = callback_data;
	LightLock_Unlock(&s_lock);
}

void http_stream_progress(uint32_t* got, uint32_t* total, int* active) {
	if (got) *got = s_progGot;
	if (total) *total = s_progTotal;
	if (active) *active = s_progOn;
}

typedef struct {
	char key[16];
	char url[192];
	char* post;
} OfflineSlot;

static OfflineSlot s_off[OFFLINE_MAX];
static int s_offCount = 0;
static int s_offReady = 0;
static int s_replay = 0;
static u64 s_replayAt = 0;

static int offline_kind(const char* post) {
	if (!post) return 0;
	if (strstr(post, "r=awardachievement")) return 1;
	if (strstr(post, "r=submitlbentry")) return 2;
	return 0;
}

static int param_id(const char* post, const char* key, uint32_t* out) {
	size_t klen = strlen(key);
	const char* p = post;
	while (p && (p = strstr(p, key))) {
		if ((p == post || p[-1] == '&') && p[klen] == '=') {
			*out = (uint32_t)strtoul(p + klen + 1, NULL, 10);
			return 1;
		}
		p += klen;
	}
	return 0;
}

static void offline_save(void) {
	FILE* f = fopen(OFFLINE_PATH, "w");
	if (!f) return;
	for (int i = 0; i < s_offCount; i++) {
		if (!s_off[i].post || !s_off[i].key[0]) continue;
		fprintf(f, "%s\t%s\t%s\n", s_off[i].key, s_off[i].url, s_off[i].post);
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

static void offline_load(void) {
	char line[2048];
	FILE* f;
	if (s_offReady) return;
	s_offReady = 1;
	f = fopen(OFFLINE_PATH, "r");
	if (!f) return;
	while (s_offCount < OFFLINE_MAX && fgets(line, sizeof(line), f)) {
		char* tab1 = strchr(line, '\t');
		char* tab2;
		char* post;
		size_t plen;
		OfflineSlot* slot;
		if (!tab1) continue;
		tab2 = strchr(tab1 + 1, '\t');
		if (!tab2) continue;
		*tab1 = 0;
		*tab2 = 0;
		post = tab2 + 1;
		plen = strlen(post);
		while (plen && (post[plen - 1] == '\n' || post[plen - 1] == '\r')) post[--plen] = 0;
		if (!line[0] || !post[0]) continue;
		slot = &s_off[s_offCount];
		memset(slot, 0, sizeof(*slot));
		snprintf(slot->key, sizeof(slot->key), "%s", line);
		strncpy(slot->url, tab1 + 1, sizeof(slot->url) - 1);
		slot->post = strdup(post);
		if (slot->post) s_offCount++;
	}
	fclose(f);
}

static void offline_upsert(const char* key, const char* url, const char* post) {
	int i;
	offline_load();
	if (!key || !key[0] || !url || !post) return;
	for (i = 0; i < s_offCount; i++) {
		if (strcmp(s_off[i].key, key) != 0) continue;
		free(s_off[i].post);
		s_off[i].post = strdup(post);
		strncpy(s_off[i].url, url, sizeof(s_off[i].url) - 1);
		s_off[i].url[sizeof(s_off[i].url) - 1] = 0;
		offline_save();
		return;
	}
	if (s_offCount >= OFFLINE_MAX) return;
	memset(&s_off[s_offCount], 0, sizeof(s_off[0]));
	snprintf(s_off[s_offCount].key, sizeof(s_off[0].key), "%s", key);
	strncpy(s_off[s_offCount].url, url, sizeof(s_off[0].url) - 1);
	s_off[s_offCount].post = strdup(post);
	if (!s_off[s_offCount].post) return;
	s_offCount++;
	offline_save();
}

static void offline_remove(const char* key) {
	int i;
	offline_load();
	if (!key || !key[0]) return;
	for (i = 0; i < s_offCount; i++) {
		if (strcmp(s_off[i].key, key) != 0) continue;
		free(s_off[i].post);
		s_off[i] = s_off[s_offCount - 1];
		memset(&s_off[s_offCount - 1], 0, sizeof(s_off[0]));
		s_offCount--;
		offline_save();
		return;
	}
}

static void offline_note(HttpJob* job) {
	int kind;
	uint32_t id = 0;
	char key[16];
	int ok;
	if (!job) return;
	ok = job->status >= 200 && job->status < 300;
	if (job->offlineKey[0]) {
		if (ok) offline_remove(job->offlineKey);
		return;
	}
	kind = offline_kind(job->post);
	if (!kind || !job->url[0] || !job->post) return;
	if (kind == 1) {
		if (!param_id(job->post, "a", &id)) return;
	} else if (!param_id(job->post, "i", &id)) {
		return;
	}
	snprintf(key, sizeof(key), "%c%u", kind == 1 ? 'A' : 'L', id);
	if (ok) offline_remove(key);
	else offline_upsert(key, job->url, job->post);
}

static void offline_pump(void) {
	int busy = 0;
	int slot = -1;
	if (s_replay) return;
	if (s_replayAt && osGetTime() < s_replayAt) return;
	offline_load();
	if (s_offCount <= 0 || !s_off[0].post || !s_off[0].url[0]) return;
	LightLock_Lock(&s_lock);
	for (int i = 0; i < HTTP_MAX_JOBS; i++) {
		if (s_jobs[i].inUse) busy = 1;
		else if (slot < 0) slot = i;
	}
	if (!busy && slot >= 0) {
		HttpJob* job = &s_jobs[slot];
		memset(job, 0, sizeof(*job));
		job->inUse = 1;
		strncpy(job->url, s_off[0].url, sizeof(job->url) - 1);
		job->post = strdup(s_off[0].post);
		strncpy(job->contentType, "application/x-www-form-urlencoded", sizeof(job->contentType) - 1);
		snprintf(job->offlineKey, sizeof(job->offlineKey), "%s", s_off[0].key);
		if (!job->post) {
			memset(job, 0, sizeof(*job));
			slot = -1;
		}
	}
	LightLock_Unlock(&s_lock);
	if (!busy && slot >= 0) s_replay = 1;
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
		int ok = job->status >= 200 && job->status < 300;
		if (job->offlineKey[0]) {
			s_replay = 0;
			s_replayAt = ok ? 0 : osGetTime() + 5000;
		}
		offline_note(job);
		memset(&resp, 0, sizeof(resp));
		resp.body = job->body;
		resp.body_length = job->bodyLen;
		resp.http_status_code = job->status > 0 ? job->status : 0;
		if (job->callback) job->callback(&resp, job->callback_data);
		free(job->post);
		free(job->body);
	}
	offline_pump();
}
