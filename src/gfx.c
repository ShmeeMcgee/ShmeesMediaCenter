// Shmees Media Center: the 2D renderer (gfx.h). Follows xsdk's own text renderer (xsdk:src/text.c): a ring of GPU
// memory in blocks, each block's quads drawn with one repeated index list, a fence per block before it's reused.

#include "gfx.h"

#include <altivec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ShmeesMediaCenter_shaders.h"
#include "gen/smc_assets.h"
#include "image_decode.h"
#include "log.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"

enum {
	// x, y; r, g, b, a; u, v: ui.vert's locations 0, 1 and 2.
	VERTEX_FLOATS = 8,
	VERTEX_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = 4 * VERTEX_BYTES,
	INDICES_PER_QUAD = 6,
	RING_BLOCKS = 4,
	BLOCK_QUADS = 4096,
	BLOCK_BYTES = BLOCK_QUADS * QUAD_BYTES,
	RING_BYTES = RING_BLOCKS * BLOCK_BYTES,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md).
	BEGIN_TIMEOUT_MS = 5000,
	WAIT_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF101418,
	RELEASES = 128,
};

typedef __vector float Vec4;

static const uint32_t quad_indices[INDICES_PER_QUAD] = { 0, 1, 2, 2, 1, 3 };

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 8 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
};

static struct {
	bool open;
	bool gpu_open;
	bool present_open;
	uint32_t width;
	uint32_t height;
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkShader video_pixel;
	bool video_shader;
	volatile uint32_t *ring;
	uint32_t ring_address;
	XsdkRepeatedIndices indices;
	uint32_t block;
	uint32_t block_quads;
	uint32_t run_first;
	const XsdkTexture *run_texture;
	uint32_t fences[RING_BLOCKS];
	XsdkBlendState blend;
	XsdkDepthStencilState depth_stencil;
	XsdkRasterState raster;
	XsdkSamplerState linear;
	GfxImage atlas;
	GfxImage badge;
	uint32_t frame_address;
	bool in_frame;
	float fade;
	float inv_atlas_w;
	float inv_atlas_h;
	uint16_t ascii[FONT_COUNT][128];
	// stats
	uint32_t quads;
	uint32_t draws;
	uint32_t last_quads;
	uint32_t last_draws;
	double frame_start;
	float frame_ms_avg;
	float frame_ms_max;
	uint32_t textures_live;
	uint32_t texture_bytes_live;
	// Textures let go of while the GPU may still read them, freed once their fence has passed.
	struct {
		XsdkTexture texture;
		uint32_t fence;
	} releases[RELEASES];
	uint32_t release_count;
} g;

static void free_released(bool all);

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------------------------------------------
// batching

static void flush_run(void)
{
	const uint32_t count = g.block_quads - g.run_first;
	if (!count || !g.run_texture) {
		g.run_first = g.block_quads;
		return;
	}
	const uint32_t address = g.ring_address + (g.block * BLOCK_QUADS + g.run_first) * QUAD_BYTES;
	xsdk_gpu_invalidate(address, count * QUAD_BYTES);
	xsdk_gpu_set_texture(UI_FRAG_SAMPLER_0_0, g.run_texture, &g.linear);
	xsdk_gpu_set_vertices(0, address, count * QUAD_BYTES);
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, g.indices.address, count * INDICES_PER_QUAD, g.indices.indices_32_bit);
	g.run_first = g.block_quads;
	g.draws++;
}

