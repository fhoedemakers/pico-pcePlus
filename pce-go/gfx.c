// gfx.c - VDC/VCE Emulation
//
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico.h"
#include "pico/time.h"
#include "pce.h"
#include "gfx.h"

// SGX profiling: accumulate microseconds spent in each render phase across a
// frame and print totals every ~1 s. Compile out by setting SGX_PROFILE=0.
#ifndef SGX_PROFILE
#define SGX_PROFILE 0
#endif

#if SGX_PROFILE
static uint32_t sgx_prof_clear_us;
static uint32_t sgx_prof_vdc1_us;
static uint32_t sgx_prof_vdc2_us;
static uint32_t sgx_prof_mix_us;
static uint32_t sgx_prof_total_us;
static uint32_t sgx_prof_frames;
static uint32_t sgx_prof_lines;

static inline void sgx_prof_dump_if_due(void)
{
	if (sgx_prof_frames < 60) return;
	printf("[SGX] %luf %lul  clr=%lu vdc1=%lu vdc2=%lu mix=%lu tot=%lu (us/frame avg)\n",
		(unsigned long)sgx_prof_frames,
		(unsigned long)sgx_prof_lines,
		(unsigned long)(sgx_prof_clear_us / sgx_prof_frames),
		(unsigned long)(sgx_prof_vdc1_us  / sgx_prof_frames),
		(unsigned long)(sgx_prof_vdc2_us  / sgx_prof_frames),
		(unsigned long)(sgx_prof_mix_us   / sgx_prof_frames),
		(unsigned long)(sgx_prof_total_us / sgx_prof_frames));
	sgx_prof_clear_us = sgx_prof_vdc1_us = sgx_prof_vdc2_us = 0;
	sgx_prof_mix_us = sgx_prof_total_us = 0;
	sgx_prof_frames = sgx_prof_lines = 0;
}
#endif

#define PAL(nibble) (PAL[(L >> ((nibble) * 4)) & 15])

#define V_FLIP  0x8000
#define H_FLIP  0x0800

// Per-VDC rendering context. Lets draw_tiles/draw_sprites/etc. work against
// either VDC1 (always, today) or a second VDC (SuperGrafx, future step) by
// swapping the pointer. The macros that read VDC1's globals (IO_VDC_REG and
// friends) are unchanged — they remain the canonical way to read VDC1 from
// non-renderer code (CPU/IO/cd.c). Renderer code uses CTX_REG instead.
typedef struct {
	const UWord *regs;       // VDC register file
	const uint16_t *vram;    // 64 KB VRAM
	const sprite_t *spram;   // 64-entry sprite attribute table
} vdc_ctx_t;

#define CTX_REG(ctx, R)            ((ctx)->regs[R])
#define CTX_SCREEN_WIDTH(ctx)      ((CTX_REG(ctx, HDR).B.l + 1) * 8)

static vdc_ctx_t vdc1_ctx;
static const vdc_ctx_t *const VDC1_CTX = &vdc1_ctx;

// Bind the VDC1 rendering context to the canonical PCE globals. Called by
// pce_init() once PCE.VRAM has been malloc'd — gfx_init() runs first (per
// InitPCE) so we can't bind there.
void gfx_bind_vdc1(void)
{
	vdc1_ctx.regs  = PCE.VDC.regs;
	vdc1_ctx.vram  = PCE.VRAM;
	vdc1_ctx.spram = PCE.SPRAM;
}

static int last_line_counter = 0;
static int line_counter = 0;

static struct {
	int scroll_x;
	int scroll_y;
	int control;
	int latched;
} gfx_context;

static uint8_t *framebuffer_top, *framebuffer_bottom;

// Bitmask of which sprites (0..63) intersect each scanline.
// Built once per frame at the start of rendering; lets draw_sprites()
// skip sprites that don't touch the current line without iterating all 64.
// In SuperGrafx mode VDC2 uses sprite_bitmask_per_line_vdc2 in parallel.
static uint64_t sprite_bitmask_per_line[XBUF_HEIGHT];
static uint64_t sprite_bitmask_per_line_vdc2[XBUF_HEIGHT];

static void __not_in_flash_func(build_sprite_lists)(const vdc_ctx_t *ctx, uint64_t *bitmask)
{
	memset(bitmask, 0, sizeof(uint64_t) * XBUF_HEIGHT);

	for (int n = 0; n < 64; n++) {
		const sprite_t *spr = &ctx->spram[n];
		uint32_t attr = spr->attr;
		int y = (spr->y & 0x3FF) - 64;
		int x = (spr->x & 0x3FF) - 32;
		int cgx = (attr >> 8) & 1;
		int cgy = (attr >> 12) & 3;
		cgy |= cgy >> 1;
		int height = (cgy + 1) * 16;
		int width = (cgx + 1) * 16;

		// Sprite entirely off-screen horizontally
		if (x >= XBUF_WIDTH || x + width <= 0) continue;

		int y_start = (y < 0) ? 0 : y;
		int y_end = y + height;
		if (y_end > XBUF_HEIGHT) y_end = XBUF_HEIGHT;
		if (y_start >= y_end) continue;

		uint64_t bit = (uint64_t)1 << n;
		for (int line = y_start; line < y_end; line++) {
			bitmask[line] |= bit;
		}
	}
}

/*
	Draw background tiles between two lines
*/
static void __not_in_flash_func(draw_tiles)(const vdc_ctx_t *ctx, uint8_t *screen_buffer, int Y1, int Y2, int scroll_x, int scroll_y)
{
	TRACE_GFX("Rendering tiles on lines %3d - %3d\tScroll: (%3d,%3d)\n", Y1, Y2, scroll_x, scroll_y);

	uint32_t _bg_w[] = { 32, 64, 128, 128 };
	uint32_t _bg_h[] = { 32, 64 };

	uint32_t bg_w = _bg_w[(CTX_REG(ctx, MWR).W >> 4) & 3]; // Bits 5-4 select the width
	uint32_t bg_h = _bg_h[(CTX_REG(ctx, MWR).W >> 6) & 1]; // Bit 6 selects the height

	int num_tiles = MIN(CTX_SCREEN_WIDTH(ctx) / 8 + 1, XBUF_WIDTH / 8 + 1);
	int x;
	int y = Y1 + scroll_y;
	int offset = y & 7;
	int h = MIN(8 - offset, Y2 - Y1);

	y >>= 3;

	uint8_t *PP = (screen_buffer + XBUF_WIDTH * Y1) - (scroll_x & 7);

	for (int line = Y1; line < Y2; y++) {
		x = scroll_x / 8;
		y &= bg_h - 1;
		for (int n = 0; n < num_tiles; n++, x++, PP += 8) {
			x &= bg_w - 1;

			int no = ctx->vram[x + y * bg_w];

			uint8_t *PAL = &PCE.Palette[(no >> 8) & 0x1F0];
			uint8_t *C = (uint8_t*)(ctx->vram + (no & 0x7FF) * 16 + offset);
			uint8_t *P = PP;

			for (int i = 0; i < h; i++, P += XBUF_WIDTH, C += 2) {
				uint32_t J, L, M;

				J = C[0] | C[1] | C[16] | C[17];

				if (!J)
					continue;

				if (P + 8 >= framebuffer_bottom) {
					MESSAGE_DEBUG("tile overflow!\n");
					break;
				} else if (P < framebuffer_top) {
					MESSAGE_DEBUG("tile underflow!\n");
					continue;
				}

				M = C[0];
				L = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
				M = C[1];
				L |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
				M = C[16];
				L |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
				M = C[17];
				L |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);

				if (J & 0x80) P[0] = PAL(1);
				if (J & 0x40) P[1] = PAL(3);
				if (J & 0x20) P[2] = PAL(5);
				if (J & 0x10) P[3] = PAL(7);
				if (J & 0x08) P[4] = PAL(0);
				if (J & 0x04) P[5] = PAL(2);
				if (J & 0x02) P[6] = PAL(4);
				if (J & 0x01) P[7] = PAL(6);
			}
		}
		line += h;
		PP += XBUF_WIDTH * h - num_tiles * 8;
		offset = 0;
		h = MIN(8, Y2 - line);
	}
}


