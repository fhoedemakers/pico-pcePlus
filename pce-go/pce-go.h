#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_PRINTF, NULL, x)
#define crc32_le(a, b, c) rg_crc32(a, b, c)
#else
#define LOG_PRINTF(level, x...) printf(x)
#define crc32_le(a, b, c) (0)
#endif

#define MESSAGE_ERROR(x...) LOG_PRINTF(1, "!! " x)
#define MESSAGE_WARN(x...)  LOG_PRINTF(2, " ! " x)
#define MESSAGE_INFO(x...)  LOG_PRINTF(3, " * " x)
#define MESSAGE_TRACE(tag, x...) LOG_PRINTF(4, " & (" tag ") " x)
#if ENABLE_DEBUG
#define MESSAGE_DEBUG(x...) LOG_PRINTF(4, " > " x)
#else
#define MESSAGE_DEBUG(x...) {}
#endif

#if ENABLE_SPR_TRACING
#define TRACE_SPR(x...) MESSAGE_TRACE("SPR", x)
#else
#define TRACE_SPR(x...) {}
#endif

#if ENABLE_GFX_TRACING
#define TRACE_GFX(x...) MESSAGE_TRACE("GFX", x)
#else
#define TRACE_GFX(x...) {}
#endif

#if ENABLE_IO_TRACING
#define TRACE_IO(x...) MESSAGE_TRACE("IO", x)
#else
#define TRACE_IO(x...) {}
#endif

#if ENABLE_CPU_TRACING
#define TRACE_CPU(x...) MESSAGE_TRACE("CPU", x)
#else
#define TRACE_CPU(x...) {}
#endif

#undef MIN
#define MIN(a,b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a < _b ? _a : _b; })
#undef MAX
#define MAX(a,b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a > _b ? _a : _b; })

#define JOY_A       0x01
#define JOY_B       0x02
#define JOY_SELECT  0x04
#define JOY_RUN     0x08
#define JOY_UP      0x10
#define JOY_RIGHT   0x20
#define JOY_DOWN    0x40
#define JOY_LEFT    0x80

// We need 16 bytes of scratch area on both side of each line. The 16 bytes can be shared by adjacent lines.
// The buffer should look like [16 bytes] [line 1] [16 bytes] ... [16 bytes] [line 242] [16 bytes]
#define XBUF_WIDTH 	(352 + 16)
#define	XBUF_HEIGHT	(242)

int LoadState(const char *name);
int SaveState(const char *name);
void ResetPCE(bool);
void RunPCE(void);
void ShutdownPCE();
int InitPCE(int samplerate, bool stereo);
// SuperGrafx mode: must be set BEFORE InitPCE so the emulator can allocate the
// second VRAM and expand work RAM to 32 KB. Pass true for .sgx ROMs, false
// otherwise. The PCE/CD path is unchanged when this is false.
void SetSgxModePCE(bool is_sgx);
int LoadCard(uint8_t *data, size_t size);
int LoadFile(const char *name);
// Load a PC Engine CD-ROM game. Scans /bios/ for a System Card / Arcade Card
// BIOS, loads it into PSRAM, sets up CD/SCD/ADPCM/Arcade Card RAM, parses the
// CUE sheet and opens the BIN file. Requires PSRAM. Returns 0 on success.
int LoadDisc(const char *cue_path);
void *PalettePCE(int bitdepth);

typedef struct __attribute__((packed))
{
	char key[12];
	uint32_t type:8;
	uint32_t len:24;
} block_hdr_t;

typedef const struct
{
	block_hdr_t desc;
	void *ptr;
} save_var_t;

extern const char SAVESTATE_HEADER[8];
extern save_var_t SaveStateVars[];

extern int osd_gfx_render_line;

extern uint8_t *osd_gfx_framebuffer(int width, int height);
extern void osd_gfx_lines_rendered(int first_line, int last_line);
extern void osd_input_read(uint8_t joypads[8]);
extern void osd_vsync(void);
extern void osd_psg_scanline(void);
// Sample-accurate PSG: called from pce_writeIO before applying a PSG register
// write, with the current frame-relative CPU cycle position. Lets the host
// generate audio up to that exact point so transient register changes (short
// SFX, DDA) are captured at their real timing instead of once per scanline batch.
extern void osd_psg_sync(int frame_cycle);