// Moves to the next block once the GPU has passed it (xsdk:src/text.c, next_block).
static void next_block(void)
{
	g.fences[g.block] = xsdk_gpu_fence();
	g.block = (g.block + 1) % RING_BLOCKS;
	g.block_quads = 0;
	g.run_first = 0;
	const NTSTATUS status = xsdk_gpu_wait(g.fences[g.block], WAIT_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		smc_log("gfx: waiting for ring block %u failed: 0x%08X", (unsigned)g.block, (unsigned)status);
}

static inline void unpack(uint32_t argb, float out[4], float fade)
{
	out[0] = (float)(argb >> 16 & 0xFF) * (1.0f / 255.0f);
	out[1] = (float)(argb >> 8 & 0xFF) * (1.0f / 255.0f);
	out[2] = (float)(argb & 0xFF) * (1.0f / 255.0f);
	out[3] = (float)(argb >> 24) * (1.0f / 255.0f) * fade;
}

// One quad: corners top left, top right, bottom left, bottom right, each its own colour.
static void quad4(const XsdkTexture *texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
	float v1, uint32_t c_tl, uint32_t c_tr, uint32_t c_bl, uint32_t c_br)
{
	if (!g.in_frame || x1 <= x0 || y1 <= y0)
		return;
	if (texture != g.run_texture) {
		flush_run();
		g.run_texture = texture;
	}
	if (g.block_quads == BLOCK_QUADS) {
		flush_run();
		next_block();
	}
	// Pixel centres sit on whole coordinates (xsdk:src/text.c), so edges go half a pixel up and left.
	x0 -= 0.5f;
	y0 -= 0.5f;
	x1 -= 0.5f;
	y1 -= 0.5f;
	float c[4][4];
	unpack(c_tl, c[0], g.fade);
	unpack(c_tr, c[1], g.fade);
	unpack(c_bl, c[2], g.fade);
	unpack(c_br, c[3], g.fade);
	const float px[4] = { x0, x1, x0, x1 };
	const float py[4] = { y0, y0, y1, y1 };
	const float pu[4] = { u0, u1, u0, u1 };
	const float pv[4] = { v0, v0, v1, v1 };
	float *const slot = (float *)(uintptr_t)&g.ring[(g.block * BLOCK_QUADS + g.block_quads) * (QUAD_BYTES / 4)];
	// 16-byte VMX stores, the one wider store write-combined memory takes (xsdk:CLAUDE.md).
	for (int i = 0; i < 4; i++) {
		vec_st((Vec4){ px[i], py[i], c[i][0], c[i][1] }, i * VERTEX_BYTES, slot);
		vec_st((Vec4){ c[i][2], c[i][3], pu[i], pv[i] }, i * VERTEX_BYTES + 16, slot);
	}
	g.block_quads++;
	g.quads++;
}

static void quad(const XsdkTexture *texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
	float v1, uint32_t argb)
{
	quad4(texture, x0, y0, x1, y1, u0, v0, u1, v1, argb, argb, argb, argb);
}

// An atlas rectangle in texels to the screen.
static void atlas_quad(float x, float y, float w, float h, float ax, float ay, float aw, float ah, uint32_t argb)
{
	quad(&g.atlas.texture, x, y, x + w, y + h, ax * g.inv_atlas_w, ay * g.inv_atlas_h, (ax + aw) * g.inv_atlas_w,
		(ay + ah) * g.inv_atlas_h, argb);
}

// ---------------------------------------------------------------------------------------------------------------
// open and close

static bool load_atlas(void)
{
	int w = 0;
	int h = 0;
	uint8_t *texels = image_decode_memory_gray(smc_atlas_png, sizeof smc_atlas_png, &w, &h);
	if (!texels || w != SMC_ATLAS_W || h != SMC_ATLAS_H) {
		smc_log("gfx: decoding the atlas failed (%dx%d)", w, h);
		free(texels);
		return false;
	}
	NTSTATUS status = xsdk_texture_create(&g.atlas.texture, XSDK_FORMAT_R8, (uint32_t)w, (uint32_t)h, 1, true);
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: creating the atlas texture failed: 0x%08X", (unsigned)status);
		free(texels);
		return false;
	}
	xsdk_texture_upload(&g.atlas.texture, 0, texels, (uint32_t)w);
	free(texels);
	// Coverage as alpha, white as colour: one shader draws text, icons, shapes and fills.
	const uint32_t channels[4] = { XSDK_CHANNEL_ONE, XSDK_CHANNEL_ONE, XSDK_CHANNEL_ONE, XSDK_CHANNEL_RED };
	xsdk_texture_swizzle(&g.atlas.texture, channels);
	xsdk_gpu_invalidate(g.atlas.texture.address, g.atlas.texture.size_bytes);
	g.atlas.width = (uint32_t)w;
	g.atlas.height = (uint32_t)h;
	g.atlas.valid = true;
	g.inv_atlas_w = 1.0f / (float)w;
	g.inv_atlas_h = 1.0f / (float)h;
	return true;
}

static uint16_t find_glyph(int font, uint32_t cp)
{
	const SmcFontStyle *const style = &smc_fonts[font];
	uint32_t lo = 0;
	uint32_t hi = style->count;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) / 2;
		if (style->glyphs[mid].code_point < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < style->count && style->glyphs[lo].code_point == cp)
		return (uint16_t)lo;
	return 0xFFFF;
}

