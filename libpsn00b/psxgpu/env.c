/*
 * PSn00bSDK GPU library (DRAWENV/DISPENV functions)
 * (C) 2022 spicyjpeg - MPL licensed
 */

#include <stdint.h>
#include <psxgpu.h>
#include <hwregs_c.h>

#define _min(x, y) (((x) < (y)) ? (x) : (y))

extern GPU_VideoMode _gpu_video_mode;

/* Private utilities */

// Converts a texture window size value (a power of two in 8-128 range) into a
// bit mask by setting the leading zeroes of the value:
//     0 = 0b00000000 -> 0b00000
//     8 = 0b00001000 -> 0b11111
//    16 = 0b00010000 -> 0b11110
//    32 = 0b00100000 -> 0b11100
//    64 = 0b01000000 -> 0b11000
//   128 = 0b10000000 -> 0b10000
// The GPU uses the mask to process texture coordinates as follows:
//   x &= ~(mask << 3)
//   x |= (offset << 3) & (mask << 3)
static inline uint32_t _get_window_mask(int size) {
	uint32_t mask = size >> 3;

	mask |= mask << 1;
	mask |= mask << 2;
	mask |= mask << 4;
	return mask & 0x1f;
}

/* Drawing API */

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h) {
	env->clip.x = x;
	env->clip.y = y;
	env->clip.w = w;
	env->clip.h = h;

	env->ofs[0] = 0;
	env->ofs[1] = 0;

	env->tw.x = 0;
	env->tw.y = 0;
	env->tw.w = 256;
	env->tw.h = 256;

	env->tpage = 0x0a;
	env->dtd   = 1;
	env->dfe   = 0;
	env->isbg  = 0;
	setRGB0(env, 0, 0, 0);

	env->dr_env.tag = 0;
	return env;
}

int DrawOTagEnv(const uint32_t *ot, DRAWENV *env) {
	DR_ENV *prim = &(env->dr_env);

	// All commands are grouped into a single display list packet for
	// performance reasons (the GPU does not care about the grouping as the
	// display list is parsed by the DMA unit in the CPU).
	setaddr(prim, ot);
	setlen(prim, 5);

	// Texture page (reset active page and set dither/mask bits)
	prim->code[0]  = 0xe1000000 | env->tpage;
	prim->code[0] |= (env->dtd & 1) << 9;
	prim->code[0] |= (env->dfe & 1) << 10;

	// Texture window
	prim->code[1]  = 0xe2000000;
	prim->code[1] |= _get_window_mask(env->tw.w);
	prim->code[1] |= _get_window_mask(env->tw.h) << 5;
	prim->code[1] |= (env->tw.x & 0xf8) << 7;  // ((tw.x / 8) & 0x1f) << 10
	prim->code[1] |= (env->tw.y & 0xf8) << 12; // ((tw.y / 8) & 0x1f) << 15

	// Set drawing area top left
	prim->code[2]  = 0xe3000000;
	prim->code[2] |=  env->clip.x & 0x3ff;
	prim->code[2] |= (env->clip.y & 0x3ff) << 10;

	// Set drawing area bottom right
	prim->code[3]  = 0xe4000000;
	prim->code[3] |=  (env->clip.x + (env->clip.w - 1)) & 0x3ff;
	prim->code[3] |= ((env->clip.y + (env->clip.h - 1)) & 0x3ff) << 10;

	// Set drawing offset
	prim->code[4]  = 0xe5000000;
	prim->code[4] |=  (env->clip.x + env->ofs[0]) & 0x7ff;
	prim->code[4] |= ((env->clip.y + env->ofs[1]) & 0x7ff) << 11;

	if (env->isbg) {
		setlen(prim, 8);

		// Rectangle fill
		// FIXME: reportedly this command doesn't accept height values >511...
		prim->code[5]  = 0x02000000;
		//prim->code[5] |= env->r0 | (env->g0 << 8) | (env->b0 << 16);
		//prim->code[6]  = env->clip.x;
		//prim->code[6] |= env->clip.y << 16;
		prim->code[5] |= *((const uint32_t *) &(env->isbg)) >> 8;
		prim->code[6]  = *((const uint32_t *) &(env->clip.x));
		prim->code[7]  = env->clip.w;
		prim->code[7] |= _min(env->clip.h, 0x1ff) << 16;
	}

	return EnqueueDrawOp((void *) &DrawOTag2, (uint32_t) prim, 0, 0);
}