/*
	Draw sprite C to framebuffer P
*/
static void __not_in_flash_func(draw_sprite)(uint8_t *P, const uint16_t *C, int height, uint32_t attr)
{
	uint8_t *PAL = &PCE.Palette[256 + ((attr & 0xF) << 4)];

	bool hflip = attr & H_FLIP;
	int inc = 1; //(attr & V_FLIP) ? -1 : 1;

	if (attr & V_FLIP) {
		inc = -1;
		C = C + height - 1;
	}

	for (int i = 0; i < height; i++, C += inc, P += XBUF_WIDTH) {

		uint32_t J = C[0] | C[16] | C[32] | C[48];
		uint32_t L1, L2, L, M;

		if (!J)
			continue;

		// This will also need to be handled in draw_sprites... (it could adjust simply constrain the height)
		if (P + 16 >= framebuffer_bottom) {
			MESSAGE_DEBUG("sprite overflow %d!\n", i);
			break;
		} else if (P < framebuffer_top) {
			MESSAGE_DEBUG("sprite underflow %d!\n", i);
			continue;
		}

		M = C[0];
		L1 = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
		L2 = ((M & 0x8800) >> 11) | ((M & 0x4400) >> 2) | ((M & 0x2200) << 7) | ((M & 0x1100) << 16);
		M = C[16];
		L1 |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
		L2 |= ((M & 0x8800) >> 10) | ((M & 0x4400) >> 1) | ((M & 0x2200) << 8) | ((M & 0x1100) << 17);
		M = C[32];
		L1 |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
		L2 |= ((M & 0x8800) >> 9) | ((M & 0x4400) >> 0) | ((M & 0x2200) << 9) | ((M & 0x1100) << 18);
		M = C[48];
		L1 |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);
		L2 |= ((M & 0x8800) >> 8) | ((M & 0x4400) << 1) | ((M & 0x2200) << 10) | ((M & 0x1100) << 19);

		if (hflip) {
			L = L2;
			// P[12..15] (J bits 15..12)
			if ((J & 0xF000) == 0xF000) {
				uint32_t v = (uint32_t)PAL[L >> 28]
				           | ((uint32_t)PAL[(L >> 20) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 12) & 0xF] << 16)
				           | ((uint32_t)PAL[(L >> 4) & 0xF] << 24);
				*(uint32_t *)(P + 12) = v;
			} else {
				if ((J & 0x8000)) P[15] = PAL(1);
				if ((J & 0x4000)) P[14] = PAL(3);
				if ((J & 0x2000)) P[13] = PAL(5);
				if ((J & 0x1000)) P[12] = PAL(7);
			}
			// P[8..11] (J bits 11..8)
			if ((J & 0x0F00) == 0x0F00) {
				uint32_t v = (uint32_t)PAL[(L >> 24) & 0xF]
				           | ((uint32_t)PAL[(L >> 16) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 8) & 0xF] << 16)
				           | ((uint32_t)PAL[L & 0xF] << 24);
				*(uint32_t *)(P + 8) = v;
			} else {
				if ((J & 0x0800)) P[11] = PAL(0);
				if ((J & 0x0400)) P[10] = PAL(2);
				if ((J & 0x0200)) P[9]  = PAL(4);
				if ((J & 0x0100)) P[8]  = PAL(6);
			}

			L = L1;
			// P[4..7] (J bits 7..4)
			if ((J & 0xF0) == 0xF0) {
				uint32_t v = (uint32_t)PAL[L >> 28]
				           | ((uint32_t)PAL[(L >> 20) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 12) & 0xF] << 16)
				           | ((uint32_t)PAL[(L >> 4) & 0xF] << 24);
				*(uint32_t *)(P + 4) = v;
			} else {
				if ((J & 0x80)) P[7] = PAL(1);
				if ((J & 0x40)) P[6] = PAL(3);
				if ((J & 0x20)) P[5] = PAL(5);
				if ((J & 0x10)) P[4] = PAL(7);
			}
			// P[0..3] (J bits 3..0)
			if ((J & 0x0F) == 0x0F) {
				uint32_t v = (uint32_t)PAL[(L >> 24) & 0xF]
				           | ((uint32_t)PAL[(L >> 16) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 8) & 0xF] << 16)
				           | ((uint32_t)PAL[L & 0xF] << 24);
				*(uint32_t *)(P + 0) = v;
			} else {
				if ((J & 0x08)) P[3] = PAL(0);
				if ((J & 0x04)) P[2] = PAL(2);
				if ((J & 0x02)) P[1] = PAL(4);
				if ((J & 0x01)) P[0] = PAL(6);
			}
		} else {
			L = L2;
			// P[0..3] (J bits 15..12)
			if ((J & 0xF000) == 0xF000) {
				uint32_t v = (uint32_t)PAL[(L >> 4) & 0xF]
				           | ((uint32_t)PAL[(L >> 12) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 20) & 0xF] << 16)
				           | ((uint32_t)PAL[L >> 28] << 24);
				*(uint32_t *)(P + 0) = v;
			} else {
				if ((J & 0x8000)) P[0] = PAL(1);
				if ((J & 0x4000)) P[1] = PAL(3);
				if ((J & 0x2000)) P[2] = PAL(5);
				if ((J & 0x1000)) P[3] = PAL(7);
			}
			// P[4..7] (J bits 11..8)
			if ((J & 0x0F00) == 0x0F00) {
				uint32_t v = (uint32_t)PAL[L & 0xF]
				           | ((uint32_t)PAL[(L >> 8) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 16) & 0xF] << 16)
				           | ((uint32_t)PAL[(L >> 24) & 0xF] << 24);
				*(uint32_t *)(P + 4) = v;
			} else {
				if ((J & 0x0800)) P[4] = PAL(0);
				if ((J & 0x0400)) P[5] = PAL(2);
				if ((J & 0x0200)) P[6] = PAL(4);
				if ((J & 0x0100)) P[7] = PAL(6);
			}

			L = L1;
			// P[8..11] (J bits 7..4)
			if ((J & 0xF0) == 0xF0) {
				uint32_t v = (uint32_t)PAL[(L >> 4) & 0xF]
				           | ((uint32_t)PAL[(L >> 12) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 20) & 0xF] << 16)
				           | ((uint32_t)PAL[L >> 28] << 24);
				*(uint32_t *)(P + 8) = v;
			} else {
				if ((J & 0x80)) P[8]  = PAL(1);
				if ((J & 0x40)) P[9]  = PAL(3);
				if ((J & 0x20)) P[10] = PAL(5);
				if ((J & 0x10)) P[11] = PAL(7);
			}
			// P[12..15] (J bits 3..0)
			if ((J & 0x0F) == 0x0F) {
				uint32_t v = (uint32_t)PAL[L & 0xF]
				           | ((uint32_t)PAL[(L >> 8) & 0xF] << 8)
				           | ((uint32_t)PAL[(L >> 16) & 0xF] << 16)
				           | ((uint32_t)PAL[(L >> 24) & 0xF] << 24);
				*(uint32_t *)(P + 12) = v;
			} else {
				if ((J & 0x08)) P[12] = PAL(0);
				if ((J & 0x04)) P[13] = PAL(2);
				if ((J & 0x02)) P[14] = PAL(4);
				if ((J & 0x01)) P[15] = PAL(6);
			}
		}
	}
}