bool gfx_open(void)
{
	memset(&g, 0, sizeof g);
	g.fade = 1.0f;
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return false;
	}
	g.gpu_open = true;
	status = xsdk_shader_load(&g.vertex, ui_vert.container, ui_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&g.pixel, ui_frag.container, ui_frag.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(
			&g.vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ VERTEX_BYTES });
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: loading the UI shaders failed: 0x%08X", (unsigned)status);
		return false;
	}
	// The video shader is only needed for playback: the UI runs without it.
	status = xsdk_shader_load(&g.video_pixel, video_frag.container, video_frag.container_bytes);
	g.video_shader = NT_SUCCESS(status);
	if (!g.video_shader)
		smc_log("gfx: loading the video shader failed: 0x%08X", (unsigned)status);
	const uint32_t index_bytes = xsdk_gpu_repeated_index_bytes(quad_indices, INDICES_PER_QUAD, BLOCK_QUADS);
	g.ring = xsdk_gpu_alloc(RING_BYTES + index_bytes);
	if (!g.ring) {
		smc_log("gfx: xsdk_gpu_alloc(%u) failed", (unsigned)(RING_BYTES + index_bytes));
		return false;
	}
	g.ring_address = xsdk_gpu_address(g.ring);
	xsdk_gpu_repeat_indices(&g.indices, g.ring + RING_BYTES / 4, quad_indices, INDICES_PER_QUAD, BLOCK_QUADS);
	if (g.indices.copies != BLOCK_QUADS || g.indices.indices_32_bit)
		smc_log("gfx: repeated indices: %u copies, 32-bit %d", (unsigned)g.indices.copies, g.indices.indices_32_bit);
	xsdk_gpu_invalidate(g.indices.address, index_bytes);

	const XsdkBlend blend = { .color_source = XSDK_BLEND_SRC_ALPHA,
		.color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&g.blend, &blend);
	xsdk_gpu_depth_stencil_state(&g.depth_stencil, &(const XsdkDepthStencil){ .depth_test = false });
	xsdk_gpu_raster_state(&g.raster, &(const XsdkRaster){ .cull = XSDK_CULL_NONE });
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.address_w = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&g.linear, &sampler);

	if (!load_atlas())
		return false;
	int bw = 0;
	int bh = 0;
	uint8_t *badge = image_decode_memory_rgba(smc_badge_png, sizeof smc_badge_png, &bw, &bh);
	if (badge) {
		gfx_image_from_rgba(&g.badge, badge, (uint32_t)bw, (uint32_t)bh);
		free(badge);
	}
	for (int f = 0; f < FONT_COUNT; f++)
		for (uint32_t cp = 0; cp < 128; cp++)
			g.ascii[f][cp] = find_glyph(f, cp);

	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &g.width, &g.height);
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return false;
	}
	g.present_open = true;
	smc_log("gfx: display %ux%u, %u buffers", (unsigned)g.width, (unsigned)g.height, (unsigned)BUFFERS);
	g.open = true;
	return true;
}

void gfx_close(void)
{
	if (g.gpu_open) {
		const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), WAIT_TIMEOUT_MS);
		if (!NT_SUCCESS(status))
			smc_log("gfx: the GPU didn't finish before close: 0x%08X", (unsigned)status);
	}
	free_released(true);
	if (g.present_open)
		xsdk_present_close();
	if (g.atlas.valid)
		xsdk_texture_free(&g.atlas.texture);
	if (g.badge.valid)
		xsdk_texture_free(&g.badge.texture);
	if (g.ring)
		xsdk_gpu_free(g.ring);
	if (g.video_shader)
		xsdk_shader_free(&g.video_pixel);
	xsdk_shader_free(&g.pixel);
	xsdk_shader_free(&g.vertex);
	if (g.gpu_open) {
		const NTSTATUS status = xsdk_gpu_close();
		if (!NT_SUCCESS(status))
			smc_log("gfx: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	}
	memset(&g, 0, sizeof g);
}

// ---------------------------------------------------------------------------------------------------------------
// frames

static void free_released(bool all)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < g.release_count; i++) {
		if (all || xsdk_gpu_fence_done(g.releases[i].fence)) {
			g.texture_bytes_live -= g.releases[i].texture.size_bytes;
			g.textures_live--;
			xsdk_texture_free(&g.releases[i].texture);
		} else {
			g.releases[kept++] = g.releases[i];
		}
	}
	g.release_count = kept;
}