void PutDrawEnv(DRAWENV *env) {
	DrawOTagEnv((const uint32_t *) 0x00ffffff, env);
}

// This function skips rebuilding the cached packet whenever possible and is
// useful if the DRAWENV structure is never modified (which is the case most of
// the time).
void PutDrawEnvFast(DRAWENV *env) {
	if (!(env->dr_env.tag))
		DrawOTagEnv((const uint32_t *) 0x00ffffff, env);
	else
		DrawOTag((const uint32_t *) &(env->dr_env));
}

/* Display API */

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h) {
	env->disp.x = x;
	env->disp.y = y;
	env->disp.w = w;
	env->disp.h = h;

	env->screen.x = 0;
	env->screen.y = 0;
	env->screen.w = 0;
	env->screen.h = 0;

	env->isinter = 0;
	env->isrgb24 = 0;
	env->reverse = 0;

	return env;
}

void PutDispEnv(const DISPENV *env) {
	uint32_t h_range, v_range, mode, fb_pos;

	mode  = _gpu_video_mode << 3;
	mode |= (env->isrgb24 & 1) << 4;
	mode |= (env->isinter & 1) << 5;
	mode |= (env->reverse & 1) << 7;

	if (env->disp.h > 256)
		mode |= 1 << 2;

	// Calculate the horizontal display range values. The original code was
	// this bad; in actual fact it was even worse due to being written in
	// assembly and using slow multiplication even when not necessary.
	int offset, span, default_span = 2560;

	if (env->disp.w > 560) {
		// 640 pixels
		mode  |= 3;
		offset = 620;
		span   = env->screen.w * 4;
	} else if (env->disp.w > 400) {
		// 512 pixels
		mode  |= 2;
		offset = 615;
		span   = env->screen.w * 4 + env->screen.w;
	} else if (env->disp.w > 352) {
		// 384 pixels (this mode is weird)
		mode  |= 1 << 6;
		offset = 539;
		span   = env->screen.w * 8 - env->screen.w;
		default_span = 2688;
	} else if (env->disp.w > 280) {
		// 320 pixels
		mode  |= 1;
		offset = 600;
		span   = env->screen.w * 8;
	} else {
		// 256 pixels
		offset = 590;
		span   = env->screen.w * 8 + env->screen.w * 2;
	}

	offset += env->screen.x * 4;
	if (!span)
		span = default_span;

	h_range  = offset & 0xfff;
	h_range |= ((offset + span) & 0xfff) << 12;

	// Calculate the vertical display range values.
	offset = 16 + env->screen.y;
	span   = env->screen.h ? env->screen.h : 240;

	v_range  = offset & 0x3ff;
	v_range |= ((offset + span) & 0x3ff) << 10;

	fb_pos  =  env->disp.x & 0x3ff;
	fb_pos |= (env->disp.y & 0x1ff) << 10;

	GPU_GP1 = 0x06000000 | h_range; // Set horizontal display range
	GPU_GP1 = 0x07000000 | v_range; // Set vertical display range
	GPU_GP1 = 0x08000000 | mode;    // Set video mode
	GPU_GP1 = 0x05000000 | fb_pos;  // Set VRAM location to display
}

/* Deprecated "raw" display API */

void PutDispEnvRaw(const DISPENV_RAW *env) {
	uint32_t h_range, v_range, fb_pos;

	h_range  =   608 + env->vid_xpos;
	h_range |= (3168 + env->vid_xpos) << 12;

	// FIXME: these hardcoded values are for NTSC displays.
	v_range  =  (136 - 120 + env->vid_ypos) & 0x3ff;
	v_range |= ((136 + 120 + env->vid_ypos) & 0x3ff) << 12;

	fb_pos  =  env->fb_x & 0x3ff;
	fb_pos |= (env->fb_y & 0x1ff) << 10;

	GPU_GP1 = 0x06000000 | h_range;       // Set horizontal display range
	GPU_GP1 = 0x07000000 | v_range;       // Set vertical display range
	GPU_GP1 = 0x08000000 | env->vid_mode; // Set video mode
	GPU_GP1 = 0x05000000 | fb_pos;        // Set VRAM location to display
}