/*
	Draw sprites between two lines
*/
static void __not_in_flash_func(draw_sprites)(const vdc_ctx_t *ctx, const uint64_t *bitmask_per_line, uint8_t *screen_buffer, int Y1, int Y2, int priority)
{
	TRACE_GFX("Rendering sprites on lines %3d - %3d\tPriority: %d\n", Y1, Y2, priority);

	// NOTE: At this time we do not respect bg sprites priority over top sprites.
	// Example: Assume that sprite #2 is priority=0 and sprite #5 is priority=1. If they
	// overlap then sprite #5 shouldn't be drawn because #2 > #5. But currently it will.

	// Fast pre-filter: union of sprite bitmasks across [Y1, Y2).
	// Per-scanline rendering hits exactly one entry; multi-line ORs a few.
	uint64_t bitmask = 0;
	int y_start = (Y1 < 0) ? 0 : Y1;
	int y_end = (Y2 > XBUF_HEIGHT) ? XBUF_HEIGHT : Y2;
	for (int y = y_start; y < y_end; y++) {
		bitmask |= bitmask_per_line[y];
	}
	if (!bitmask) return;

	// We iterate sprites in reverse order because earlier sprites have
	// higher priority and therefore must overwrite later sprites.

	for (int n = 63; n >= 0; n--) {
		if (!((bitmask >> n) & 1)) continue;

		const sprite_t *spr = &ctx->spram[n];
		uint32_t attr = spr->attr;

		if (((attr >> 7) & 1) != priority) {
			continue;
		}

		int y = (spr->y & 0x3FF) - 64;
		int x = (spr->x & 0x3FF) - 32;
		int cgx = (attr >> 8) & 1;
		int cgy = (attr >> 12) & 3;
		int no = (spr->no & 0x7FF);

		cgy |= cgy >> 1;

		no = (no >> 1) & ~(cgy * 2 + cgx);
		no &= 0x1FF; // PCE has max of 512 sprites

		TRACE_SPR("Sprite 0x%02X : X = %d, Y = %d, attr = %d, no = %d\n", n, x, y, attr, no);

		// Sprite is completely outside our window, skip it
		if (y >= Y2 || y + (cgy + 1) * 16 < Y1 || x >= XBUF_WIDTH || x + (cgx + 1) * 16 < 0) {
			continue;
		}

		cgy *= 16;

		// Sprite-cell base. Vertical cells are 128 uint16_t apart
		// ((yy/16) * 128). Horizontal cells are 64 apart (j * 64).
		const uint16_t *C_base = ctx->vram + (no * 64);

		for (int yy = 0; yy <= cgy; yy += 16) {
			// Screen row of this segment's top (before top-clip).
			int segment_top = (attr & V_FLIP) ? (y + cgy - yy) : (y + yy);
			int t = Y1 - segment_top;
			if (t < 0) t = 0;
			if (t >= 16) continue;   // segment entirely above Y1

			int avail = MIN(16 - t, Y2 - segment_top - t);
			int target = MIN(avail, Y2 - Y1);
			if (target <= 0) continue; // segment entirely below Y2

			int height = target;

			// Recompute P from segment_top each iteration: with per-scanline
			// rendering screen_buffer is a virtual offset, so we cannot rely
			// on P-advancement deltas between iterations — a skipped segment
			// must not leave P pointing into unrelated BSS.
			uint8_t *P = screen_buffer + (segment_top + t) * XBUF_WIDTH + x;

			// Pick the row offset within this cell-pair that draw_sprite needs.
			// Non-V_FLIP reads C[0..height-1] forward, so start at row t.
			// V_FLIP reads C[height-1..0] in reverse, so C[height-1] must be
			// sprite row (15-t) -> start at row (16-t-height).
			const uint16_t *C = C_base + (yy / 16) * 128
			             + ((attr & V_FLIP) ? (16 - t - height) : t);

			for (int j = 0; j <= cgx; j++) {
				draw_sprite(P + (attr & H_FLIP ? cgx - j : j) * 16,
				            C + j * 64, height, attr);
			}
		}
	}
}


// ============================================================================
// SuperGrafx (SGX) renderers
// ----------------------------------------------------------------------------
// Render VDC1 and VDC2 to independent (color, flag) line buffers, then the
// VPC mixer walks the two buffers and produces the final host scanline.
//
// Flag byte per pixel:
//   0x00 = transparent
//   0xFF = drawn (any kind — tile or sprite)
//
// Using 0x00/0xFF lets the VPC mixer use the byte as a bitmask directly
// (branchless: `color & flag | other & ~flag`), and the compiler can
// process 4 pixels at a time via uint32 loads/stores. Profiling showed a
// scalar bit-test (was 0x01) was burning ~4.4 ms/frame in the mixer; this
// encoding cuts that to ~600 us.
//
// Sprite-vs-tile tracking (needed by VPC priority modes 1 and 2) was dropped
// for speed. Both modes are rare in commercial SGX games; if a title misrenders
// because of it, add a second `sgx_spr_vdc{1,2}` mask buffer and consult it
// in the slow path only.
// ============================================================================

