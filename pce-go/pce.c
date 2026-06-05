// pce.c - Machine emulation (Memory/IO/Timer)
//
#include <stdlib.h>
#include <string.h>
#include "pico.h"
#include "pce-go.h"
#include "pce.h"
#include "gfx.h"
#include "cd.h"

// PSRAM-aware allocator. VRAM2 is too large to keep in the SRAM heap alongside
// the 150 KB framebuffer + everything else (causes OOM). We tried putting it in
// a static SRAM array; that consumed enough BSS to leave the sbrk heap with too
// little room for the transient FATFS/USB/libc allocations PCE games depend on.
// PSRAM is the only place left — VDC2 rendering pays a latency cost (~slower
// per-pixel fetches) but the game runs.
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

// Global struct containing our emulated hardware status
PCE_t PCE;

// Memory Mapping
uint8_t *PageR[8];
uint8_t *PageW[8];

static inline void timer_run(int cycles);

/**
  * Reset the hardware
  **/
void
pce_reset(bool hard)
{
	memset(&PCE.VCE, 0, sizeof(PCE.VCE));
	memset(&PCE.VDC, 0, sizeof(PCE.VDC));
	memset(&PCE.PSG, 0, sizeof(PCE.PSG));
	memset(&PCE.Timer, 0, sizeof(PCE.Timer));

	if (PCE.VPC.is_sgx) {
		memset(&PCE.VDC2, 0, sizeof(PCE.VDC2));
		// Keep VPC.is_sgx (set before InitPCE); clear the rest.
		uint8_t was_sgx = PCE.VPC.is_sgx;
		memset(&PCE.VPC, 0, sizeof(PCE.VPC));
		PCE.VPC.is_sgx = was_sgx;
	}

	if (hard) {
		memset(PCE.RAM, 0, PCE.VPC.is_sgx ? 0x8000 : 0x2000);
		memset(PCE.VRAM, 0, 0x10000);
		if (PCE.VPC.is_sgx && PCE.VRAM2) {
			memset(PCE.VRAM2, 0, 0x10000);
		}
		memset(PCE.SPRAM, 0, 512);
		if (PCE.VPC.is_sgx) {
			memset(PCE.SPRAM2, 0, 512);
		}
		memset(PCE.Palette, 0, 512);
		memset(PCE.NULLRAM, 0xFF, 0x2000);
	}

	IO_VDC_REG[VPR].B.h = 0x0f;
	IO_VDC_REG[VPR].B.l = 0x02;

	if (PCE.VPC.is_sgx) {
		PCE.VDC2.regs[VPR].B.h = 0x0f;
		PCE.VDC2.regs[VPR].B.l = 0x02;
	}

	PCE.SF2 = 0;
	PCE.Timer.cycles_per_line = 113;
	PCE.Cycles = 0;

	// Reset sound generator values
	for (int i = 0; i < PSG_CHANNELS; i++) {
		PCE.PSG.chan[i].control = 0x80;
	}
	// Seed the noise LFSRs (ch 4/5). A zero LFSR is a stuck state (XOR of
	// zeros stays zero) → silent noise, so it must be non-zero after the
	// PSG memset above. Mesen2 inits the 18-bit LFSR to 1.
	PCE.PSG.chan[4].noise_rand = 1;
	PCE.PSG.chan[5].noise_rand = 1;

	// Reset memory banking
	pce_bank_set(7, 0x00);
	pce_bank_set(6, 0x05);
	pce_bank_set(5, 0x04);
	pce_bank_set(4, 0x03);
	pce_bank_set(3, 0x02);
	pce_bank_set(2, 0x01);
	pce_bank_set(1, 0xF8);
	pce_bank_set(0, 0xFF);

	// Reset CD subsystem (no-op when no disc is attached)
	cd_reset();

	// Reset CPU
	h6280_reset();
}


/**
  * Initialize the hardware
  **/
