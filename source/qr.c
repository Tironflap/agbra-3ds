#include "qr.h"
#include "app.h"
#include "video.h"

#include "quirc.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAM_W 400
#define CAM_H 240

static void swap_rb(uint16_t* px, int n) {
	int i;
	for (i = 0; i < n; i++) {
		uint16_t c = px[i];
		px[i] = (uint16_t)((c & 0x07E0) | ((c & 0xF800) >> 11) | ((c & 0x001F) << 11));
	}
}

static void to_gray(const uint16_t* px, uint8_t* gray, int n) {
	int i;
	for (i = 0; i < n; i++) {
		uint16_t c = px[i];
		int r = ((c >> 11) & 31) << 3;
		int g = ((c >> 5) & 63) << 2;
		int b = (c & 31) << 3;
		gray[i] = (uint8_t)((r * 30 + g * 59 + b * 11) / 100);
	}
}

static int decode_frame(struct quirc* q, char* text, size_t textLen) {
	int n;
	int i;
	struct quirc_code code;
	struct quirc_data data;
	n = quirc_count(q);
	for (i = 0; i < n; i++) {
		quirc_extract(q, i, &code);
		if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
			quirc_flip(&code);
			if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
		}
		if (data.payload_len <= 0) continue;
		if ((size_t)data.payload_len >= textLen) data.payload_len = (int)textLen - 1;
		memcpy(text, data.payload, (size_t)data.payload_len);
		text[data.payload_len] = 0;
		return 1;
	}
	return 0;
}

int qr_scan(char* text, size_t textLen) {
	uint16_t* cam = NULL;
	struct quirc* q = NULL;
	u32 transfer = 0;
	int pixels = CAM_W * CAM_H;
	int found = 0;
	int alive = 1;
	Result rc;

	if (!text || textLen < 8) return -1;
	text[0] = 0;

	rc = camInit();
	if (R_FAILED(rc)) {
		app_set_status("Camera failed (%08lx)", (unsigned long)rc);
		return -1;
	}
	CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);
	CAMU_SetNoiseFilter(SELECT_OUT1, true);
	CAMU_SetAutoExposure(SELECT_OUT1, true);
	CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	if (R_FAILED(CAMU_GetMaxBytes(&transfer, CAM_W, CAM_H)) || transfer == 0)
		transfer = (u32)(CAM_W * 2);

	cam = memalign(0x1000, (size_t)pixels * 2);
	q = quirc_new();
	if (!cam || !q || quirc_resize(q, CAM_W, CAM_H) != 0) {
		app_set_status("Not enough memory to scan");
		found = -1;
		goto out;
	}
	memset(cam, 0, (size_t)pixels * 2);

	CAMU_SetTransferBytes(PORT_CAM1, transfer, CAM_W, CAM_H);
	CAMU_Activate(SELECT_OUT1);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);

	video_console_target();
	consoleClear();
	printf("\x1b[33mRomM\x1b[0m\n\n");
	printf("Point an outer camera at the\n");
	printf("pairing QR on the RomM page.\n\n");
	printf("Create a token, then Pair.\n\n");
	printf("B cancels\n");
	video_present_bottom();

	while ((alive = aptMainLoop()) != 0) {
		Handle ev = 0;
		int w = 0;
		int h = 0;
		uint8_t* gray;
		hidScanInput();
		if (hidKeysDown() & KEY_B) break;
		if (R_FAILED(CAMU_SetReceiving(&ev, cam, PORT_CAM1, (u32)pixels * 2, (s16)transfer)))
			break;
		svcWaitSynchronization(ev, 300000000LL);
		svcCloseHandle(ev);
		swap_rb(cam, pixels);
		gray = quirc_begin(q, &w, &h);
		if (gray && w == CAM_W && h == CAM_H) to_gray(cam, gray, pixels);
		quirc_end(q);
		video_show_camera(cam, CAM_W, CAM_H);
		if (decode_frame(q, text, textLen)) {
			found = 1;
			break;
		}
		gspWaitForVBlank();
	}
	if (!alive) g_app.running = false;

	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);

out:
	if (q) quirc_destroy(q);
	free(cam);
	camExit();
	return found;
}