#define SGX_PX_DRAWN   0xFF

// Per-scanline scratch (one line at a time — render_lines_sgx loops over lines).
// 4-byte aligned so the mixer can do uint32 loads.
static uint8_t sgx_col_vdc1[XBUF_WIDTH] __attribute__((aligned(4)));
static uint8_t sgx_col_vdc2[XBUF_WIDTH] __attribute__((aligned(4)));
static uint8_t sgx_flg_vdc1[XBUF_WIDTH] __attribute__((aligned(4)));
static uint8_t sgx_flg_vdc2[XBUF_WIDTH] __attribute__((aligned(4)));

// Note: an earlier revision had a 256-entry SGX tile-cell cache here. Measuring
// showed RP2350's hardware QMI/XIP cache (16 KB, 32-byte lines) already absorbs
// repeated tile-cell reads from PSRAM at hardware speed, so a software layer
// just added tag-compare overhead with no benefit. Removed. If we ever need to
// re-introduce one, the access pattern likely worth caching is sprite cells
// (128 bytes each, span multiple XIP lines) or the BAT, not tile cells.

// VDC2 latched scrolling/control context (mirror of gfx_context for VDC1).
static struct {
	int scroll_x;
	int scroll_y;
	int control;
	int latched;
} gfx_context_vdc2;

// VDC2-side equivalent of PCE.ScrollYDiff lives in PCE.VPC.scroll_y_diff_vdc2
// so the VPC port dispatcher in pce.c can update it.

static void __not_in_flash_func(draw_tiles_sgx_line)(const vdc_ctx_t *ctx,
	uint8_t *col, uint8_t *flg, int Y, int scroll_x, int scroll_y)
{
	uint32_t _bg_w[] = { 32, 64, 128, 128 };
	uint32_t _bg_h[] = { 32, 64 };

	uint32_t bg_w = _bg_w[(CTX_REG(ctx, MWR).W >> 4) & 3];
	uint32_t bg_h = _bg_h[(CTX_REG(ctx, MWR).W >> 6) & 1];

	int num_tiles = MIN(CTX_SCREEN_WIDTH(ctx) / 8 + 1, XBUF_WIDTH / 8 + 1);

	int y = Y + scroll_y;
	int row_offset = y & 7;
	int tile_y = (y >> 3) & (bg_h - 1);
	int tile_x = scroll_x / 8;
	int px = -(scroll_x & 7);

	for (int n = 0; n < num_tiles; n++, tile_x++, px += 8) {
		tile_x &= bg_w - 1;

		int no = ctx->vram[tile_x + tile_y * bg_w];
		uint8_t *PAL = &PCE.Palette[(no >> 8) & 0x1F0];
		const uint8_t *C = (const uint8_t *)(ctx->vram + (no & 0x7FF) * 16 + row_offset);

		uint32_t J = C[0] | C[1] | C[16] | C[17];
		if (!J) continue;

		uint32_t M, L;
		M = C[0];  L  = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
		M = C[1];  L |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
		M = C[16]; L |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
		M = C[17]; L |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);

		// Clip the 8-pixel run to the visible scanline window.
		int dst0 = px;
		if (dst0 + 8 <= 0 || dst0 >= XBUF_WIDTH) continue;

		// Whole-tile fully visible? Most common case; lets the 4-pixel fast
		// paths skip per-pixel bounds checks entirely.
		const int run_lo_fits = (dst0 >= 0 && dst0 + 4 <= XBUF_WIDTH);
		const int run_hi_fits = (dst0 + 4 >= 0 && dst0 + 8 <= XBUF_WIDTH);

		#define EMIT(off, n_) do { \
			int _dx = dst0 + (off); \
			if ((unsigned)_dx < XBUF_WIDTH) { \
				col[_dx] = PAL[(L >> ((n_) * 4)) & 15]; \
				flg[_dx] = SGX_PX_DRAWN; \
			} \
		} while (0)

		// Run A (pixels 0..3) — fast path when fully visible and fully opaque.
		if (run_lo_fits && (J & 0xF0) == 0xF0) {
			uint32_t v = (uint32_t)PAL[(L >>  4) & 0xF]
			           | ((uint32_t)PAL[(L >> 12) & 0xF] << 8)
			           | ((uint32_t)PAL[(L >> 20) & 0xF] << 16)
			           | ((uint32_t)PAL[(L >> 28) & 0xF] << 24);
			*(uint32_t *)(col + dst0)     = v;
			*(uint32_t *)(flg + dst0)     = 0xFFFFFFFFu;
		} else {
			if (J & 0x80) EMIT(0, 1);
			if (J & 0x40) EMIT(1, 3);
			if (J & 0x20) EMIT(2, 5);
			if (J & 0x10) EMIT(3, 7);
		}

		// Run B (pixels 4..7).
		if (run_hi_fits && (J & 0x0F) == 0x0F) {
			uint32_t v = (uint32_t)PAL[ L        & 0xF]
			           | ((uint32_t)PAL[(L >>  8) & 0xF] << 8)
			           | ((uint32_t)PAL[(L >> 16) & 0xF] << 16)
			           | ((uint32_t)PAL[(L >> 24) & 0xF] << 24);
			*(uint32_t *)(col + dst0 + 4) = v;
			*(uint32_t *)(flg + dst0 + 4) = 0xFFFFFFFFu;
		} else {
			if (J & 0x08) EMIT(4, 0);
			if (J & 0x04) EMIT(5, 2);
			if (J & 0x02) EMIT(6, 4);
			if (J & 0x01) EMIT(7, 6);
		}

		#undef EMIT
	}
}

