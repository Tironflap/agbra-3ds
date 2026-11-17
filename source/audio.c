#include "audio.h"

#include <3ds.h>
#include <string.h>

#define AUDIO_RATE 32768.0f
/* The DSP stalls on a short buffer and then never marks it finished, so
 * later audio is refused. Only full blocks are submitted. */
#define AUDIO_CHUNK 512
#define AUDIO_BUFFERS 8
#define RING_FRAMES 8192

static ndspWaveBuf s_bufs[AUDIO_BUFFERS];
static int16_t* s_pcm = NULL;
static int16_t s_ring[RING_FRAMES * 2];
static size_t s_rhead = 0;
static size_t s_rtail = 0;
static size_t s_rcount = 0;
static int s_stall = 0;
static bool s_ok = false;
static float s_base = AUDIO_RATE;
static float s_tempo = 1.0f;

static void apply_rate(void) {
	if (s_ok) ndspChnSetRate(0, s_base * s_tempo);
}

static void ring_drop(size_t frames) {
	if (frames > s_rcount) frames = s_rcount;
	s_rtail = (s_rtail + frames) % RING_FRAMES;
	s_rcount -= frames;
}

static void ring_write(const int16_t* samples, size_t frames) {
	if (s_rcount + frames > RING_FRAMES) ring_drop(s_rcount + frames - RING_FRAMES);
	while (frames) {
		size_t n = RING_FRAMES - s_rhead;
		if (n > frames) n = frames;
		memcpy(s_ring + s_rhead * 2, samples, n * 2 * sizeof(int16_t));
		s_rhead = (s_rhead + n) % RING_FRAMES;
		s_rcount += n;
		samples += n * 2;
		frames -= n;
	}
}

static void ring_read(int16_t* dst) {
	size_t n = RING_FRAMES - s_rtail;
	if (n > AUDIO_CHUNK) n = AUDIO_CHUNK;
	memcpy(dst, s_ring + s_rtail * 2, n * 2 * sizeof(int16_t));
	if (n < AUDIO_CHUNK) {
		memcpy(dst + n * 2, s_ring, (AUDIO_CHUNK - n) * 2 * sizeof(int16_t));
	}
	s_rtail = (s_rtail + AUDIO_CHUNK) % RING_FRAMES;
	s_rcount -= AUDIO_CHUNK;
}

static void audio_kick(void) {
	int queued = 0;
	if (!s_ok) return;
	for (int i = 0; i < AUDIO_BUFFERS; i++) {
		ndspWaveBuf* wb = &s_bufs[i];
		if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) continue;
		if (s_rcount < AUDIO_CHUNK) break;
		ring_read(wb->data_pcm16);
		wb->nsamples = AUDIO_CHUNK;
		DSP_FlushDataCache(wb->data_pcm16, AUDIO_CHUNK * 2 * sizeof(int16_t));
		ndspChnWaveBufAdd(0, wb);
		queued = 1;
	}
	if (queued || s_rcount < AUDIO_CHUNK) {
		s_stall = 0;
		return;
	}
	/* Every block is stuck. Drop the queue and start it again. */
	if (++s_stall < 8) return;
	ndspChnWaveBufClear(0);
	for (int i = 0; i < AUDIO_BUFFERS; i++) s_bufs[i].status = NDSP_WBUF_DONE;
	s_stall = 0;
}

bool audio_init(void) {
	float mix[12];
	if (ndspInit() != 0) return false;
	ndspSetMasterVol(1.0f);
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, AUDIO_RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	s_pcm = linearAlloc(AUDIO_CHUNK * 2 * sizeof(int16_t) * AUDIO_BUFFERS);
	if (!s_pcm) {
		ndspExit();
		return false;
	}
	memset(s_pcm, 0, AUDIO_CHUNK * 2 * sizeof(int16_t) * AUDIO_BUFFERS);
	memset(s_bufs, 0, sizeof(s_bufs));
	for (int i = 0; i < AUDIO_BUFFERS; i++) {
		s_bufs[i].data_pcm16 = s_pcm + i * AUDIO_CHUNK * 2;
		s_bufs[i].nsamples = AUDIO_CHUNK;
		s_bufs[i].status = NDSP_WBUF_DONE;
	}
	s_rhead = 0;
	s_rtail = 0;
	s_rcount = 0;
	s_stall = 0;
	s_ok = true;
	return true;
}

bool audio_ready(void) {
	return s_ok;
}

void audio_exit(void) {
	if (!s_ok) return;
	ndspChnWaveBufClear(0);
	linearFree(s_pcm);
	s_pcm = NULL;
	ndspExit();
	s_ok = false;
}

void audio_set_rate(float hz) {
	if (hz > 1.0f) s_base = hz;
	apply_rate();
}

void audio_set_tempo(float scale) {
	if (scale < 0.45f) scale = 0.45f;
	if (scale > 4.0f) scale = 4.0f;
	if (scale > s_tempo - 0.02f && scale < s_tempo + 0.02f) return;
	s_tempo = scale;
	apply_rate();
}

void audio_clear(void) {
	if (!s_ok) return;
	ndspChnWaveBufClear(0);
	for (int i = 0; i < AUDIO_BUFFERS; i++) s_bufs[i].status = NDSP_WBUF_DONE;
	s_rhead = 0;
	s_rtail = 0;
	s_rcount = 0;
	s_stall = 0;
}

void audio_push(const int16_t* samples, size_t frames) {
	if (!s_ok || !samples || !frames) return;
	ring_write(samples, frames);
	audio_kick();
}