int
pce_init(void)
{
	// Work RAM: 8 KB on PC Engine, 32 KB on SuperGrafx (mapped at banks F8-FB).
	size_t ram_size = PCE.VPC.is_sgx ? 0x8000 : 0x2000;
	PCE.RAM = malloc(ram_size);
	PCE.VRAM = malloc(0x10000);
	if (PCE.VPC.is_sgx) {
		PCE.VRAM2 = frens_f_malloc(0x10000);
	}
	PCE.NULLRAM = malloc(0x2000);
	PCE.IOAREA = PCE.NULLRAM + 4;
	PCE.MemoryMapR = calloc(256, sizeof(uint8_t *));
	PCE.MemoryMapW = calloc(256, sizeof(uint8_t *));

	if (!PCE.RAM || !PCE.VRAM || !PCE.NULLRAM || !PCE.MemoryMapR || !PCE.MemoryMapW
		|| (PCE.VPC.is_sgx && !PCE.VRAM2)) {
		pce_term();
		return -1;
	}

	// VRAM/SPRAM/VDC.regs storage now exists — point gfx.c's VDC1 context
	// at it. (gfx_init() runs before pce_init() in InitPCE so it can't bind.)
	gfx_bind_vdc1();

	for (int i = 0; i < 0xFF; i++) {
		PCE.MemoryMapR[i] = PCE.NULLRAM;
		PCE.MemoryMapW[i] = PCE.NULLRAM;
	}

	PCE.MemoryMapR[0xF8] = PCE.RAM;
	PCE.MemoryMapW[0xF8] = PCE.RAM;
	if (PCE.VPC.is_sgx) {
		// SuperGrafx: banks F9-FB extend WRAM to a full 32 KB. The page-set
		// macros add a per-page offset, so MemoryMap[V] must point at the
		// BASE of each 8 KB window.
		PCE.MemoryMapR[0xF9] = PCE.RAM + 0x2000;
		PCE.MemoryMapW[0xF9] = PCE.RAM + 0x2000;
		PCE.MemoryMapR[0xFA] = PCE.RAM + 0x4000;
		PCE.MemoryMapW[0xFA] = PCE.RAM + 0x4000;
		PCE.MemoryMapR[0xFB] = PCE.RAM + 0x6000;
		PCE.MemoryMapW[0xFB] = PCE.RAM + 0x6000;
	}
	PCE.MemoryMapR[0xFF] = PCE.IOAREA;
	PCE.MemoryMapW[0xFF] = PCE.IOAREA;

	if (cd_init() != 0) {
		pce_term();
		return -1;
	}

	// pce_reset();

	return 0;
}


/**
  * Terminate the hardware
  **/
void
pce_term(void)
{
	cd_term();

	free(PCE.RAM);
	PCE.RAM = NULL;
	free(PCE.VRAM);
	PCE.VRAM = NULL;
	if (PCE.VRAM2) {
		frens_f_free(PCE.VRAM2);
		PCE.VRAM2 = NULL;
	}
	free(PCE.NULLRAM);
	PCE.NULLRAM = NULL;
	free(PCE.ExRAM);
	PCE.ExRAM = NULL;
	free(PCE.ROM);
	PCE.ROM = NULL;
	free(PCE.MemoryMapR);
	PCE.MemoryMapR = NULL;
	free(PCE.MemoryMapW);
	PCE.MemoryMapW = NULL;
}


/**
  * Run emulation for one frame
  **/
void __not_in_flash_func(pce_run)(void)
{
	// Handle pending video mode changes
	if (PCE.VDC.mode_chg) {
		PCE.VDC.screen_width = IO_VDC_SCREEN_WIDTH;
		PCE.VDC.screen_height = IO_VDC_SCREEN_HEIGHT;
		PCE.VDC.mode_chg = 0;
	}
	// Emulate!
	for (PCE.Scanline = 0; PCE.Scanline < 263; ++PCE.Scanline) {
		PCE.MaxCycles += PCE.Timer.cycles_per_line;
		/*while (PCE.MaxCycles > 0) */ {
			h6280_run(PCE.MaxCycles);
			timer_run(PCE.Cycles);
			PCE.MaxCycles -= PCE.Cycles;
			PCE.Cycles = 0;
		}
		gfx_run();
		osd_psg_scanline();
	}
}


/**
 * Functions to access PCE hardware
 **/

static inline void
timer_run(int cycles)
{
	PCE.Timer.cycles_counter -= PCE.Timer.cycles_per_line;

	if (PCE.Timer.cycles_counter < 0) {
		PCE.Timer.cycles_counter += CYCLES_PER_TIMER_TICK;
		if (PCE.Timer.running) {
			// Trigger when it underflows from 0
			if (PCE.Timer.counter > 0x7F) {
				PCE.Timer.counter = PCE.Timer.reload;
				CPU.irq_lines |= INT_TIMER;
			}
			PCE.Timer.counter--;
		}
	}
}