static void set_state(void)
{
	xsdk_gpu_set_target(g.width, g.height);
	xsdk_gpu_set_shaders(&g.vertex, &g.pixel);
	xsdk_built_shader_set_literals(&ui_vert);
	xsdk_built_shader_set_literals(&ui_frag);
	const float to_clip[4] = { 2.0f / (float)g.width, -2.0f / (float)g.height, -1.0f, 1.0f };
	xsdk_gpu_set_vertex_constants(UI_VERT_PUSH, to_clip, UI_VERT_PUSH_REGISTERS);
	xsdk_gpu_set_blend(&g.blend);
	xsdk_gpu_set_depth_stencil(&g.depth_stencil);
	xsdk_gpu_set_raster(&g.raster);
	g.run_texture = NULL;
}

bool gfx_begin_frame(void)
{
	if (!g.open)
		return false;
	uint32_t address = 0;
	const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: xsdk_present_begin failed: 0x%08X", (unsigned)status);
		return false;
	}
	g.frame_start = now_ms();
	g.frame_address = address;
	g.in_frame = true;
	g.quads = 0;
	g.draws = 0;
	g.fade = 1.0f;
	set_state();
	return true;
}

void gfx_end_frame(void)
{
	if (!g.in_frame)
		return;
	flush_run();
	g.run_texture = NULL;
	// Copies the frame into the front buffer, then clears eDRAM for the next one.
	xsdk_gpu_resolve(g.frame_address, g.width, g.height, CLEAR_ARGB);
	xsdk_present_end();
	g.in_frame = false;
	// The next frame starts in a fresh block, so this frame's quads stay put while the GPU reads them.
	next_block();
	free_released(false);
	const float ms = (float)(now_ms() - g.frame_start);
	g.frame_ms_avg = g.frame_ms_avg * 0.95f + ms * 0.05f;
	if (ms > g.frame_ms_max)
		g.frame_ms_max = ms;
	g.last_quads = g.quads;
	g.last_draws = g.draws;
}

