// Shmees Media Center: JPEG, PNG, BMP and TGA decoding on the CPU through stb_image (public domain). Safe from any
// thread: none of it touches the GPU. Every result is malloc'd; free() it.
#ifndef SMC_IMAGE_DECODE_H
#define SMC_IMAGE_DECODE_H

#include <stdint.h>

// One byte a pixel.
uint8_t *image_decode_memory_gray(const uint8_t *data, uint32_t size, int *width, int *height);
// Four bytes a pixel, R G B A.
uint8_t *image_decode_memory_rgba(const uint8_t *data, uint32_t size, int *width, int *height);
// A file decoded to RGBA and scaled down, keeping its shape, to fit max_w by max_h; never scaled up.
uint8_t *image_decode_file_fit(const char *path, uint32_t max_w, uint32_t max_h, uint32_t *width, uint32_t *height);
// Why the last decode on any thread failed (stb_image keeps one reason for all threads here).
const char *image_decode_failure(void);

#endif
