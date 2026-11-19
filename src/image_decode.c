// Shmees Media Center: image decoding (image_decode.h).

#include "image_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// xsdk's toolchain is built without thread-local storage (--disable-tls), so stb_image's failure reason is a plain
// global. Posters and backdrops only: no HDR, PSD, GIF, PIC or PNM.
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "../third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum {
	// A poster file over this is refused rather than read: 24 MB covers any sane JPEG or PNG.
	MAX_FILE_BYTES = 24 * 1024 * 1024,
	// stb_image decodes the whole image first; one over this many pixels (8K by 4K) is refused.
	MAX_PIXELS = 8192 * 4096,
};

uint8_t *image_decode_memory_gray(const uint8_t *data, uint32_t size, int *width, int *height)
{
	int channels = 0;
	return stbi_load_from_memory(data, (int)size, width, height, &channels, 1);
}

uint8_t *image_decode_memory_rgba(const uint8_t *data, uint32_t size, int *width, int *height)
{
	int channels = 0;
	return stbi_load_from_memory(data, (int)size, width, height, &channels, 4);
}

const char *image_decode_failure(void)
{
	const char *reason = stbi_failure_reason();
	return reason ? reason : "unknown";
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return NULL;
	uint8_t *data = NULL;
	if (fseek(file, 0, SEEK_END) == 0) {
		const long length = ftell(file);
		if (length > 0 && length <= MAX_FILE_BYTES && fseek(file, 0, SEEK_SET) == 0) {
			data = malloc((size_t)length);
			if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
				free(data);
				data = NULL;
			}
			*size = (uint32_t)length;
		}
	}
	fclose(file);
	return data;
}

// Box filter: each output pixel the average of the source pixels it covers, in fixed point.
static uint8_t *scale_down(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	uint8_t *dst = malloc((size_t)dw * dh * 4);
	if (!dst)
		return NULL;
	for (uint32_t y = 0; y < dh; y++) {
		const uint32_t y0 = (uint32_t)((uint64_t)y * sh / dh);
		uint32_t y1 = (uint32_t)((uint64_t)(y + 1) * sh / dh);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (uint32_t x = 0; x < dw; x++) {
			const uint32_t x0 = (uint32_t)((uint64_t)x * sw / dw);
			uint32_t x1 = (uint32_t)((uint64_t)(x + 1) * sw / dw);
			if (x1 <= x0)
				x1 = x0 + 1;
			uint32_t sum[4] = { 0, 0, 0, 0 };
			for (uint32_t sy = y0; sy < y1; sy++) {
				const uint8_t *p = src + ((size_t)sy * sw + x0) * 4;
				for (uint32_t sx = x0; sx < x1; sx++, p += 4) {
					sum[0] += p[0];
					sum[1] += p[1];
					sum[2] += p[2];
					sum[3] += p[3];
				}
			}
			const uint32_t n = (y1 - y0) * (x1 - x0);
			uint8_t *d = dst + ((size_t)y * dw + x) * 4;
			for (int c = 0; c < 4; c++)
				d[c] = (uint8_t)((sum[c] + n / 2) / n);
		}
	}
	return dst;
}

uint8_t *image_decode_file_fit(const char *path, uint32_t max_w, uint32_t max_h, uint32_t *width, uint32_t *height)
{
	uint32_t size = 0;
	uint8_t *data = read_file(path, &size);
	if (!data)
		return NULL;
	int w = 0;
	int h = 0;
	int channels = 0;
	if (!stbi_info_from_memory(data, (int)size, &w, &h, &channels) || w <= 0 || h <= 0 ||
		(uint64_t)w * (uint64_t)h > MAX_PIXELS) {
		free(data);
		return NULL;
	}
	uint8_t *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
	free(data);
	if (!rgba)
		return NULL;
	uint32_t dw = (uint32_t)w;
	uint32_t dh = (uint32_t)h;
	if (dw > max_w || dh > max_h) {
		// Fit inside, keeping the aspect: whichever side is further over decides.
		if ((uint64_t)dw * max_h > (uint64_t)dh * max_w) {
			dh = (uint32_t)((uint64_t)dh * max_w / dw);
			dw = max_w;
		} else {
			dw = (uint32_t)((uint64_t)dw * max_h / dh);
			dh = max_h;
		}
		if (!dw)
			dw = 1;
		if (!dh)
			dh = 1;
		uint8_t *scaled = scale_down(rgba, (uint32_t)w, (uint32_t)h, dw, dh);
		stbi_image_free(rgba);
		if (!scaled)
			return NULL;
		rgba = scaled;
	}
	*width = dw;
	*height = dh;
	return rgba;
}
