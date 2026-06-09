#pragma once

#include <stdbool.h>

int gfx_init(void);
void gfx_run(void);
void gfx_term(void);
void gfx_irq(int type);
void gfx_reset(bool hard);
void gfx_latch_context(int force);
void gfx_latch_context_vdc2(int force);
void gfx_bind_vdc1(void);

// Frame-skip: when set, render_lines() skips rasterization and the host
// line callback for the current frame (CPU/VDC/IRQ/collision logic still
// runs). The framebuffer keeps the previous image. Used to hold 60fps
// audio through render-bound scenes by dropping only the visible update.
void gfx_set_skip_render(bool skip);