void gfx_set_fade(float a)
{
	g.fade = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

void gfx_clip(float x, float y, float w, float h)
{
	flush_run();
	int32_t x0 = (int32_t)floorf(x);
	int32_t y0 = (int32_t)floorf(y);
	int32_t x1 = (int32_t)ceilf(x + w);
	int32_t y1 = (int32_t)ceilf(y + h);
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)g.width)
		x1 = (int32_t)g.width;
	if (y1 > (int32_t)g.height)
		y1 = (int32_t)g.height;
	if (x1 <= x0 || y1 <= y0) {
		x1 = x0 + 1;
		y1 = y0 + 1;
	}
	xsdk_gpu_set_scissor((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

void gfx_unclip(void)
{
	flush_run();
	xsdk_gpu_set_scissor(0, 0, g.width, g.height);
}

// ---------------------------------------------------------------------------------------------------------------
// shapes

void gfx_rect(float x, float y, float w, float h, uint32_t argb)
{
	atlas_quad(x, y, w, h, SMC_SOLID_X, SMC_SOLID_Y, 0.0f, 0.0f, argb);
}

void gfx_gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
	const float u = SMC_SOLID_X * g.inv_atlas_w;
	const float v = SMC_SOLID_Y * g.inv_atlas_h;
	quad4(&g.atlas.texture, x, y, x + w, y + h, u, v, u, v, top, top, bottom, bottom);
}

void gfx_gradient_h(float x, float y, float w, float h, uint32_t left, uint32_t right)
{
	const float u = SMC_SOLID_X * g.inv_atlas_w;
	const float v = SMC_SOLID_Y * g.inv_atlas_h;
	quad4(&g.atlas.texture, x, y, x + w, y + h, u, v, u, v, left, right, left, right);
}

// Nine pieces of a tile: corners edge texels square drawn e pixels square, edges stretched along, centre stretched.
static void nine_slice(float tx, float ty, float tw, float th, float edge, float x, float y, float w, float h, float e,
	uint32_t argb)
{
	if (e * 2.0f > w)
		e = w * 0.5f;
	if (e * 2.0f > h)
		e = h * 0.5f;
	const float xs[4] = { x, x + e, x + w - e, x + w };
	const float ys[4] = { y, y + e, y + h - e, y + h };
	const float us[4] = { tx, tx + edge, tx + tw - edge, tx + tw };
	const float vs[4] = { ty, ty + edge, ty + th - edge, ty + th };
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			atlas_quad(xs[c], ys[r], xs[c + 1] - xs[c], ys[r + 1] - ys[r], us[c], vs[r], us[c + 1] - us[c],
				vs[r + 1] - vs[r], argb);
}

void gfx_round_rect(float x, float y, float w, float h, float radius, uint32_t argb)
{
	nine_slice(SMC_SHAPE_ROUND_X, SMC_SHAPE_ROUND_Y, SMC_SHAPE_ROUND_W, SMC_SHAPE_ROUND_H, SMC_SHAPE_ROUND_EDGE, x, y, w,
		h, radius, argb);
}

void gfx_round_outline(float x, float y, float w, float h, float radius, uint32_t argb)
{
	nine_slice(SMC_SHAPE_RING_X, SMC_SHAPE_RING_Y, SMC_SHAPE_RING_W, SMC_SHAPE_RING_H, SMC_SHAPE_RING_EDGE, x, y, w, h,
		radius, argb);
}

void gfx_circle(float cx, float cy, float r, uint32_t argb)
{
	atlas_quad(cx - r, cy - r, r * 2.0f, r * 2.0f, SMC_SHAPE_CIRCLE_X, SMC_SHAPE_CIRCLE_Y, SMC_SHAPE_CIRCLE_W,
		SMC_SHAPE_CIRCLE_H, argb);
}

void gfx_shadow(float x, float y, float w, float h, float spread, uint32_t argb)
{
	nine_slice(SMC_SHAPE_SHADOW_X, SMC_SHAPE_SHADOW_Y, SMC_SHAPE_SHADOW_W, SMC_SHAPE_SHADOW_H, SMC_SHAPE_SHADOW_EDGE,
		x - spread, y - spread, w + spread * 2.0f, h + spread * 2.0f, spread * 2.0f, argb);
}

void gfx_icon(int icon, float x, float y, float size, uint32_t argb)
{
	if (icon < 0 || icon >= ICON_COUNT)
		return;
	atlas_quad(x, y, size, size, smc_icon_xy[icon][0], smc_icon_xy[icon][1], SMC_ICON_PX, SMC_ICON_PX, argb);
}

// ---------------------------------------------------------------------------------------------------------------
// text

static uint32_t next_code_point(const uint8_t **s)
{
	const uint8_t *p = *s;
	*s = p + 1;
	if (p[0] < 0x80)
		return p[0];
	const uint32_t len = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC0 ? 2 : 0;
	if (!len || p[0] >= 0xF8)
		return '?';
	uint32_t cp = p[0] & (0x7F >> len);
	for (uint32_t i = 1; i < len; i++) {
		if ((p[i] & 0xC0) != 0x80)
			return '?';
		cp = cp << 6 | (p[i] & 0x3F);
	}
	*s = p + len;
	return cp;
}

static const SmcGlyph *glyph_of(int font, uint32_t cp)
{
	uint16_t index = cp < 128 ? g.ascii[font][cp] : find_glyph(font, cp);
	if (index == 0xFFFF)
		index = cp < 128 ? g.ascii[font]['?'] : find_glyph(font, '?');
	if (index == 0xFFFF)
		return NULL;
	return &smc_fonts[font].glyphs[index];
}

float gfx_line_height(int font)
{
	if (font < 0 || font >= FONT_COUNT)
		return 0.0f;
	return (float)(smc_fonts[font].ascent + smc_fonts[font].descent);
}

float gfx_text_width(int font, const char *utf8)
{
	if (font < 0 || font >= FONT_COUNT || !utf8)
		return 0.0f;
	float w = 0.0f;
	for (const uint8_t *s = (const uint8_t *)utf8; *s;) {
		const SmcGlyph *gl = glyph_of(font, next_code_point(&s));
		if (gl)
			w += gl->advance;
	}
	return w;
}

static float draw_run(int font, float x, float y, uint32_t argb, const char *utf8, size_t max_bytes)
{
	const float baseline = y + (float)smc_fonts[font].ascent;
	float pen = x;
	const uint8_t *s = (const uint8_t *)utf8;
	const uint8_t *const end = s + max_bytes;
	while (*s && s < end) {
		const uint32_t cp = next_code_point(&s);
		if (cp == '\n' || cp < 0x20)
			continue;
		const SmcGlyph *gl = glyph_of(font, cp);
		if (!gl)
			continue;
		if (gl->w && gl->h) {
			// Whole pixels keep the glyphs as crisp as they were rasterised.
			const float gx = floorf(pen + (float)gl->xoff + 0.5f);
			const float gy = floorf(baseline + (float)gl->yoff + 0.5f);
			atlas_quad(gx, gy, gl->w, gl->h, gl->x, gl->y, gl->w, gl->h, argb);
		}
		pen += gl->advance;
	}
	return pen - x;
}

float gfx_text(int font, float x, float y, uint32_t argb, const char *utf8)
{
	if (font < 0 || font >= FONT_COUNT || !utf8)
		return 0.0f;
	return draw_run(font, x, y, argb, utf8, strlen(utf8));
}

float gfx_text_fit(int font, float x, float y, float max_w, uint32_t argb, const char *utf8)
{
	if (font < 0 || font >= FONT_COUNT || !utf8)
		return 0.0f;
	if (gfx_text_width(font, utf8) <= max_w)
		return gfx_text(font, x, y, argb, utf8);
	const char *ellipsis = "\xE2\x80\xA6";
	const float ew = gfx_text_width(font, ellipsis);
	float w = 0.0f;
	const uint8_t *s = (const uint8_t *)utf8;
	const uint8_t *cut = s;
	while (*s) {
		const uint8_t *before = s;
		const SmcGlyph *gl = glyph_of(font, next_code_point(&s));
		const float adv = gl ? gl->advance : 0.0f;
		if (w + adv + ew > max_w) {
			cut = before;
			break;
		}
		w += adv;
		cut = s;
	}
	float drawn = draw_run(font, x, y, argb, utf8, (size_t)(cut - (const uint8_t *)utf8));
	drawn += gfx_text(font, x + drawn, y, argb, ellipsis);
	return drawn;
}

float gfx_text_wrap(int font, float x, float y, float max_w, int max_lines, uint32_t argb, const char *utf8)
{
	if (font < 0 || font >= FONT_COUNT || !utf8 || max_lines <= 0)
		return 0.0f;
	const float lh = gfx_line_height(font) + 4.0f;
	char line[512];
	int lines = 0;
	const char *p = utf8;
	while (*p && lines < max_lines) {
		while (*p == ' ')
			p++;
		size_t len = 0;
		size_t last_fit = 0;
		const char *q = p;
		while (*q && *q != '\n') {
			const char *word_end = q;
			while (*word_end && *word_end != ' ' && *word_end != '\n')
				word_end++;
			const size_t candidate = (size_t)(word_end - p);
			if (candidate >= sizeof line)
				break;
			memcpy(line, p, candidate);
			line[candidate] = 0;
			if (gfx_text_width(font, line) > max_w && last_fit)
				break;
			last_fit = candidate;
			q = word_end;
			while (*q == ' ')
				q++;
		}
		len = last_fit ? last_fit : (size_t)(q - p);
		if (len >= sizeof line)
			len = sizeof line - 1;
		memcpy(line, p, len);
		line[len] = 0;
		p += len;
		lines++;
		if (lines == max_lines && *p && *p != '\n')
			gfx_text_fit(font, x, y, max_w - 1.0f, argb, (strcat(line, " \xE2\x80\xA6"), line));
		else
			gfx_text_fit(font, x, y, max_w, argb, line);
		y += lh;
		if (*p == '\n')
			p++;
	}
	return (float)lines * lh;
}

// ---------------------------------------------------------------------------------------------------------------
// images

bool gfx_image_from_rgba(GfxImage *image, const uint8_t *rgba, uint32_t width, uint32_t height)
{
	memset(image, 0, sizeof *image);
	if (!width || !height || width > 4096 || height > 4096)
		return false;
	NTSTATUS status = xsdk_texture_create(&image->texture, XSDK_FORMAT_A8R8G8B8, width, height, 1, true);
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: texture %ux%u failed: 0x%08X", (unsigned)width, (unsigned)height, (unsigned)status);
		return false;
	}
	uint32_t *row = malloc(width * 4);
	if (!row) {
		xsdk_texture_free(&image->texture);
		return false;
	}
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *src = rgba + (size_t)y * width * 4;
		for (uint32_t x = 0; x < width; x++, src += 4)
			row[x] = (uint32_t)src[3] << 24 | (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];
		xsdk_texture_upload_rows(&image->texture, 0, y, 1, row, width * 4);
	}
	free(row);
	xsdk_gpu_invalidate(image->texture.address, image->texture.size_bytes);
	image->width = width;
	image->height = height;
	image->valid = true;
	g.textures_live++;
	g.texture_bytes_live += image->texture.size_bytes;
	return true;
}

