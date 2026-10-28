#include "audio.h"

#include <3ds.h>
#include <string.h>

#define AUDIO_RATE 32728.0f
#define AUDIO_SAMPLES 512
#define AUDIO_BUFFERS 3

static ndspWaveBuf s_bufs[AUDIO_BUFFERS];
static int16_t* s_pcm = NULL;
static int s_index = 0;
static bool s_ok = false;

bool audio_init(void) {
	if (ndspInit() != 0) return false;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, AUDIO_RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	s_pcm = linearAlloc(AUDIO_SAMPLES * 2 * sizeof(int16_t) * AUDIO_BUFFERS);
	if (!s_pcm) {
		ndspExit();
		return false;
	}
	memset(s_pcm, 0, AUDIO_SAMPLES * 2 * sizeof(int16_t) * AUDIO_BUFFERS);
	memset(s_bufs, 0, sizeof(s_bufs));
	for (int i = 0; i < AUDIO_BUFFERS; i++) {
		s_bufs[i].data_pcm16 = s_pcm + i * AUDIO_SAMPLES * 2;
		s_bufs[i].nsamples = AUDIO_SAMPLES;
		s_bufs[i].status = NDSP_WBUF_DONE;
	}
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
	if (s_ok && hz > 1.0f) ndspChnSetRate(0, hz);
}

void audio_clear(void) {
	if (!s_ok) return;
	ndspChnWaveBufClear(0);
	for (int i = 0; i < AUDIO_BUFFERS; i++) s_bufs[i].status = NDSP_WBUF_DONE;
}

void audio_push(const int16_t* samples, size_t frames) {
	if (!s_ok || !samples || !frames) return;
	size_t offset = 0;
	while (offset < frames) {
		ndspWaveBuf* wb = &s_bufs[s_index];
		if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE) break;
		size_t n = frames - offset;
		if (n > AUDIO_SAMPLES) n = AUDIO_SAMPLES;
		int16_t* dst = wb->data_pcm16;
		/* mGBA produces interleaved stereo PCM16 */
		memcpy(dst, samples + offset * 2, n * 2 * sizeof(int16_t));
		if (n < AUDIO_SAMPLES) {
			memset(dst + n * 2, 0, (AUDIO_SAMPLES - n) * 2 * sizeof(int16_t));
		}
		wb->nsamples = (u32)(n < AUDIO_SAMPLES ? n : AUDIO_SAMPLES);
		DSP_FlushDataCache(dst, AUDIO_SAMPLES * 2 * sizeof(int16_t));
		ndspChnWaveBufAdd(0, wb);
		s_index = (s_index + 1) % AUDIO_BUFFERS;
		offset += n;
	}
}
