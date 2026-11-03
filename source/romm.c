#include "romm.h"
#include "app.h"
#include "config.h"
#include "fs.h"
#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROMM_PAGE 20
#define ROMM_MAX 20

static RommRow s_rows[ROMM_MAX];
static int s_count;
static int s_total;
static int s_offset;
static int s_platformId;
static char s_slug[12];
static char s_platformName[72];
static char s_message[160];
static char s_downName[72];
static int s_downIndex;
static int s_downOverwrite;
static int s_downloading;
static int s_busy;
static int s_retryAuth;
static int s_contentFallback;
static RommPhase s_phase = ROMM_NEED_LOGIN;
/* What to repeat after a refresh: 1 platforms, 2 roms, 3 download. */
static int s_want;

static void url_encode(const char* in, char* out, size_t outLen) {
	static const char hex[] = "0123456789ABCDEF";
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 1 < outLen; i++) {
		unsigned char c = (unsigned char)in[i];
		int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		if (plain) {
			out[j++] = (char)c;
		} else {
			if (j + 3 >= outLen) break;
			out[j++] = '%';
			out[j++] = hex[c >> 4];
			out[j++] = hex[c & 15];
		}
	}
	out[j] = 0;
}

static void normalize_base(const char* in, char* out, size_t n) {
	while (*in == ' ') in++;
	if (strncmp(in, "http://", 7) != 0 && strncmp(in, "https://", 8) != 0)
		snprintf(out, n, "http://%s", in);
	else
		snprintf(out, n, "%s", in);
	size_t len = strlen(out);
	while (len && (out[len - 1] == '/' || out[len - 1] == ' ')) out[--len] = 0;
	if (len >= 4 && strcmp(out + len - 4, "/api") == 0) out[len - 4] = 0;
}

static const char* skip_ws(const char* p, const char* end) {
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
	return p;
}

static int decode_string(const char* p, const char* end, char* out, size_t outLen) {
	if (!p || p >= end || *p != '"' || outLen == 0) return 0;
	p++;
	size_t j = 0;
	while (p < end && *p != '"') {
		unsigned char c = (unsigned char)*p++;
		if (c == '\\' && p < end) {
			char e = *p++;
			if (e == 'n' || e == 'r' || e == 't') c = ' ';
			else if (e == 'u' && p + 4 <= end) {
				int cp = 0;
				for (int k = 0; k < 4; k++) {
					char h = *p++;
					cp <<= 4;
					if (h >= '0' && h <= '9') cp += h - '0';
					else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
				}
				if (cp < 0x80) c = (unsigned char)cp;
				else if (cp < 0x800 && j + 2 < outLen) {
					out[j++] = (char)(0xC0 | (cp >> 6));
					c = (unsigned char)(0x80 | (cp & 0x3F));
				} else c = '?';
			} else c = (unsigned char)e;
		}
		if (c < 32) c = ' ';
		if (j + 1 < outLen) out[j++] = (char)c;
	}
	out[j] = 0;
	return 1;
}

/* Value of a key on this object, not inside a nested object or array. */
static const char* value_of(const char* obj, const char* end, const char* key) {
	int depth = 0;
	int inStr = 0;
	int esc = 0;
	size_t keyLen = strlen(key);
	for (const char* p = obj; p < end; p++) {
		char c = *p;
		if (inStr) {
			if (esc) esc = 0;
			else if (c == '\\') esc = 1;
			else if (c == '"') inStr = 0;
			continue;
		}
		if (c == '"') {
			if (depth == 1 && (size_t)(end - (p + 1)) > keyLen &&
				memcmp(p + 1, key, keyLen) == 0 && p[1 + keyLen] == '"') {
				const char* v = skip_ws(p + 2 + keyLen, end);
				if (v < end && *v == ':') return skip_ws(v + 1, end);
			}
			inStr = 1;
			continue;
		}
		if (c == '{' || c == '[') depth++;
		else if (c == '}' || c == ']') {
			depth--;
			if (depth <= 0) break;
		}
	}
	return NULL;
}

static int value_int(const char* v, int* out) {
	if (!v || *v == '"') return 0;
	char* end = NULL;
	long n = strtol(v, &end, 10);
	if (end == v) return 0;
	*out = (int)n;
	return 1;
}

static int value_bool(const char* v) {
	if (!v) return -1;
	if (strncmp(v, "true", 4) == 0) return 1;
	if (strncmp(v, "false", 5) == 0) return 0;
	return -1;
}