// Render a single sprite cell (up to 16 wide, `height` rows) into the (col, flg)
// line buffer starting at horizontal offset 'x'. 'C' is the source row (16-bit
// planes). Used for VDC2 — VDC1 still goes through the original draw_sprite()
// when SGX is off.
static void __not_in_flash_func(draw_sprite_sgx_cell)(uint8_t *col, uint8_t *flg,
	int x, const uint16_t *C, uint32_t attr, uint8_t pal_flag)
{
	uint8_t *PAL = &PCE.Palette[256 + ((attr & 0xF) << 4)];
	bool hflip = attr & H_FLIP;

	uint32_t J = C[0] | C[16] | C[32] | C[48];
	if (!J) return;

	uint32_t M, L1, L2;
	M = C[0];  L1 = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
	           L2 = ((M & 0x8800) >> 11) | ((M & 0x4400) >> 2) | ((M & 0x2200) << 7) | ((M & 0x1100) << 16);
	M = C[16]; L1 |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
	           L2 |= ((M & 0x8800) >> 10) | ((M & 0x4400) >> 1) | ((M & 0x2200) << 8) | ((M & 0x1100) << 17);
	M = C[32]; L1 |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
	           L2 |= ((M & 0x8800) >> 9) | ((M & 0x4400) >> 0) | ((M & 0x2200) << 9) | ((M & 0x1100) << 18);
	M = C[48]; L1 |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);
	           L2 |= ((M & 0x8800) >> 8) | ((M & 0x4400) << 1) | ((M & 0x2200) << 10) | ((M & 0x1100) << 19);

	// 16 pixels laid out as J bits 15..0 (left to right when !hflip).
	// Same nibble extraction pattern as draw_sprite's slow path, but with the
	// addition of a 4-pixel uint32 fast path per quadrant when all 4 pixels of
	// the quadrant are opaque AND the quadrant fully fits in the scanline.
	const uint32_t flg4 = pal_flag * 0x01010101u;

	#define EMIT(dst_off, src_bit, L_, n_) do { \
		if (J & (1U << (src_bit))) { \
			int _dx = x + (dst_off); \
			if ((unsigned)_dx < XBUF_WIDTH) { \
				col[_dx] = PAL[(L_ >> ((n_) * 4)) & 15]; \
				flg[_dx] = pal_flag; \
			} \
		} \
	} while (0)

	// QUAD: try fast path (all 4 pixels opaque AND quadrant fully visible),
	// else fall back to four EMITs. j_mask is the contiguous nibble of J for
	// the quadrant; n0..n3 are the L nibble indices for the 4 pixels.
	#define QUAD(quad_off, j_mask, L_, n0, n1, n2, n3, b0, b1, b2, b3, np0, np1, np2, np3) do { \
		int _dx = x + (quad_off); \
		if ((J & (j_mask)) == (j_mask) && _dx >= 0 && _dx + 4 <= XBUF_WIDTH) { \
			uint32_t _v = (uint32_t)PAL[((L_) >> ((n0) * 4)) & 15] \
			            | ((uint32_t)PAL[((L_) >> ((n1) * 4)) & 15] << 8) \
			            | ((uint32_t)PAL[((L_) >> ((n2) * 4)) & 15] << 16) \
			            | ((uint32_t)PAL[((L_) >> ((n3) * 4)) & 15] << 24); \
			*(uint32_t *)(col + _dx) = _v; \
			*(uint32_t *)(flg + _dx) = flg4; \
		} else { \
			EMIT((quad_off) + 0, b0, L_, np0); \
			EMIT((quad_off) + 1, b1, L_, np1); \
			EMIT((quad_off) + 2, b2, L_, np2); \
			EMIT((quad_off) + 3, b3, L_, np3); \
		} \
	} while (0)

	if (hflip) {
		QUAD( 0, 0x000F, L1, 6, 4, 2, 0,   0,  1,  2,  3,  6, 4, 2, 0);
		QUAD( 4, 0x00F0, L1, 7, 5, 3, 1,   4,  5,  6,  7,  7, 5, 3, 1);
		QUAD( 8, 0x0F00, L2, 6, 4, 2, 0,   8,  9, 10, 11,  6, 4, 2, 0);
		QUAD(12, 0xF000, L2, 7, 5, 3, 1,  12, 13, 14, 15,  7, 5, 3, 1);
	} else {
		QUAD( 0, 0xF000, L2, 1, 3, 5, 7,  15, 14, 13, 12,  1, 3, 5, 7);
		QUAD( 4, 0x0F00, L2, 0, 2, 4, 6,  11, 10,  9,  8,  0, 2, 4, 6);
		QUAD( 8, 0x00F0, L1, 1, 3, 5, 7,   7,  6,  5,  4,  1, 3, 5, 7);
		QUAD(12, 0x000F, L1, 0, 2, 4, 6,   3,  2,  1,  0,  0, 2, 4, 6);
	}

	#undef QUAD
	#undef EMIT
}

static void __not_in_flash_func(draw_sprites_sgx_line)(const vdc_ctx_t *ctx,
	const uint64_t *bitmask_per_line, uint8_t *col, uint8_t *flg, int Y, int priority)
{
	if (Y < 0 || Y >= XBUF_HEIGHT) return;
	uint64_t bitmask = bitmask_per_line[Y];
	if (!bitmask) return;

	for (int n = 63; n >= 0; n--) {
		if (!((bitmask >> n) & 1)) continue;
		const sprite_t *spr = &ctx->spram[n];
		uint32_t attr = spr->attr;
		if (((attr >> 7) & 1) != priority) continue;

		int y = (spr->y & 0x3FF) - 64;
		int x = (spr->x & 0x3FF) - 32;
		int cgx = (attr >> 8) & 1;
		int cgy = (attr >> 12) & 3;
		int no = (spr->no & 0x7FF);

		cgy |= cgy >> 1;
		no = (no >> 1) & ~(cgy * 2 + cgx);
		no &= 0x1FF;

		if (y >= Y + 1 || y + (cgy + 1) * 16 < Y || x >= XBUF_WIDTH || x + (cgx + 1) * 16 < 0) continue;

		int total_h = (cgy + 1) * 16;
		int sprite_row = (attr & V_FLIP) ? (total_h - 1 - (Y - y)) : (Y - y);
		if (sprite_row < 0 || sprite_row >= total_h) continue;

		int cell_row_y = sprite_row >> 4;             // which 16-tall cell band
		int row_within_cell = sprite_row & 15;        // 0..15

		const uint16_t *C_base = ctx->vram + (no * 64);
		const uint16_t *C_row  = C_base + cell_row_y * 128 + row_within_cell;

		for (int j = 0; j <= cgx; j++) {
			int cell_x = (attr & H_FLIP) ? (cgx - j) : j;
			draw_sprite_sgx_cell(col, flg, x + cell_x * 16, C_row + j * 64, attr, SGX_PX_DRAWN);
		}
	}
}