static inline void
cart_write(uint16_t A, uint8_t V)
{
	TRACE_IO("Cart Write %02x at %04x\n", V, A);

	// SF2 Mapper
	if (A >= 0xFFF0 && PCE.ROM_SIZE >= 0xC0)
	{
		if (PCE.SF2 != (A & 3))
		{
			PCE.SF2 = A & 3;
			uint8_t *base = PCE.ROM_DATA + PCE.SF2 * (512 * 1024);
			for (int i = 0x40; i < 0x80; i++)
			{
				PCE.MemoryMapR[i] = base + i * 0x2000;
			}
			for (int i = 0; i < 8; i++)
			{
				if (PCE.MMR[i] >= 0x40 && PCE.MMR[i] < 0x80)
					pce_bank_set(i, PCE.MMR[i]);
			}
		}
	}
}


// --- Per-VDC port helpers ---------------------------------------------------
// One handler shared by PCE (always VDC1) and SuperGrafx (VDC1 or VDC2 via the
// VPC ST register). Behavior for VDC1 must remain bit-identical to the original
// inline switch — verify with the .pce regression set after touching either.
// is_primary == 1 for VDC1 (drives PCE.ScrollYDiff + gfx_latch_context, which
// are still VDC1-only at this stage). For VDC2 we skip those side effects;
// step 6 introduces a VDC2-side latch/scroll-diff.

#define VR(R)        (vdc->regs[R])
#define VR_ACTIVE    (vdc->regs[vdc->reg])
#define VR_INC(reg)  do { unsigned _i[] = {1,32,64,128}; vdc->regs[(reg)].W += _i[(vdc->regs[CR].W >> 11) & 3]; } while (0)
#define VR_MINLINE   (VR(VPR).B.h + VR(VPR).B.l)

static inline uint8_t __not_in_flash_func(vdc_io_read)(vdc_t *vdc, uint16_t *vram, uint8_t port)
{
	switch (port) {
	case 0: {
		uint8_t ret = vdc->status;
		vdc->status = 0;
		return ret;
	}
	case 1:
		return 0;
	case 2:
		if (vdc->reg == VRR) {
			return vram[VR(MARR).W & 0x7FFF] & 0xFF;
		}
		return VR_ACTIVE.B.l;
	case 3:
		if (vdc->reg == VRR) {
			uint8_t ret = vram[VR(MARR).W & 0x7FFF] >> 8;
			VR_INC(MARR);
			return ret;
		}
		return VR_ACTIVE.B.h;
	}
	return 0xFF;
}

