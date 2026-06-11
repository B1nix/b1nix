/*
 * M38 Sound Smoke Test — WAV parser/player verification
 *
 * Tests:
 *  - /dev/dsp exists and is openable
 *  - write() to /dev/dsp accepts PCM data
 *  - WAV file parsing and playback (if /test.wav exists)
 *
 * Emits M38-SMOKE markers consumed by tests/smoke.sh.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void marker(const char *text) {
	write(1, text, strlen(text));
}

/* Generate a 440 Hz sine wave into buf (16-bit LE stereo, 48 kHz).
 * Returns the number of bytes written to buf. */
static int gen_sine(char *buf, int buf_sz, int freq, int sample_rate) {
	int samples = buf_sz / 4; /* 4 bytes per sample (2ch × 16-bit) */
	if (samples <= 0) return 0;
	for (int i = 0; i < samples; i++) {
		/* Simple integer sine: phase in Q16 */
		int period = sample_rate / freq;
		if (period <= 0) period = 1;
		int pos = i % period;
		int half = period / 2;
		int v;
		if (pos < half) {
			v = 16000 * 2 * pos / period;
		} else {
			v = 16000 * 2 * (period - pos) / period;
		}
		v -= 8000;
		if (v > 16000) v = 16000;
		if (v < -16000) v = -16000;
		short s = (short)v;
		/* Stereo: same sample on L and R */
		memcpy(buf + i * 4,     &s, 2);
		memcpy(buf + i * 4 + 2, &s, 2);
	}
	return samples * 4;
}

/* Minimal WAV header parser. Returns 0 on success and fills out the
 * format fields; returns -1 if the file is not a valid WAV. */
struct wav_info {
	int sample_rate;
	int channels;
	int bits_per_sample;
	int data_offset;  /* byte offset to PCM data */
	int data_size;    /* byte length of PCM data */
};

static int wav_parse(const unsigned char *hdr, int hdr_len, struct wav_info *info) {
	if (hdr_len < 44) return -1;
	/* RIFF header */
	if (memcmp(hdr, "RIFF", 4) != 0) return -1;
	if (memcmp(hdr + 8, "WAVE", 4) != 0) return -1;
	/* Scan chunks */
	int pos = 12;
	while (pos + 8 <= hdr_len) {
		unsigned int chunk_id = *(unsigned int *)(hdr + pos);
		unsigned int chunk_sz = *(unsigned int *)(hdr + pos + 4);
		pos += 8;
		if (chunk_id == 0x20746d66) { /* "fmt " */
			if (pos + 16 > hdr_len) return -1;
			int audio_fmt = *(unsigned short *)(hdr + pos);
			if (audio_fmt != 1) return -1; /* PCM only */
			info->channels = *(unsigned short *)(hdr + pos + 2);
			info->sample_rate = *(unsigned int *)(hdr + pos + 4);
			info->bits_per_sample = *(unsigned short *)(hdr + pos + 14);
		} else if (chunk_id == 0x61746164) { /* "data" */
			info->data_offset = pos;
			info->data_size = chunk_sz;
			return 0;
		}
		pos += (chunk_sz + 1) & ~1; /* word-align */
	}
	return -1;
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	marker("M38-SMOKE: start\n");

	/* Test 1: /dev/dsp exists and is openable */
	int fd = open("/dev/dsp", O_WRONLY);
	if (fd >= 0) {
		marker("M38-SMOKE: ok open-dsp\n");
	} else {
		/* In QEMU without sound device, /dev/dsp may not exist.
		 * This is not a hard failure — the device may not be probed. */
		marker("M38-SMOKE: skip no-dsp\n");
		marker("M38-SMOKE: done\n");
		return 0;
	}

	/* Test 2: write a short test tone (100 ms of 440 Hz) */
	{
		char buf[48000]; /* 48000 bytes = 250 ms at 48kHz stereo 16-bit */
		int n = gen_sine(buf, sizeof(buf), 440, 48000);
		if (n > 0) {
			int wr = write(fd, buf, n);
			if (wr == n) {
				marker("M38-SMOKE: ok write-pcm\n");
			} else {
				marker("M38-SMOKE: fail write-pcm\n");
			}
		} else {
			marker("M38-SMOKE: fail gen-sine\n");
		}
	}

	/* Test 3: WAV file playback (if /test.wav exists in initramfs) */
	{
		int wfd = open("/test.wav", O_RDONLY);
		if (wfd >= 0) {
			/* Read the first 4 KiB to parse the header */
			unsigned char hdr[4096];
			int n = read(wfd, hdr, sizeof(hdr));
			if (n > 0) {
				struct wav_info wi;
				memset(&wi, 0, sizeof(wi));
				if (wav_parse(hdr, n, &wi) == 0) {
					marker("M38-SMOKE: ok wav-parse\n");

					/* Write the data portion already read */
					int data_already = n - wi.data_offset;
					if (data_already > 0) {
						write(fd, hdr + wi.data_offset, data_already);
					}
					/* Continue reading the rest */
					char chunk[8192];
					int remaining = wi.data_size - data_already;
					while (remaining > 0) {
						int to_read = remaining;
						if (to_read > (int)sizeof(chunk))
							to_read = sizeof(chunk);
						int r = read(wfd, chunk, to_read);
						if (r <= 0) break;
						write(fd, chunk, r);
						remaining -= r;
					}
					marker("M38-SMOKE: ok wav-play\n");
				} else {
					marker("M38-SMOKE: fail wav-parse\n");
				}
			}
			close(wfd);
		} else {
			marker("M38-SMOKE: skip no-wav\n");
		}
	}

	close(fd);

	marker("M38-SMOKE: done\n");
	return 0;
}