bool gfx_image_from_memory(GfxImage *image, const uint8_t *data, uint32_t size)
{
	int w = 0;
	int h = 0;
	uint8_t *rgba = image_decode_memory_rgba(data, size, &w, &h);
	if (!rgba)
		return false;
	const bool ok = gfx_image_from_rgba(image, rgba, (uint32_t)w, (uint32_t)h);
	free(rgba);
	return ok;
}

uint8_t *gfx_decode_file(const char *path, uint32_t max_w, uint32_t max_h, uint32_t *width, uint32_t *height)
{
	return image_decode_file_fit(path, max_w, max_h, width, height);
}

void gfx_image_free(GfxImage *image)
{
	if (!image || !image->valid)
		return;
	// The GPU may still sample it in a queued frame.
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), WAIT_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		smc_log("gfx: GPU busy, leaking a texture rather than freeing it under the GPU");
		image->valid = false;
		return;
	}
	g.textures_live--;
	g.texture_bytes_live -= image->texture.size_bytes;
	xsdk_texture_free(&image->texture);
	memset(image, 0, sizeof *image);
}

void gfx_image_release(GfxImage *image)
{
	if (!image || !image->valid)
		return;
	if (g.release_count == RELEASES) {
		gfx_image_free(image);
		return;
	}
	g.releases[g.release_count].texture = image->texture;
	g.releases[g.release_count].fence = xsdk_gpu_fence();
	g.release_count++;
	memset(image, 0, sizeof *image);
}