static inline void __not_in_flash_func(vdc_io_write)(vdc_t *vdc, uint16_t *vram, uint8_t port, uint8_t V, int is_primary)
{
	switch (port) {
	case 0: // Latch
		vdc->reg = V & 31;
		return;

	case 1: // Not used
		return;

	case 2: // VDC data (LSB)
		switch (vdc->reg & 31) {
		case MAWR: case MARR: case VWR: case vdc3: case vdc4:
		case RCR: case MWR:
		case DCR: case SOUR: case DISTR: case LENR: case SATB:
			break;

		case CR:
			if (VR_ACTIVE.B.l != V) {
				if (is_primary) gfx_latch_context(0);
				else            gfx_latch_context_vdc2(0);
			}
			break;

		case BXR:
			if (VR_ACTIVE.B.l != V) {
				if (is_primary) gfx_latch_context(0);
				else            gfx_latch_context_vdc2(0);
			}
			break;

		case BYR:
			if (is_primary) {
				gfx_latch_context(0);
				PCE.ScrollYDiff = PCE.Scanline - 1 - VR_MINLINE;
			} else {
				gfx_latch_context_vdc2(0);
				PCE.VPC.scroll_y_diff_vdc2 = PCE.Scanline - 1 - VR_MINLINE;
			}
			break;

		case HSR:
			V = 0x1F;
			vdc->mode_chg = 1;
			break;

		case HDR:
			V &= 0x7F;
			vdc->mode_chg = 1;
			break;

		case VPR:
			V &= 0x1F;
			vdc->mode_chg = 1;
			break;

		case VDW: case VCR:
			vdc->mode_chg = 1;
			break;
		}
		VR_ACTIVE.B.l = V;
		return;

	case 3: // VDC data (MSB)
		switch (vdc->reg & 31) {
		case MAWR: case MARR: case vdc3: case vdc4: case MWR:
		case DCR: case SOUR: case DISTR:
			break;

		case VWR:
			if (VR(MAWR).W < 0x8000) {
				vram[VR(MAWR).W] = (V << 8) | VR_ACTIVE.B.l;
			}
			VR_INC(MAWR);
			break;

		case CR:
			if (VR_ACTIVE.B.h != V) {
				if (is_primary) gfx_latch_context(0);
				else            gfx_latch_context_vdc2(0);
			}
			break;

		case RCR:
			V &= 0x3;
			break;

		case BXR:
			V &= 0x3;
			if (VR_ACTIVE.B.h != V) {
				if (is_primary) gfx_latch_context(0);
				else            gfx_latch_context_vdc2(0);
			}
			break;

		case BYR:
			if (is_primary) gfx_latch_context(0);
			else            gfx_latch_context_vdc2(0);
			V &= 0x1;
			if (is_primary) {
				PCE.ScrollYDiff = PCE.Scanline - 1 - VR_MINLINE;
				if (PCE.ScrollYDiff < 0) {
					MESSAGE_DEBUG("PCE.ScrollYDiff went negative when substraction VPR.h/.l (%d,%d)\n",
						VR(VPR).B.h, VR(VPR).B.l);
				}
			} else {
				PCE.VPC.scroll_y_diff_vdc2 = PCE.Scanline - 1 - VR_MINLINE;
			}
			break;

		case HSR:
			V &= 0x7F;
			vdc->mode_chg = 1;
			break;

		case HDR:
			V &= 0x7F;
			break;

		case VPR:
			V &= 0x7F;
			vdc->mode_chg = 1;
			break;

		case VDW:
			V &= 0x1;
			vdc->mode_chg = 1;
			break;

		case VCR:
			vdc->mode_chg = 1;
			return; // not interested in the MSB of VCR

		case LENR:
			VR(LENR).B.h = V;
			{
				int src_inc = (VR(DCR).W & 8) ? -1 : 1;
				int dst_inc = (VR(DCR).W & 4) ? -1 : 1;
				while (VR(LENR).W != 0xFFFF) {
					if (VR(DISTR).W < 0x8000) {
						vram[VR(DISTR).W] = vram[VR(SOUR).W];
					}
					VR(SOUR).W  += src_inc;
					VR(DISTR).W += dst_inc;
					VR(LENR).W  -= 1;
				}
			}
			if (is_primary) {
				gfx_irq(VDC_STAT_DV);
			} else {
				// VDC2 DV IRQ — push to its own stack, then drain via gfx_irq.
				PCE.VDC2.pending_irqs <<= 4;
				PCE.VDC2.pending_irqs |= VDC_STAT_DV & 0xF;
				gfx_irq(-1);
			}
			return;

		case SATB:
			vdc->satb = DMA_TRANSFER_PENDING;
			break;
		}
		VR_ACTIVE.B.h = V;
		return;
	}
}