static void take_string(const char* obj, const char* end, const char* key, char* out, size_t n) {
	out[0] = 0;
	const char* v = value_of(obj, end, key);
	if (v && *v == '"') decode_string(v, end, out, n);
}

static int slug_ok(const char* slug) {
	return strcmp(slug, "gb") == 0 || strcmp(slug, "gbc") == 0 || strcmp(slug, "gba") == 0;
}

static void add_platform(const char* obj, const char* end) {
	if (s_count >= ROMM_MAX) return;
	char slug[12];
	take_string(obj, end, "slug", slug, sizeof(slug));
	if (!slug[0]) take_string(obj, end, "fs_slug", slug, sizeof(slug));
	for (char* c = slug; *c; c++) *c = (char)tolower((unsigned char)*c);
	if (!slug_ok(slug)) return;
	int id = 0;
	if (!value_int(value_of(obj, end, "id"), &id) || id <= 0) return;
	RommRow* row = &s_rows[s_count++];
	memset(row, 0, sizeof(*row));
	row->id = id;
	take_string(obj, end, "name", row->name, sizeof(row->name));
	if (!row->name[0]) snprintf(row->name, sizeof(row->name), "%s", slug);
	snprintf(row->slug, sizeof(row->slug), "%s", slug);
	value_int(value_of(obj, end, "rom_count"), &row->romCount);
}

static void first_file_name(const char* obj, const char* end, char* out, size_t n) {
	out[0] = 0;
	const char* p = obj;
	while (p < end) {
		const char* hit = strstr(p, "\"file_name\"");
		if (!hit || hit >= end) break;
		const char* colon = strchr(hit, ':');
		if (!colon || colon >= end) break;
		const char* v = skip_ws(colon + 1, end);
		if (v < end && *v == '"') {
			decode_string(v, end, out, n);
			if (out[0]) return;
		}
		p = hit + 11;
	}
}

static void add_rom(const char* obj, const char* end) {
	if (s_count >= ROMM_MAX) return;
	if (value_bool(value_of(obj, end, "is_physical")) == 1) return;
	if (value_bool(value_of(obj, end, "has_file_on_disk")) == 0) return;
	int id = 0;
	if (!value_int(value_of(obj, end, "id"), &id) || id <= 0) return;
	RommRow* row = &s_rows[s_count++];
	memset(row, 0, sizeof(*row));
	row->id = id;
	take_string(obj, end, "name", row->name, sizeof(row->name));
	take_string(obj, end, "file_name", row->file, sizeof(row->file));
	if (!row->file[0]) take_string(obj, end, "fs_name", row->file, sizeof(row->file));
	if (!row->file[0]) first_file_name(obj, end, row->file, sizeof(row->file));
	if (!row->name[0]) snprintf(row->name, sizeof(row->name), "%s", row->file[0] ? row->file : "game");
	const char* sz = value_of(obj, end, "file_size_bytes");
	if (sz && *sz != '"') row->bytes = (unsigned)strtoul(sz, NULL, 10);
	snprintf(row->slug, sizeof(row->slug), "%s", s_slug);
}

static void walk_array(const char* openBracket, const char* end, void (*fn)(const char*, const char*)) {
	if (!openBracket || *openBracket != '[') return;
	int depth = 1;
	int inStr = 0;
	int esc = 0;
	const char* obj = NULL;
	for (const char* p = openBracket + 1; p < end; p++) {
		char c = *p;
		if (inStr) {
			if (esc) esc = 0;
			else if (c == '\\') esc = 1;
			else if (c == '"') inStr = 0;
			continue;
		}
		if (c == '"') { inStr = 1; continue; }
		if (c == '{' || c == '[') {
			if (c == '{' && depth == 1) obj = p;
			depth++;
		} else if (c == '}' || c == ']') {
			depth--;
			if (c == '}' && depth == 1 && obj) {
				fn(obj, p + 1);
				obj = NULL;
				if (s_count >= ROMM_MAX) return;
			}
			if (depth == 0) break;
		}
	}
}

static void parse_rows(const char* json, size_t len, int items, void (*fn)(const char*, const char*)) {
	if (!json || !len) return;
	const char* end = json + len;
	const char* p = json;
	while (p < end && *p != '{' && *p != '[') p++;
	if (p >= end) return;
	if (*p == '{') {
		int total = 0;
		if (value_int(value_of(p, end, "total"), &total)) s_total = total;
		if (items) {
			const char* key = strstr(p, "\"items\"");
			if (!key || key >= end) return;
			const char* br = strchr(key, '[');
			if (br && br < end) walk_array(br, end, fn);
			return;
		}
	}
	const char* br = (*p == '[') ? p : strchr(p, '[');
	if (br && br < end) walk_array(br, end, fn);
}

