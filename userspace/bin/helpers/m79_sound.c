/*
 * M79 Sound Stack Smoke Test — AC'97 /dev/dsp1 + OSS mixer ioctls
 *
 * Tests:
 *  - /dev/dsp1 (AC'97, M79) is openable
 *  - OSS SOUND_MIXER_READ_* / WRITE_* ioctls round-trip through the kernel
 *    mixer layer (devmask, volume get/set, mute)
 *  - write() to /dev/dsp1 accepts PCM and the AC'97 DMA engine plays it
 *
 * Emits M79-SMOKE markers consumed by tests/smoke.sh.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include "../../include/linux/soundcard.h"
#include <unistd.h>

#ifndef VOLUME_PACK
#define VOLUME_LEFT(v)  ((v) & 0xFF)
#define VOLUME_RIGHT(v) (((v) >> 8) & 0xFF)
#define VOLUME_PACK(l, r) ((((r) & 0xFF) << 8) | ((l) & 0xFF))
#endif

static void marker(const char *text) {
	write(1, text, strlen(text));
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	marker("M79-SMOKE: start\n");

	int fd = open("/dev/dsp1", O_WRONLY);
	if (fd < 0) {
		/* No AC'97 device in this QEMU config — not a hard failure. */
		marker("M79-SMOKE: skip no-dsp1\n");
		marker("M79-SMOKE: done\n");
		return 0;
	}
	marker("M79-SMOKE: ok open-dsp1\n");

	/* OSS mixer mask queries. */
	{
		int v;
		if (ioctl(fd, SOUND_MIXER_READ_DEVMASK, &v) == 0 &&
		    (v & (1 << SOUND_MIXER_VOLUME)) &&
		    (v & (1 << SOUND_MIXER_PCM)) &&
		    (v & (1 << SOUND_MIXER_MUTE))) {
			marker("M79-SMOKE: ok devmask\n");
		} else {
			marker("M79-SMOKE: fail devmask\n");
		}
	}

	/* Volume set + read-back round-trip. */
	{
		int v = VOLUME_PACK(63, 63);
		if (ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &v) == 0) {
			int got;
			if (ioctl(fd, SOUND_MIXER_READ_VOLUME, &got) == 0 &&
			    VOLUME_LEFT(got) >= 60 && VOLUME_LEFT(got) <= 66 &&
			    VOLUME_RIGHT(got) >= 60 && VOLUME_RIGHT(got) <= 66) {
				marker("M79-SMOKE: ok mixer-vol\n");
			} else {
				marker("M79-SMOKE: fail mixer-vol\n");
			}
		} else {
			marker("M79-SMOKE: fail mixer-vol\n");
		}
	}

	/* Mute + restore. */
	{
		int m = 1;
		if (ioctl(fd, SOUND_MIXER_WRITE_MUTE, &m) == 0) {
			int got = -1;
			if (ioctl(fd, SOUND_MIXER_READ_MUTE, &got) == 0 && got == 1) {
				marker("M79-SMOKE: ok mixer-mute\n");
			} else {
				marker("M79-SMOKE: fail mixer-mute\n");
			}
		} else {
			marker("M79-SMOKE: fail mixer-mute\n");
		}
		m = 0;
		ioctl(fd, SOUND_MIXER_WRITE_MUTE, &m);
	}

	/* Play a short 440 Hz tone through the AC'97 PCM-out DMA engine. */
	{
		/* 100 ms of 48 kHz stereo 16-bit: 4800 frames, and a frame is two
		 * channels of two bytes. The buffer was half that and the loop below
		 * wrote every one of the 4800 frames into it, which put 9600 bytes
		 * past its end - onto the first page of the heap, which on aarch64 is
		 * not mapped until something asks for it. */
		static char buf[19200];
		int n = 4800;
		for (int i = 0; i < n; i++) {
			int period = 48000 / 440;
			int pos = i % period;
			int half = period / 2;
			int v = (pos < half)
				? 16000 * 2 * pos / period
				: 16000 * 2 * (period - pos) / period;
			v -= 8000;
			if (v > 16000) v = 16000;
			if (v < -16000) v = -16000;
			short s = (short)v;
			buf[i * 4]     = (char)(s & 0xFF);
			buf[i * 4 + 1] = (char)((s >> 8) & 0xFF);
			buf[i * 4 + 2] = (char)(s & 0xFF);
			buf[i * 4 + 3] = (char)((s >> 8) & 0xFF);
		}
		int wr = write(fd, buf, sizeof(buf));
		if (wr == (int)sizeof(buf)) {
			marker("M79-SMOKE: ok pcm-write\n");
		} else {
			marker("M79-SMOKE: fail pcm-write\n");
		}
	}

	close(fd);

	marker("M79-SMOKE: done\n");
	return 0;
}