uint8_t __not_in_flash_func(pce_readIO)(uint16_t A)
{
	// Arcade Card bank $40-$43 memory-mapped read
	if (CD.cd_attached && CD.acd_ram) {
		uint8_t bank = PCE.MMR[A >> 13];
		if (bank >= 0x40 && bank <= 0x43)
			return cd_acd_read_bank(bank & 0x03);
	}

	uint8_t ret = 0xFF; // Open Bus

	// The last read value in 0800-017FF is read from the io buffer
	if (A >= 0x800 && A < 0x1800)
		ret = PCE.io_buffer;

	switch (A & 0x1F00) {
	case 0x0000:                /* VDC / VPC (SuperGrafx) */
		if (PCE.VPC.is_sgx) {
			uint8_t port = A & 0x1F;
			if (port < 4) {
				vdc_t *vdc = PCE.VPC.st_to_vdc2 ? &PCE.VDC2 : &PCE.VDC;
				uint16_t *vram = PCE.VPC.st_to_vdc2 ? PCE.VRAM2 : PCE.VRAM;
				ret = vdc_io_read(vdc, vram, port);
			} else if (port >= 0x10 && port <= 0x13) {
				ret = vdc_io_read(&PCE.VDC2, PCE.VRAM2, port & 3);
			} else if (port == 0x08) {
				ret = PCE.VPC.priority1;
			} else if (port == 0x09) {
				ret = PCE.VPC.priority2;
			} else if (port == 0x0A) {
				ret = PCE.VPC.window1 & 0xFF;
			} else if (port == 0x0B) {
				ret = (PCE.VPC.window1 >> 8) & 0x03;
			} else if (port == 0x0C) {
				ret = PCE.VPC.window2 & 0xFF;
			} else if (port == 0x0D) {
				ret = (PCE.VPC.window2 >> 8) & 0x03;
			} else {
				ret = 0;
			}
			break;
		}
		ret = vdc_io_read(&PCE.VDC, PCE.VRAM, A & 3);
		break;

	case 0x0400:                /* VCE */
		switch (A & 7) {
		case 0: ret = 0xFF; break; // Write only
		case 1: ret = 0xFF; break; // Unused
		case 2: ret = 0xFF; break; // Write only
		case 3: ret = 0xFF; break; // Write only
		case 4: ret = PCE.VCE.regs[PCE.VCE.reg].B.l; break; // Color LSB (8 bit)
		case 5: {
			ret = (PCE.VCE.regs[PCE.VCE.reg++].B.h) | 0xFE; // Color MSB (1 bit)
			PCE.VCE.reg &= 0x1FF;
			break;
		}
		case 6: ret = 0xFF; break; // Unused
		}
		break;

	case 0x0800:                /* PSG */
		switch (A & 15) {
		case 0: ret = PCE.PSG.ch; break;
		case 1: ret = PCE.PSG.volume; break;
		case 2: ret = PCE.PSG.chan[PCE.PSG.ch].freq_lsb; break;
		case 3: ret = PCE.PSG.chan[PCE.PSG.ch].freq_msb; break;
		case 4: ret = PCE.PSG.chan[PCE.PSG.ch].control; break;
		case 5: ret = PCE.PSG.chan[PCE.PSG.ch].balance; break;
		case 6: ret = PCE.PSG.chan[PCE.PSG.ch].wave_index; break;
		case 7: ret = PCE.PSG.chan[PCE.PSG.ch].noise_ctrl; break;
		case 8: ret = PCE.PSG.lfo_freq; break;
		case 9: ret = PCE.PSG.lfo_ctrl; break;
		}
		break;

	case 0x0C00:                /* Timer */
		ret = (PCE.io_buffer & 0x80);
		if (PCE.Timer.cycles_counter == PCE.Cycles)
			ret |= (PCE.Timer.counter - 1) & 0x7F;
		else
			ret |= PCE.Timer.counter;
		break;

	case 0x1000:                /* Joypad */
		ret = PCE.Joypad.regs[PCE.Joypad.counter] ^ 0xff;
		if (PCE.Joypad.nibble & 1)
			ret >>= 4;
		else {
			ret &= 15;
			PCE.Joypad.counter = ((PCE.Joypad.counter + 1) % 5);
		}
		// Bits 4-5 always set. Bit 6 = 0 (Japan) / 1 (US). Bit 7 = 0 (CD
		// attached) / 1 (no CD). The BIOS reads this to decide whether to
		// boot in CD mode and whether to apply region-specific logic.
		ret |= 0x30;
		if (cd_bios_is_us())  ret |= 0x40;
		if (!CD.cd_attached)  ret |= 0x80;
		break;

	case 0x1400:                /* IRQ */
		switch (A & 3) {
		case 2:
			ret = CPU.irq_mask | (PCE.io_buffer & ~INT_MASK);
			break;
		case 3:
			ret = CPU.irq_lines;
			CPU.irq_lines = 0;
			break;
		}
		break;

	case 0x1A00:                // Arcade Card
		if (CD.cd_attached)
			ret = cd_read(A);
		break;

	case 0x1800:                // CD-ROM extension + Super System Card ID
		if (CD.cd_attached)
			ret = cd_read(A);
		break;
	}

	TRACE_IO("IO Read %02x at %04x\n", ret, A);

	// The last read value in 0800-017FF is saved in the io buffer
	if (A >= 0x800 && A < 0x1800)
		PCE.io_buffer = ret;

	return ret;
}