static void set_http_message(int status, const char* body, size_t len) {
	s_message[0] = 0;
	if (body && len) {
		const char* d = strstr(body, "\"detail\"");
		if (d && d < body + len) {
			const char* colon = strchr(d, ':');
			if (colon && colon < body + len) {
				const char* v = skip_ws(colon + 1, body + len);
				if (v < body + len && *v == '"')
					decode_string(v, body + len, s_message, sizeof(s_message));
			}
		}
	}
	if (!s_message[0]) snprintf(s_message, sizeof(s_message), "RomM HTTP %d", status);
}

static void auth_value(char* out, size_t n) {
	snprintf(out, n, "Bearer %s", g_cfg.romm_token);
}

static void RC_CCONV on_platforms(const rc_api_server_response_t* resp, void* ud);
static void RC_CCONV on_roms(const rc_api_server_response_t* resp, void* ud);
static void RC_CCONV on_download(const rc_api_server_response_t* resp, void* ud);
static void RC_CCONV on_token(const rc_api_server_response_t* resp, void* ud);
static void refresh_token(void);

static void fetch_platforms(void) {
	if (!g_cfg.romm_token[0] || !g_cfg.romm_url[0]) {
		s_phase = ROMM_NEED_LOGIN;
		s_busy = 0;
		snprintf(s_message, sizeof(s_message), "Sign in to RomM first");
		return;
	}
	s_busy = 1;
	s_phase = ROMM_BUSY;
	s_want = 1;
	s_message[0] = 0;
	char url[256];
	char auth[900];
	snprintf(url, sizeof(url), "%s/api/platforms", g_cfg.romm_url);
	auth_value(auth, sizeof(auth));
	http_request(url, NULL, NULL, auth, NULL, on_platforms, NULL);
}

static void fetch_roms(void) {
	s_busy = 1;
	s_phase = ROMM_BUSY;
	s_want = 2;
	s_message[0] = 0;
	char url[320];
	char auth[900];
	snprintf(url, sizeof(url), "%s/api/roms?platform_id=%d&limit=%d&offset=%d",
		g_cfg.romm_url, s_platformId, ROMM_PAGE, s_offset);
	auth_value(auth, sizeof(auth));
	http_request(url, NULL, NULL, auth, NULL, on_roms, NULL);
}