void gfx_image(const GfxImage *image, float x, float y, float w, float h, uint32_t tint)
{
	if (!image || !image->valid)
		return;
	quad(&image->texture, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, tint);
}

void gfx_image_cover(const GfxImage *image, float x, float y, float w, float h, uint32_t tint)
{
	if (!image || !image->valid || w <= 0.0f || h <= 0.0f)
		return;
	const float ia = (float)image->width / (float)image->height;
	const float ra = w / h;
	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	if (ia > ra) {
		const float keep = ra / ia;
		u0 = (1.0f - keep) * 0.5f;
		u1 = u0 + keep;
	} else {
		const float keep = ia / ra;
		v0 = (1.0f - keep) * 0.5f;
		v1 = v0 + keep;
	}
	quad(&image->texture, x, y, x + w, y + h, u0, v0, u1, v1, tint);
}

const GfxImage *gfx_badge(void)
{
	return &g.badge;
}

// ---------------------------------------------------------------------------------------------------------------
// video

bool gfx_video_create(GfxVideo *video, uint32_t width, uint32_t height, uint32_t chroma_width, uint32_t chroma_height)
{
	memset(video, 0, sizeof *video);
	if (!g.video_shader || !width || !height || width > 4096 || height > 4096)
		return false;
	for (int set = 0; set < GFX_VIDEO_SETS; set++)
		for (int p = 0; p < 3; p++) {
			const uint32_t w = p ? chroma_width : width;
			const uint32_t h = p ? chroma_height : height;
			// Linear: an upload is a copy of each row, with no tiling for the CPU to do.
			const NTSTATUS status = xsdk_texture_create(&video->planes[set][p], XSDK_FORMAT_R8, w, h, 1, false);
			if (!NT_SUCCESS(status)) {
				smc_log("gfx: video plane %ux%u failed: 0x%08X", (unsigned)w, (unsigned)h, (unsigned)status);
				for (int s2 = 0; s2 <= set; s2++)
					for (int p2 = 0; p2 < 3; p2++)
						if (video->planes[s2][p2].texels && (s2 < set || p2 < p))
							xsdk_texture_free(&video->planes[s2][p2]);
				memset(video, 0, sizeof *video);
				return false;
			}
			g.textures_live++;
			g.texture_bytes_live += video->planes[set][p].size_bytes;
		}
	video->width = width;
	video->height = height;
	video->chroma_width = chroma_width;
	video->chroma_height = chroma_height;
	video->current = GFX_VIDEO_SETS - 1;
	video->valid = true;
	return true;
}

// Rows of the matrix from (y, u, v, 1) to red, green and blue for the YCbCr standard's red and blue weights.
static void yuv_matrix(float kr, float kb, bool full_range, float m[12])
{
	const float kg = 1.0f - kr - kb;
	const float ys = full_range ? 1.0f : 255.0f / 219.0f;
	const float yo = full_range ? 0.0f : -16.0f / 219.0f;
	const float cs = full_range ? 1.0f : 255.0f / 224.0f;
	const float co = full_range ? -128.0f / 255.0f : -128.0f / 224.0f;
	const float rv = 2.0f - 2.0f * kr;
	const float bu = 2.0f - 2.0f * kb;
	const float gu = kb * bu / kg;
	const float gv = kr * rv / kg;
	const float rows[12] = { ys, 0.0f, rv * cs, yo + rv * co, ys, -gu * cs, -gv * cs, yo - gu * co - gv * co, ys,
		bu * cs, 0.0f, yo + bu * co };
	memcpy(m, rows, sizeof rows);
}