// Vectorized mix of one contiguous segment [start, end) under a single VPC cfg.
// Buffers are 4-byte aligned; the segment head/tail handle pixels not on a
// uint32 boundary (only when a VPC window splits mid-word — rare but legal).
static inline void __not_in_flash_func(mix_segment)(uint8_t *out, int start, int end,
	uint8_t cfg, uint32_t bd_4, uint8_t backdrop)
{
	const int v1_en = cfg & 0x01;
	const int v2_en = cfg & 0x02;
	const int width = end - start;
	if (width <= 0) return;

	if (!v1_en && !v2_en) {
		memset(out + start, backdrop, width);
		return;
	}

	// Compute the aligned middle range. Head = pixels before next uint32
	// boundary; tail = pixels after last uint32 boundary.
	int aligned_start = (start + 3) & ~3;
	int aligned_end   = end & ~3;
	if (aligned_start > end) aligned_start = end;
	if (aligned_end < aligned_start) aligned_end = aligned_start;

	#define SCALAR_PX(i_) do { \
		uint8_t f1 = sgx_flg_vdc1[(i_)]; \
		uint8_t f2 = sgx_flg_vdc2[(i_)]; \
		uint8_t r; \
		if (v1_en && f1)        r = sgx_col_vdc1[(i_)]; \
		else if (v2_en && f2)   r = sgx_col_vdc2[(i_)]; \
		else                    r = backdrop; \
		out[(i_)] = r; \
	} while (0)

	for (int i = start; i < aligned_start; i++) SCALAR_PX(i);

	if (v1_en && v2_en) {
		for (int i = aligned_start; i < aligned_end; i += 4) {
			uint32_t f1 = *(uint32_t *)(sgx_flg_vdc1 + i);
			uint32_t c1 = *(uint32_t *)(sgx_col_vdc1 + i);
			uint32_t f2 = *(uint32_t *)(sgx_flg_vdc2 + i);
			uint32_t c2 = *(uint32_t *)(sgx_col_vdc2 + i);
			uint32_t pick2 = (c2 & f2) | (~f2 & bd_4);
			*(uint32_t *)(out + i) = (c1 & f1) | (~f1 & pick2);
		}
	} else if (v1_en) {
		for (int i = aligned_start; i < aligned_end; i += 4) {
			uint32_t f1 = *(uint32_t *)(sgx_flg_vdc1 + i);
			uint32_t c1 = *(uint32_t *)(sgx_col_vdc1 + i);
			*(uint32_t *)(out + i) = (c1 & f1) | (~f1 & bd_4);
		}
	} else {
		for (int i = aligned_start; i < aligned_end; i += 4) {
			uint32_t f2 = *(uint32_t *)(sgx_flg_vdc2 + i);
			uint32_t c2 = *(uint32_t *)(sgx_col_vdc2 + i);
			*(uint32_t *)(out + i) = (c2 & f2) | (~f2 & bd_4);
		}
	}

	for (int i = aligned_end; i < end; i++) SCALAR_PX(i);
	#undef SCALAR_PX
}

// VPC per-pixel mixer — Mesen2's PceVpc::ProcessScanline.
// window_cfg[region] packs: bit0=Vdc1Enabled, bit1=Vdc2Enabled, bits2-3=PriorityMode.
// region index: 0=NoWindow, 1=Window1, 2=Window2, 3=Both.
//
// Strategy: split the scanline at w1/w2 boundaries into at most 3 contiguous
// segments. Each segment sees a single cfg and runs the vectorized fast path
// (uint32 byte-mask). Sprite-vs-tile tracking was dropped: priority modes 1
// and 2 collapse to default (mode 0).
static void __not_in_flash_func(mix_vpc_scanline)(uint8_t *out, int width)
{
	const uint8_t  backdrop = PCE.Palette[0];
	const uint32_t bd_4 = 0x01010101u * backdrop;
	uint16_t w1 = PCE.VPC.window1;
	uint16_t w2 = PCE.VPC.window2;

	// No windows: one segment covers everything.
	if (w1 == 0 && w2 == 0) {
		mix_segment(out, 0, width, PCE.VPC.window_cfg[0], bd_4, backdrop);
		return;
	}

	// region(x) = (x < w1 ? 1 : 0) | (x < w2 ? 2 : 0)
	// Segments by sorted boundaries: [0, xa), [xa, xb), [xb, width).
	int xa, xb, mid_region;
	if (w1 < w2) { xa = w1; xb = w2; mid_region = 2; }    // 1 -> 0 at w1
	else         { xa = w2; xb = w1; mid_region = 1; }    // 2 -> 0 at w2
	if (xa > width) xa = width;
	if (xb > width) xb = width;

	if (xa > 0)
		mix_segment(out, 0,  xa,    PCE.VPC.window_cfg[3], bd_4, backdrop);
	if (xb > xa)
		mix_segment(out, xa, xb,    PCE.VPC.window_cfg[mid_region], bd_4, backdrop);
	if (width > xb)
		mix_segment(out, xb, width, PCE.VPC.window_cfg[0], bd_4, backdrop);
}

static void __not_in_flash_func(render_lines_sgx)(int min_line, int max_line)
{
	uint8_t *screen_buffer = osd_gfx_framebuffer(PCE.VDC.screen_width, PCE.VDC.screen_height);
	if (!screen_buffer) return;
	framebuffer_top = screen_buffer - 16;
	framebuffer_bottom = screen_buffer + PCE.VDC.screen_height * XBUF_WIDTH;

	vdc_ctx_t vdc2_ctx = {
		.regs  = PCE.VDC2.regs,
		.vram  = PCE.VRAM2,
		.spram = PCE.SPRAM2,
	};

	// Visible width — most PCE modes render 256 or 352 pixels; mixing the full
	// XBUF_WIDTH (368) wastes ~15-44% of the mixer per scanline.
	int mix_width = (int)PCE.VDC.screen_width;
	if (mix_width > XBUF_WIDTH) mix_width = XBUF_WIDTH;

	int vdc1_active = gfx_context.control & 0xC0;
	int vdc2_active = gfx_context_vdc2.control & 0xC0;

	for (int y = min_line; y < max_line; y++) {
#if SGX_PROFILE
		uint32_t t0 = time_us_32();
#endif
		memset(sgx_flg_vdc1, 0, mix_width);
		memset(sgx_flg_vdc2, 0, mix_width);
#if SGX_PROFILE
		uint32_t t1 = time_us_32();
#endif

		// VDC1
		if (vdc1_active & 0x40)
			draw_sprites_sgx_line(VDC1_CTX, sprite_bitmask_per_line, sgx_col_vdc1, sgx_flg_vdc1, y, 0);
		if (vdc1_active & 0x80)
			draw_tiles_sgx_line(VDC1_CTX, sgx_col_vdc1, sgx_flg_vdc1, y, gfx_context.scroll_x, gfx_context.scroll_y);
		if (vdc1_active & 0x40)
			draw_sprites_sgx_line(VDC1_CTX, sprite_bitmask_per_line, sgx_col_vdc1, sgx_flg_vdc1, y, 1);
#if SGX_PROFILE
		uint32_t t2 = time_us_32();
#endif

		// VDC2 — completely skip when CR has both background and sprite disabled.
		if (vdc2_active) {
			if (vdc2_active & 0x40)
				draw_sprites_sgx_line(&vdc2_ctx, sprite_bitmask_per_line_vdc2, sgx_col_vdc2, sgx_flg_vdc2, y, 0);
			if (vdc2_active & 0x80)
				draw_tiles_sgx_line(&vdc2_ctx, sgx_col_vdc2, sgx_flg_vdc2, y, gfx_context_vdc2.scroll_x, gfx_context_vdc2.scroll_y);
			if (vdc2_active & 0x40)
				draw_sprites_sgx_line(&vdc2_ctx, sprite_bitmask_per_line_vdc2, sgx_col_vdc2, sgx_flg_vdc2, y, 1);
		}
#if SGX_PROFILE
		uint32_t t3 = time_us_32();
#endif

		mix_vpc_scanline(screen_buffer + y * XBUF_WIDTH, mix_width);
#if SGX_PROFILE
		uint32_t t4 = time_us_32();
		sgx_prof_clear_us += (t1 - t0);
		sgx_prof_vdc1_us  += (t2 - t1);
		sgx_prof_vdc2_us  += (t3 - t2);
		sgx_prof_mix_us   += (t4 - t3);
		sgx_prof_total_us += (t4 - t0);
		sgx_prof_lines++;
#endif
	}

	osd_gfx_lines_rendered(min_line, max_line);
}

