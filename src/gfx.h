// Shmees Media Center: the 2D renderer. Everything on screen is a textured, coloured quad drawn with one shader
// (shaders/ui.*): text, icons, rounded panels and shadows come from the coverage atlas; posters and artwork from
// their own RGBA textures. Quads are batched in a ring of GPU memory and drawn in runs that share a texture.
#ifndef SMC_GFX_H
#define SMC_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/texture.h"

enum {
	SCREEN_W = 1280,
	SCREEN_H = 720,
};

typedef struct {
	XsdkTexture texture;
	uint32_t width;
	uint32_t height;
	bool valid;
} GfxImage;

// Opens the GPU, the 720p display, the shader, the quad ring and the atlas. false (logged) on failure.
bool gfx_open(void);
void gfx_close(void);

// A frame: begin waits for a front buffer; end draws what's queued, resolves and presents it.
bool gfx_begin_frame(void);
void gfx_end_frame(void);

// Multiplies every colour drawn from now on by a in [0, 1] (screen fades).
void gfx_set_fade(float a);
// Clip drawing to a rectangle (scissor), or not.
void gfx_clip(float x, float y, float w, float h);
void gfx_unclip(void);

// Colours are 0xAARRGGBB.
void gfx_rect(float x, float y, float w, float h, uint32_t argb);
void gfx_gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void gfx_gradient_h(float x, float y, float w, float h, uint32_t left, uint32_t right);
void gfx_round_rect(float x, float y, float w, float h, float radius, uint32_t argb);
void gfx_round_outline(float x, float y, float w, float h, float radius, uint32_t argb);
void gfx_circle(float cx, float cy, float r, uint32_t argb);
// A soft drop shadow around a rectangle, spread pixels out from it.
void gfx_shadow(float x, float y, float w, float h, float spread, uint32_t argb);
void gfx_icon(int icon, float x, float y, float size, uint32_t argb);

// Text: y is the top of the line box. Returns the advance width drawn.
float gfx_text(int font, float x, float y, uint32_t argb, const char *utf8);
float gfx_text_width(int font, const char *utf8);
float gfx_line_height(int font);
// Draws text cut to max_w with an ellipsis if it doesn't fit.
float gfx_text_fit(int font, float x, float y, float max_w, uint32_t argb, const char *utf8);
// Word-wrapped paragraph, at most max_lines lines (the last ellipsized). Returns the height used.
float gfx_text_wrap(int font, float x, float y, float max_w, int max_lines, uint32_t argb, const char *utf8);

// Images: RGBA8 rows from the CPU, or a file (JPEG/PNG/BMP/TGA via stb_image) scaled down to fit max_w x max_h.
bool gfx_image_from_rgba(GfxImage *image, const uint8_t *rgba, uint32_t width, uint32_t height);
bool gfx_image_from_memory(GfxImage *image, const uint8_t *data, uint32_t size);
// Decodes and scales on the calling thread (no GPU calls): for the loader thread. Free *rgba with free().
uint8_t *gfx_decode_file(const char *path, uint32_t max_w, uint32_t max_h, uint32_t *width, uint32_t *height);
void gfx_image_free(GfxImage *image);
// Lets the image go without waiting: its texture is freed once the GPU has finished every frame queued so far. Don't
// draw it after.
void gfx_image_release(GfxImage *image);
void gfx_image(const GfxImage *image, float x, float y, float w, float h, uint32_t tint);
// Cover-fit: fills the rectangle, cropping the image's overflow evenly.
void gfx_image_cover(const GfxImage *image, float x, float y, float w, float h, uint32_t tint);

const GfxImage *gfx_badge(void);

// A video picture's three planes as 8-bit textures, in a few sets so a new frame never overwrites one the GPU may still
// be drawing.
enum {
	GFX_VIDEO_SETS = 3,
};

typedef struct {
	XsdkTexture planes[GFX_VIDEO_SETS][3];
	uint32_t fences[GFX_VIDEO_SETS];
	uint32_t width;
	uint32_t height;
	uint32_t chroma_width;
	uint32_t chroma_height;
	// The set holding the newest frame, and whether any frame has been uploaded.
	int current;
	bool ready;
	bool valid;
	// Rows of the YUV-to-RGB matrix for the current frame (see video.frag).
	float matrix[12];
} GfxVideo;

// Textures for frames width by height with chroma planes chroma_width by chroma_height.
bool gfx_video_create(GfxVideo *video, uint32_t width, uint32_t height, uint32_t chroma_width, uint32_t chroma_height);
// Copies a frame's planes in (rows linesize bytes apart) into the next set; bt709 and full_range pick the matrix.
void gfx_video_upload(GfxVideo *video, const uint8_t *const planes[3], const int linesize[3], bool bt709,
	bool full_range);
// Draws the newest frame stretched to the rectangle.
void gfx_video_draw(GfxVideo *video, float x, float y, float w, float h, uint32_t tint);
void gfx_video_free(GfxVideo *video);

typedef struct {
	float frame_ms_avg;
	float frame_ms_max;
	uint32_t quads_last_frame;
	uint32_t draws_last_frame;
	uint32_t missed_vblanks;
	uint32_t textures_live;
	uint32_t texture_bytes_live;
} GfxStats;
void gfx_stats(GfxStats *stats);

#endif