static void queue_download(void) {
	if (s_downIndex < 0 || s_downIndex >= s_count) {
		s_busy = 0;
		s_downloading = 0;
		s_phase = ROMM_ROMS;
		return;
	}
	const RommRow* row = &s_rows[s_downIndex];
	char file[120];
	const char* src = row->file[0] ? row->file : row->name;
	size_t j = 0;
	const char* base = src;
	for (const char* p = src; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
	for (size_t i = 0; base[i] && j + 1 < sizeof(file); i++) {
		unsigned char c = (unsigned char)base[i];
		if (c < 32 || strchr(":*?\"<>|", c)) c = '_';
		file[j++] = (char)c;
	}
	file[j] = 0;
	if (!file[0]) strncpy(file, "game.gba", sizeof(file) - 1);
	const char* dir = AGBRA_ROMS_DIR;
	if (strcmp(s_slug, "gb") == 0) dir = AGBRA_GB_ROMS_DIR;
	else if (strcmp(s_slug, "gbc") == 0) dir = AGBRA_GBC_ROMS_DIR;
	fs_mkdir_p(dir);
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (!s_downOverwrite && fs_exists(path)) {
		s_busy = 0;
		s_downloading = 0;
		s_phase = ROMM_ROMS;
		snprintf(s_message, sizeof(s_message), "Already on the SD card. Y replaces it.");
		return;
	}
	char enc[400];
	url_encode(row->file[0] ? row->file : file, enc, sizeof(enc));
	char url[720];
	if (row->file[0] && !s_contentFallback)
		snprintf(url, sizeof(url), "%s/api/roms/%d/content/%s", g_cfg.romm_url, row->id, enc);
	else
		snprintf(url, sizeof(url), "%s/api/roms/%d/content", g_cfg.romm_url, row->id);
	strncpy(s_downName, file, sizeof(s_downName) - 1);
	s_downName[sizeof(s_downName) - 1] = 0;
	s_downloading = 1;
	s_busy = 1;
	s_phase = ROMM_BUSY;
	s_want = 3;
	char auth[900];
	auth_value(auth, sizeof(auth));
	http_request(url, NULL, NULL, auth, path, on_download, NULL);
}

static int unauthorized(int status) {
	if (status != 401) return 0;
	if (g_cfg.romm_refresh[0] && !s_retryAuth) {
		s_retryAuth = 1;
		refresh_token();
		return 1;
	}
	s_busy = 0;
	s_downloading = 0;
	s_phase = ROMM_NEED_LOGIN;
	snprintf(s_message, sizeof(s_message), "RomM sign-in expired. X to sign in again.");
	g_cfg.romm_token[0] = 0;
	config_save();
	return 1;
}

static void RC_CCONV on_platforms(const rc_api_server_response_t* resp, void* ud) {
	(void)ud;
	int status = resp ? resp->http_status_code : 0;
	if (unauthorized(status)) return;
	s_busy = 0;
	s_retryAuth = 0;
	s_count = 0;
	if (status < 200 || status >= 300) {
		s_phase = ROMM_ERROR;
		set_http_message(status, resp ? resp->body : NULL, resp ? resp->body_length : 0);
		return;
	}
	parse_rows(resp->body, resp->body_length, 0, add_platform);
	s_phase = ROMM_PLATFORMS;
	if (s_count == 0)
		snprintf(s_message, sizeof(s_message), "No GB, GBC, or GBA platforms on this server.");
}

static void RC_CCONV on_roms(const rc_api_server_response_t* resp, void* ud) {
	(void)ud;
	int status = resp ? resp->http_status_code : 0;
	if (unauthorized(status)) return;
	s_busy = 0;
	s_retryAuth = 0;
	s_count = 0;
	s_total = 0;
	if (status < 200 || status >= 300) {
		s_phase = ROMM_ERROR;
		set_http_message(status, resp ? resp->body : NULL, resp ? resp->body_length : 0);
		return;
	}
	parse_rows(resp->body, resp->body_length, 1, add_rom);
	s_phase = ROMM_ROMS;
	if (s_count == 0) snprintf(s_message, sizeof(s_message), "No games on this page.");
}

static void RC_CCONV on_download(const rc_api_server_response_t* resp, void* ud) {
	(void)ud;
	int status = resp ? resp->http_status_code : 0;
	if (status == 404 && !s_contentFallback) {
		s_contentFallback = 1;
		queue_download();
		return;
	}
	if (unauthorized(status)) return;
	s_busy = 0;
	s_downloading = 0;
	s_retryAuth = 0;
	s_contentFallback = 0;
	s_phase = ROMM_ROMS;
	if (status >= 200 && status < 300) {
		snprintf(s_message, sizeof(s_message), "Saved %s", s_downName);
		app_set_status("Saved %s", s_downName);
	} else {
		set_http_message(status, resp ? resp->body : NULL, resp ? resp->body_length : 0);
		app_set_status("%s", s_message);
	}
}

static void retry_want(void) {
	if (s_want == 2) fetch_roms();
	else if (s_want == 3) queue_download();
	else fetch_platforms();
}

static void RC_CCONV on_token(const rc_api_server_response_t* resp, void* ud) {
	(void)ud;
	int status = resp ? resp->http_status_code : 0;
	const char* body = resp ? resp->body : NULL;
	size_t len = resp ? resp->body_length : 0;
	if (status < 200 || status >= 300 || !body) {
		s_busy = 0;
		s_downloading = 0;
		s_retryAuth = 0;
		s_phase = ROMM_ERROR;
		set_http_message(status, body, len);
		return;
	}
	const char* end = body + len;
	char access[768];
	char refresh[768];
	access[0] = refresh[0] = 0;
	const char* root = body;
	while (root < end && *root != '{') root++;
	if (root < end) {
		take_string(root, end, "access_token", access, sizeof(access));
		take_string(root, end, "refresh_token", refresh, sizeof(refresh));
	}
	if (!access[0]) {
		s_busy = 0;
		s_phase = ROMM_ERROR;
		snprintf(s_message, sizeof(s_message), "RomM did not return a token");
		return;
	}
	strncpy(g_cfg.romm_token, access, sizeof(g_cfg.romm_token) - 1);
	g_cfg.romm_token[sizeof(g_cfg.romm_token) - 1] = 0;
	if (refresh[0]) {
		strncpy(g_cfg.romm_refresh, refresh, sizeof(g_cfg.romm_refresh) - 1);
		g_cfg.romm_refresh[sizeof(g_cfg.romm_refresh) - 1] = 0;
	}
	config_save();
	retry_want();
}

static void refresh_token(void) {
	size_t n = strlen(g_cfg.romm_refresh);
	char* enc = malloc(n * 3 + 1);
	char* body = malloc(n * 3 + 64);
	if (!enc || !body) {
		free(enc);
		free(body);
		s_busy = 0;
		s_phase = ROMM_ERROR;
		snprintf(s_message, sizeof(s_message), "Out of memory");
		return;
	}
	url_encode(g_cfg.romm_refresh, enc, n * 3 + 1);
	snprintf(body, n * 3 + 64, "grant_type=refresh_token&refresh_token=%s", enc);
	free(enc);
	char url[256];
	snprintf(url, sizeof(url), "%s/api/token", g_cfg.romm_url);
	s_busy = 1;
	s_phase = ROMM_BUSY;
	http_request(url, body, "application/x-www-form-urlencoded", NULL, NULL, on_token, NULL);
	free(body);
}

void romm_login(const char* url, const char* user, const char* secret) {
	if (!url || !url[0] || !secret || !secret[0]) return;
	normalize_base(url, g_cfg.romm_url, sizeof(g_cfg.romm_url));
	if (user) {
		strncpy(g_cfg.romm_user, user, sizeof(g_cfg.romm_user) - 1);
		g_cfg.romm_user[sizeof(g_cfg.romm_user) - 1] = 0;
	}
	s_retryAuth = 0;
	s_want = 1;
	if (strncmp(secret, "rmm_", 4) == 0) {
		strncpy(g_cfg.romm_token, secret, sizeof(g_cfg.romm_token) - 1);
		g_cfg.romm_token[sizeof(g_cfg.romm_token) - 1] = 0;
		g_cfg.romm_refresh[0] = 0;
		config_save();
		fetch_platforms();
		return;
	}
	char userEnc[192];
	char passEnc[768];
	url_encode(g_cfg.romm_user, userEnc, sizeof(userEnc));
	url_encode(secret, passEnc, sizeof(passEnc));
	char body[1024];
	snprintf(body, sizeof(body), "grant_type=password&username=%s&password=%s", userEnc, passEnc);
	char tokenUrl[256];
	snprintf(tokenUrl, sizeof(tokenUrl), "%s/api/token", g_cfg.romm_url);
	s_busy = 1;
	s_phase = ROMM_BUSY;
	s_message[0] = 0;
	config_save();
	http_request(tokenUrl, body, "application/x-www-form-urlencoded", NULL, NULL, on_token, NULL);
}

int romm_ready(void) {
	return g_cfg.romm_url[0] && g_cfg.romm_token[0];
}

RommPhase romm_phase(void) { return s_phase; }
int romm_count(void) { return s_count; }
int romm_offset(void) { return s_offset; }
int romm_total(void) { return s_total; }
int romm_busy(void) { return s_busy; }
const char* romm_message(void) { return s_message; }
const char* romm_platform_name(void) { return s_platformName; }

const RommRow* romm_at(int index) {
	if (index < 0 || index >= s_count) return NULL;
	return &s_rows[index];
}

void romm_load_platforms(void) {
	s_offset = 0;
	fetch_platforms();
}

void romm_open(int index) {
	if (s_busy || index < 0 || index >= s_count) return;
	s_platformId = s_rows[index].id;
	strncpy(s_slug, s_rows[index].slug, sizeof(s_slug) - 1);
	s_slug[sizeof(s_slug) - 1] = 0;
	strncpy(s_platformName, s_rows[index].name, sizeof(s_platformName) - 1);
	s_platformName[sizeof(s_platformName) - 1] = 0;
	s_offset = 0;
	s_contentFallback = 0;
	fetch_roms();
}

void romm_page(int dir) {
	if (s_busy || s_phase != ROMM_ROMS || !dir) return;
	int next = s_offset + dir * ROMM_PAGE;
	if (next < 0) return;
	if (s_total > 0 && next >= s_total) return;
	if (s_total == 0 && dir > 0 && s_count < ROMM_PAGE) return;
	s_offset = next;
	fetch_roms();
}

void romm_download(int index, int overwrite) {
	if (s_busy || index < 0 || index >= s_count) return;
	s_downIndex = index;
	s_downOverwrite = overwrite ? 1 : 0;
	s_contentFallback = 0;
	queue_download();
}

int romm_progress(char* buf, size_t n) {
	if (!s_downloading || !buf || !n) return 0;
	uint32_t got = 0, total = 0;
	int on = 0;
	http_stream_progress(&got, &total, &on);
	if (total)
		snprintf(buf, n, "Saving %s  %u / %u KB", s_downName, got / 1024u, total / 1024u);
	else
		snprintf(buf, n, "Saving %s  %u KB", s_downName, got / 1024u);
	return 1;
}