/*
	Hit Check Sprite#0 and others
*/
static inline bool
sprite_hit_check(const vdc_ctx_t *ctx)
{
	const sprite_t *spr = &ctx->spram[0];
	int x0 = spr->x;
	int y0 = spr->y;
	int w0 = (((spr->attr >> 8) & 1) + 1) * 16;
	int h0 = (((spr->attr >> 12) & 3) + 1) * 16;

	spr++;

	for (int i = 1; i < 64; i++, spr++) {
		int x = spr->x;
		int y = spr->y;
		int w = (((spr->attr >> 8) & 1) + 1) * 16;
		int h = (((spr->attr >> 12) & 3) + 1) * 16;
		if ((x < x0 + w0) && (x + w > x0) && (y < y0 + h0) && (y + h > y0))
			return 1;
	}
	return 0;
}


void __not_in_flash_func(gfx_latch_context)(int force)
{
	if (!gfx_context.latched || force) { // Context is already saved + we haven't render the line using it
		gfx_context.scroll_x = IO_VDC_REG[BXR].W;
		gfx_context.scroll_y = IO_VDC_REG[BYR].W - PCE.ScrollYDiff;
		gfx_context.control = IO_VDC_REG[CR].W;
		gfx_context.latched = 1;
	}
}

// Parallel latch for VDC2 — invoked from the VPC dispatcher when VDC2's CR/BXR/BYR
// is written, and from gfx_run() at the start of each visible frame.
void __not_in_flash_func(gfx_latch_context_vdc2)(int force)
{
	if (!PCE.VPC.is_sgx) return;
	if (!gfx_context_vdc2.latched || force) {
		gfx_context_vdc2.scroll_x = PCE.VDC2.regs[BXR].W;
		gfx_context_vdc2.scroll_y = PCE.VDC2.regs[BYR].W - PCE.VPC.scroll_y_diff_vdc2;
		gfx_context_vdc2.control  = PCE.VDC2.regs[CR].W;
		gfx_context_vdc2.latched  = 1;
	}
}


/*
	Render lines into the buffer from min_line to max_line (inclusive)
*/
static bool gfx_skip_render = false;

void gfx_set_skip_render(bool skip)
{
	gfx_skip_render = skip;
}

static inline void
render_lines(int min_line, int max_line)
{
	gfx_context.latched = 0;
	if (PCE.VPC.is_sgx) gfx_context_vdc2.latched = 0;

	// Frame-skip: keep the latch bookkeeping above, but drop the expensive
	// rasterization + host line conversion. VDC state, IRQs and sprite-0
	// collision (all in gfx_run) are unaffected, so game logic is intact.
	if (gfx_skip_render)
		return;

	if (PCE.VPC.is_sgx) {
		render_lines_sgx(min_line, max_line);
		return;
	}

	uint8_t *screen_buffer = osd_gfx_framebuffer(PCE.VDC.screen_width, PCE.VDC.screen_height);
	if (!screen_buffer) {
		return;
	}

	// Assume 16 columns of scratch area around our buffer.
	framebuffer_top = screen_buffer - 16;
	framebuffer_bottom = screen_buffer + PCE.VDC.screen_height * XBUF_WIDTH;

	// We must fill the region with color 0 first.
	// Clamp to XBUF_WIDTH: wide modes (up to 512px) would overflow the single-line buffer.
	// We only display 320px, so rendering up to XBUF_WIDTH (368) covers everything visible.
	size_t screen_width = MIN(IO_VDC_SCREEN_WIDTH, XBUF_WIDTH);
	for (int y = min_line; y < max_line; y++) {
		memset(screen_buffer + (y * XBUF_WIDTH), PCE.Palette[0], screen_width);
	}

	// Sprites with priority 0 are drawn behind the tiles
	if (gfx_context.control & 0x40) {
		draw_sprites(VDC1_CTX, sprite_bitmask_per_line, screen_buffer, min_line, max_line, 0);
	}

	// Draw the background tiles
	if (gfx_context.control & 0x80) {
		draw_tiles(VDC1_CTX, screen_buffer, min_line, max_line, gfx_context.scroll_x, gfx_context.scroll_y);
	}

	// Draw regular sprites
	if (gfx_context.control & 0x40) {
		draw_sprites(VDC1_CTX, sprite_bitmask_per_line, screen_buffer, min_line, max_line, 1);
	}

	osd_gfx_lines_rendered(min_line, max_line);
}


int
gfx_init(void)
{
	gfx_reset(true);
	return 0;
}


void
gfx_reset(bool hard)
{
	last_line_counter = 0;
	line_counter = 0;
}


void
gfx_term(void)
{
	//
}