void __not_in_flash_func(pce_writeIO)(uint16_t A, uint8_t V)
{
	// Arcade Card bank $40-$43 memory-mapped write
	if (CD.cd_attached && CD.acd_ram) {
		uint8_t bank = PCE.MMR[A >> 13];
		if (bank >= 0x40 && bank <= 0x43) {
			cd_acd_write_bank(bank & 0x03, V);
			return;
		}
	}

	TRACE_IO("IO Write %02x at %04x\n", V, A);

	// The last write value in 0800-017FF is saved in the io buffer
	if (A >= 0x800 && A < 0x1800)
		PCE.io_buffer = V;

	switch (A & 0x1F00) {
	case 0x0000:                /* VDC / VPC (SuperGrafx) */
		if (PCE.VPC.is_sgx) {
			uint8_t port = A & 0x1F;
			if (port < 4) {
				vdc_t *vdc = PCE.VPC.st_to_vdc2 ? &PCE.VDC2 : &PCE.VDC;
				uint16_t *vram = PCE.VPC.st_to_vdc2 ? PCE.VRAM2 : PCE.VRAM;
				vdc_io_write(vdc, vram, port, V, !PCE.VPC.st_to_vdc2);
			} else if (port >= 0x10 && port <= 0x13) {
				vdc_io_write(&PCE.VDC2, PCE.VRAM2, port & 3, V, 0);
			} else if (port == 0x08) {
				// Priority1: low nib = Both window, high nib = Window2
				PCE.VPC.priority1 = V;
				PCE.VPC.window_cfg[3] = V & 0x0F;        // Both
				PCE.VPC.window_cfg[2] = (V >> 4) & 0x0F; // Window2
			} else if (port == 0x09) {
				// Priority2: low nib = Window1, high nib = NoWindow
				PCE.VPC.priority2 = V;
				PCE.VPC.window_cfg[1] = V & 0x0F;        // Window1
				PCE.VPC.window_cfg[0] = (V >> 4) & 0x0F; // NoWindow
			} else if (port == 0x0A) {
				PCE.VPC.window1 = (PCE.VPC.window1 & 0x300) | V;
			} else if (port == 0x0B) {
				PCE.VPC.window1 = (PCE.VPC.window1 & 0xFF) | ((V & 0x03) << 8);
			} else if (port == 0x0C) {
				PCE.VPC.window2 = (PCE.VPC.window2 & 0x300) | V;
			} else if (port == 0x0D) {
				PCE.VPC.window2 = (PCE.VPC.window2 & 0xFF) | ((V & 0x03) << 8);
			} else if (port == 0x0E) {
				PCE.VPC.st_to_vdc2 = V & 0x01;
			}
			return;
		}
		vdc_io_write(&PCE.VDC, PCE.VRAM, A & 3, V, 1);
		return;

	case 0x0400:                /* VCE */
		switch (A & 7) {
		case 0:                                 // VCE control
			return;

		case 1:                                 // Not used
			return;

		case 2:                                 // Color table address (LSB)
			PCE.VCE.reg &= 0x100;
			PCE.VCE.reg |= V;
			return;

		case 3:                                 // Color table address (MSB)
			PCE.VCE.reg &= 0xFF;
			PCE.VCE.reg |= (V & 1) << 8;
			return;

		case 4:                                 // Color table data (LSB)
			PCE.VCE.regs[PCE.VCE.reg].B.l = V;
			{
				size_t n = PCE.VCE.reg;
				size_t c = PCE.VCE.regs[n].W >> 1;
				if (n == 0) {
					for (int i = 0; i < 256; i += 16)
						PCE.Palette[i] = c;
				} else if (n & 15)
					PCE.Palette[n] = c;
			}
			return;

		case 5:                                 // Color table data (MSB)
			PCE.VCE.regs[PCE.VCE.reg].B.h = V;
			{
				size_t n = PCE.VCE.reg;
				size_t c = PCE.VCE.regs[n].W >> 1;
				if (n == 0) {
					for (int i = 0; i < 256; i += 16)
						PCE.Palette[i] = c;
				} else if (n & 15)
					PCE.Palette[n] = c;
			}
			PCE.VCE.reg = (PCE.VCE.reg + 1) & 0x1FF;
			return;

		case 6:                                 // Not used
			return;

		case 7:                                 // Not used
			return;
		}
		break;

	case 0x0800:                /* PSG */
		// Sample-accurate audio: generate PSG output up to the current CPU
		// cycle BEFORE applying this register write, so the previous register
		// state is rendered for exactly the cycles it was active (mirrors
		// Mesen2's PcePsg::Run() before each write). frame_cycle is monotonic
		// within a frame: Scanline * cycles_per_line + cycles consumed so far.
		osd_psg_sync(PCE.Scanline * PCE.Timer.cycles_per_line + PCE.Cycles);
		switch (A & 15) {
		case 0:                                 // Select PSG channel
			PCE.PSG.ch = MIN(V & 7, 5);
			return;

		case 1:                                 // Select global volume
			PCE.PSG.volume = V;
			return;

		case 2:                                 // Frequency setting, 8 lower bits
			PCE.PSG.chan[PCE.PSG.ch].freq_lsb = V;
			return;

		case 3:                                 // Frequency setting, 4 upper bits
			PCE.PSG.chan[PCE.PSG.ch].freq_msb = V & 0xF;
			return;

		case 4:
			if ((V & 0xC0) == (PSG_DDA_ENABLE)) {
				PCE.PSG.chan[PCE.PSG.ch].wave_index = 0; // Reset wave index pointer
			}

			PCE.PSG.chan[PCE.PSG.ch].control = V;
			return;

		case 5:                                 // Set channel specific volume
			PCE.PSG.chan[PCE.PSG.ch].balance = V;
			return;

		case 6:                                 // Put a value into the waveform or direct audio buffers
			if (PCE.PSG.chan[PCE.PSG.ch].control & PSG_DDA_ENABLE) {
				// DDA mode: sample-and-hold the single output value (Mesen2
				// model). Sample-accurate sync (osd_psg_sync, called above)
				// renders the held value for exactly the cycles between writes,
				// so the high-rate DDA stream reproduces correctly without a
				// FIFO. The old buffered model mistimed playback (clicks).
				PCE.PSG.chan[PCE.PSG.ch].dda_value = V & 0x1F;
			} else {
				// Wave mode: write to the wave buffer
				PCE.PSG.chan[PCE.PSG.ch].wave_data[PCE.PSG.chan[PCE.PSG.ch].wave_index] = V & 0x1F;
				PCE.PSG.chan[PCE.PSG.ch].wave_index = (PCE.PSG.chan[PCE.PSG.ch].wave_index + 1) & 0x1F;
			}
			return;

		case 7:
			PCE.PSG.chan[PCE.PSG.ch].noise_ctrl = V;
			return;

		case 8:
			PCE.PSG.lfo_freq = V;
			return;

		case 9:
			PCE.PSG.lfo_ctrl = V;
			return;
		}
		break;

	case 0x0C00:                /* Timer */
		switch (A & 1) {
		case 0:
			PCE.Timer.reload = (V & 0x7F); // + 1;
			return;
		case 1:
			V &= 1;
            if (V && !PCE.Timer.running){
                // PCE.Timer.cycles_counter = PCE.Cycles + CYCLES_PER_TIMER_TICK;
				PCE.Timer.counter = PCE.Timer.reload;
            }
			PCE.Timer.running = V;
			return;
		}
		break;

	case 0x1000:                /* Joypad */
		PCE.Joypad.nibble = V & 1;
		if (V & 2)
			PCE.Joypad.counter = 0;
		return;

	case 0x1400:                /* IRQ */
		switch (A & 3) {
		case 2:
			CPU.irq_mask = V & INT_MASK;
			return;
		case 3:
			CPU.irq_lines &= ~INT_TIMER;
			return;
		}
		break;

	case 0x1A00:                /* Arcade Card */
		if (CD.cd_attached)
			cd_write(A, V);
		return;

	case 0x1800:                /* CD-ROM extension + Super System Card ID */
		if (CD.cd_attached)
			cd_write(A, V);
		return;

	case 0x1F00:                /* Street Fighter 2 Mapper */
		cart_write(A, V);
		return;
	}

	MESSAGE_DEBUG("ignored I/O write: %04x,%02x at PC = %04X\n", A, V, CPU.PC);
}