void gfx_video_upload(GfxVideo *video, const uint8_t *const planes[3], const int linesize[3], bool bt709,
	bool full_range)
{
	if (!video->valid)
		return;
	const int set = (video->current + 1) % GFX_VIDEO_SETS;
	// The GPU may still be drawing an older frame from this set.
	if (video->fences[set] && !xsdk_gpu_fence_done(video->fences[set])) {
		const NTSTATUS status = xsdk_gpu_wait(video->fences[set], WAIT_TIMEOUT_MS);
		if (!NT_SUCCESS(status))
			smc_log("gfx: waiting for a video set failed: 0x%08X", (unsigned)status);
	}
	for (int p = 0; p < 3; p++) {
		XsdkTexture *t = &video->planes[set][p];
		xsdk_texture_upload_rows(t, 0, 0, t->height, planes[p], (uint32_t)linesize[p]);
		xsdk_gpu_invalidate(t->address, t->size_bytes);
	}
	if (bt709)
		yuv_matrix(0.2126f, 0.0722f, full_range, video->matrix);
	else
		yuv_matrix(0.299f, 0.114f, full_range, video->matrix);
	video->current = set;
	video->ready = true;
}

void gfx_video_draw(GfxVideo *video, float x, float y, float w, float h, uint32_t tint)
{
	if (!g.in_frame || !video->valid || !video->ready || w <= 0.0f || h <= 0.0f)
		return;
	flush_run();
	g.run_texture = NULL;
	if (g.block_quads == BLOCK_QUADS)
		next_block();
	const uint32_t first = g.block_quads;
	// The quad as any other, its texture coordinates the whole frame.
	g.run_texture = &video->planes[video->current][0];
	quad(g.run_texture, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, tint);
	g.run_texture = NULL;
	g.run_first = g.block_quads;
	const uint32_t address = g.ring_address + (g.block * BLOCK_QUADS + first) * QUAD_BYTES;
	xsdk_gpu_invalidate(address, QUAD_BYTES);
	xsdk_gpu_set_shaders(&g.vertex, &g.video_pixel);
	xsdk_built_shader_set_literals(&ui_vert);
	xsdk_built_shader_set_literals(&video_frag);
	xsdk_gpu_set_pixel_constants(VIDEO_FRAG_PUSH, video->matrix, VIDEO_FRAG_PUSH_REGISTERS);
	xsdk_gpu_set_texture(VIDEO_FRAG_SAMPLER_0_0, &video->planes[video->current][0], &g.linear);
	xsdk_gpu_set_texture(VIDEO_FRAG_SAMPLER_0_1, &video->planes[video->current][1], &g.linear);
	xsdk_gpu_set_texture(VIDEO_FRAG_SAMPLER_0_2, &video->planes[video->current][2], &g.linear);
	xsdk_gpu_set_vertices(0, address, QUAD_BYTES);
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, g.indices.address, INDICES_PER_QUAD, g.indices.indices_32_bit);
	g.draws++;
	video->fences[video->current] = xsdk_gpu_fence();
	// Back to the UI's shaders for what's drawn after.
	set_state();
}

void gfx_video_free(GfxVideo *video)
{
	if (!video->valid)
		return;
	for (int set = 0; set < GFX_VIDEO_SETS; set++)
		for (int p = 0; p < 3; p++) {
			if (g.release_count < RELEASES) {
				g.releases[g.release_count].texture = video->planes[set][p];
				g.releases[g.release_count].fence = xsdk_gpu_fence();
				g.release_count++;
			} else {
				xsdk_gpu_wait(xsdk_gpu_fence(), WAIT_TIMEOUT_MS);
				g.texture_bytes_live -= video->planes[set][p].size_bytes;
				g.textures_live--;
				xsdk_texture_free(&video->planes[set][p]);
			}
		}
	memset(video, 0, sizeof *video);
}

void gfx_stats(GfxStats *stats)
{
	XsdkPresentStats ps;
	xsdk_present_stats(&ps);
	stats->frame_ms_avg = g.frame_ms_avg;
	stats->frame_ms_max = g.frame_ms_max;
	stats->quads_last_frame = g.last_quads;
	stats->draws_last_frame = g.last_draws;
	stats->missed_vblanks = ps.missed;
	stats->textures_live = g.textures_live;
	stats->texture_bytes_live = g.texture_bytes_live;
}