/*
	Raises a VDC IRQ and/or process pending VDC IRQs.
	More than one interrupt can happen in a single line on real hardware and the cpu
	would usually receive them one by one. We use an uint32 as a 8 slot buffer.
*/
void __not_in_flash_func(gfx_irq)(int type)
{
	/* If IRQ, push it on the stack */
	if (type >= 0) {
		PCE.VDC.pending_irqs <<= 4;
		PCE.VDC.pending_irqs |= type & 0xF;
	}

	/* Pop the first pending vdc interrupt only if CPU.irq_lines is clear */
	int pos = 28;
	while (!(CPU.irq_lines & INT_IRQ1) && PCE.VDC.pending_irqs) {
		if (PCE.VDC.pending_irqs >> pos) {
			PCE.VDC.status |= 1 << (PCE.VDC.pending_irqs >> pos);
			PCE.VDC.pending_irqs &= ~(0xF << pos);
			CPU.irq_lines |= INT_IRQ1; // Notify the CPU
		}
		pos -= 4;
	}

	// SuperGrafx: VDC2 shares the same IRQ1 line. Drain its independent stack
	// the same way. CPU disambiguates VDC1/VDC2 sources by reading both status
	// registers ($1FE000 and $1FE010).
	if (PCE.VPC.is_sgx) {
		pos = 28;
		while (!(CPU.irq_lines & INT_IRQ1) && PCE.VDC2.pending_irqs) {
			if (PCE.VDC2.pending_irqs >> pos) {
				PCE.VDC2.status |= 1 << (PCE.VDC2.pending_irqs >> pos);
				PCE.VDC2.pending_irqs &= ~(0xF << pos);
				CPU.irq_lines |= INT_IRQ1;
			}
			pos -= 4;
		}
	}
}


/*
	Process one scanline
*/
void __not_in_flash_func(gfx_run)(void)
{
	int scanline = PCE.Scanline;

	/* DMA Transfer in "progress" */
	if (PCE.VDC.satb > DMA_TRANSFER_COUNTER) {
		if (--PCE.VDC.satb == DMA_TRANSFER_COUNTER) {
			if (SATBIntON) {
				gfx_irq(VDC_STAT_DS);
			}
		}
	}
	if (PCE.VPC.is_sgx && PCE.VDC2.satb > DMA_TRANSFER_COUNTER) {
		// VDC2 SATB-DMA timing mirror. IRQ merge into IRQ1 lands in step 7.
		if (--PCE.VDC2.satb == DMA_TRANSFER_COUNTER) {
			if (PCE.VDC2.regs[DCR].W & 0x01) {
				// Raise via VDC2's pending stack; step 7 ORs into IRQ1.
				PCE.VDC2.pending_irqs <<= 4;
				PCE.VDC2.pending_irqs |= VDC_STAT_DS & 0xF;
			}
		}
	}

	/* Test raster hit */
	if (RasHitON) {
		if (IO_VDC_REG[RCR].W >= 0x40 && (IO_VDC_REG[RCR].W <= 0x146)) {
			uint16_t temp_rcr = (uint16_t)(IO_VDC_REG[RCR].W - 0x40);
			if (scanline == (temp_rcr + IO_VDC_MINLINE) % 263) {
				TRACE_GFX("\n-----------------RASTER HIT (%d)------------------\n", scanline);
				gfx_irq(VDC_STAT_RR);
			}
		}
	}
	if (PCE.VPC.is_sgx && (PCE.VDC2.regs[CR].W & 0x04)) {
		// VDC2 raster hit. Pending stack only; step 7 OR-merges to IRQ1.
		if (PCE.VDC2.regs[RCR].W >= 0x40 && PCE.VDC2.regs[RCR].W <= 0x146) {
			uint16_t temp_rcr2 = (uint16_t)(PCE.VDC2.regs[RCR].W - 0x40);
			int vdc2_minline = PCE.VDC2.regs[VPR].B.h + PCE.VDC2.regs[VPR].B.l;
			if (scanline == (temp_rcr2 + vdc2_minline) % 263) {
				PCE.VDC2.pending_irqs <<= 4;
				PCE.VDC2.pending_irqs |= VDC_STAT_RR & 0xF;
			}
		}
	}

	/* Visible area */
	if (scanline >= 14 && scanline <= 255) {
		if (scanline == IO_VDC_MINLINE) {
			gfx_latch_context(1);
			build_sprite_lists(VDC1_CTX, sprite_bitmask_per_line);
			if (PCE.VPC.is_sgx) {
				vdc_ctx_t v2 = { PCE.VDC2.regs, PCE.VRAM2, PCE.SPRAM2 };
				gfx_latch_context_vdc2(1);
				build_sprite_lists(&v2, sprite_bitmask_per_line_vdc2);
			}
		}

		if (scanline >= IO_VDC_MINLINE && scanline <= IO_VDC_MAXLINE) {
			if (gfx_context.latched) {
				if (last_line_counter < line_counter) {
					osd_gfx_render_line = last_line_counter;
					render_lines(last_line_counter, line_counter);
				}
				last_line_counter = line_counter;
			}
			gfx_latch_context(1);
			osd_gfx_render_line = line_counter;
			render_lines(line_counter, line_counter + 1);
			line_counter++;
			last_line_counter = line_counter;
		}
	}
	/* V Blank trigger line */
	else if (scanline == 256) {
		if (last_line_counter < line_counter) {
			gfx_latch_context(0);
			osd_gfx_render_line = last_line_counter;
			render_lines(last_line_counter, line_counter);
		}
#if SGX_PROFILE
		if (PCE.VPC.is_sgx) {
			sgx_prof_frames++;
			sgx_prof_dump_if_due();
		}
#endif

		// Trigger interrupts
		if (SpHitON && sprite_hit_check(VDC1_CTX)) {
			gfx_irq(VDC_STAT_CR);
		}
		// VBlank status bit is always set (hardware flag) — the BIOS
		// polls $0000 for VBlank with IRQs disabled during screen
		// transitions. gfx_irq() alone won't set the bit when IRQ1
		// is already pending, so set it directly too.
		PCE.VDC.status |= 1 << VDC_STAT_VD;
		if (VBlankON) {
			gfx_irq(VDC_STAT_VD);
		}

		/* VRAM to SATB DMA */
		if (PCE.VDC.satb == DMA_TRANSFER_PENDING || AutoSATBON) {
			memcpy(PCE.SPRAM, PCE.VRAM + IO_VDC_REG[SATB].W, 512);
			PCE.VDC.satb = DMA_TRANSFER_COUNTER + 4;
		}

		if (PCE.VPC.is_sgx) {
			PCE.VDC2.status |= 1 << VDC_STAT_VD;
			if (PCE.VDC2.regs[CR].W & 0x08) {
				PCE.VDC2.pending_irqs <<= 4;
				PCE.VDC2.pending_irqs |= VDC_STAT_VD & 0xF;
			}
			int auto_satb2 = PCE.VDC2.regs[DCR].W & 0x10;
			if (PCE.VDC2.satb == DMA_TRANSFER_PENDING || auto_satb2) {
				memcpy(PCE.SPRAM2, PCE.VRAM2 + PCE.VDC2.regs[SATB].W, 512);
				PCE.VDC2.satb = DMA_TRANSFER_COUNTER + 4;
			}
		}
	}
	/* V Blank area */
	else {
		gfx_context.latched = 0;
		last_line_counter = 0;
		line_counter = 0;
		PCE.ScrollYDiff = 0;
	}

	/* Always call at least once (to handle pending IRQs) */
	gfx_irq(-1);
}
