// pce-go.c - Entry file to start/stop/reset/save emulation
//
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pce-go.h"
#include "gfx.h"
#include "psg.h"
#include "pce.h"
#include "cd.h"

#include "ff.h"

extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

/**
 * Save state file description.
 */
#define SVAR_1(k, v) { {k, 1, 1}, &v }
#define SVAR_2(k, v) { {k, 2, 2}, &v }
#define SVAR_4(k, v) { {k, 4, 4}, &v }
#define SVAR_A(k, v) { {k, 0, sizeof(v)}, &v }
#define SVAR_N(k, v, n) { {k, 0, n}, &v }
#define SVAR_P(k, v, n) { {k, 5, n}, &v }
#define SVAR_END { {"END", 0, 0}, NULL }

const char SAVESTATE_HEADER[8] = "PCE_V010";
save_var_t SaveStateVars[] =
{
	// Arrays
	SVAR_P("RAM", PCE.RAM, 0x2000),             SVAR_P("VRAM", PCE.VRAM, 0x10000),
	SVAR_N("SPRAM", PCE.SPRAM, 512),            SVAR_N("PAL", PCE.Palette, 512),
	SVAR_A("MMR", PCE.MMR),

	// CPU registers
	SVAR_2("CPU.PC", CPU.PC),    SVAR_1("CPU.A", CPU.A),    SVAR_1("CPU.X", CPU.X),
	SVAR_1("CPU.Y", CPU.Y),      SVAR_1("CPU.P", CPU.P),    SVAR_1("CPU.S", CPU.S),

	// Misc
	SVAR_4("Cycles", PCE.Cycles),               SVAR_4("MaxCycles", PCE.MaxCycles),
	SVAR_1("SF2", PCE.SF2),

	// IRQ
	SVAR_1("IRQ.mask", CPU.irq_mask),           SVAR_1("IRQ.lines", CPU.irq_lines),
	SVAR_1("IRQ.m_delay", CPU.irq_mask_delay),

	// PSG
	SVAR_1("PSG.ch", PCE.PSG.ch),               SVAR_1("PSG.vol", PCE.PSG.volume),
	SVAR_1("PSG.lfo_f", PCE.PSG.lfo_freq),      SVAR_1("PSG.lfo_c", PCE.PSG.lfo_ctrl),
	SVAR_N("PSG.ch0", PCE.PSG.chan[0], 40),     SVAR_N("PSG.ch1", PCE.PSG.chan[1], 40),
	SVAR_N("PSG.ch2", PCE.PSG.chan[2], 40),     SVAR_N("PSG.ch3", PCE.PSG.chan[3], 40),
	SVAR_N("PSG.ch4", PCE.PSG.chan[4], 40),     SVAR_N("PSG.ch5", PCE.PSG.chan[5], 40),

	// VCE
	SVAR_A("VCE.regs", PCE.VCE.regs),           SVAR_2("VCE.reg", PCE.VCE.reg),

	// VDC
	SVAR_A("VDC.regs", PCE.VDC.regs),           SVAR_1("VDC.reg", PCE.VDC.reg),
	SVAR_1("VDC.status", PCE.VDC.status),       SVAR_1("VDC.satb", PCE.VDC.satb),
	SVAR_4("VDC.irqs", PCE.VDC.pending_irqs),   SVAR_1("VDC.vram", PCE.VDC.vram),

	// Timer
	SVAR_4("TMR.reload", PCE.Timer.reload),   SVAR_4("TMR.running", PCE.Timer.running),
	SVAR_4("TMR.counter", PCE.Timer.counter), SVAR_4("TMR.next", PCE.Timer.cycles_counter),
	SVAR_4("TMR.freq", PCE.Timer.cycles_per_line),

	SVAR_END
};

#define TWO_PART_ROM 0x0001
#define ONBOARD_RAM  0x0100
#define US_ENCODED   0x0010

static const struct {
	const uint32_t CRC;
	const char *Name;
	const uint32_t Flags;
} romFlags[] = {
	{0xF0ED3094, "Blazing Lazers", TWO_PART_ROM},
	{0xB4A1B0F6, "Blazing Lazers", TWO_PART_ROM},
	{0x55E9630D, "Legend of Hero Tonma", US_ENCODED},
	{0x083C956A, "Populous", ONBOARD_RAM},
	{0x0A9ADE99, "Populous", ONBOARD_RAM},
	{0x00000000, "Unknown", 0},
};

static bool running = false;

/**
 * Load card into memory and set its memory map
 * NOTE: This function takes ownership of `data`
 */
int
LoadCard(uint8_t *data, size_t size)
{
	if (data == NULL || size < 0x2000 || size > 0x1000000)
	{
		MESSAGE_ERROR("Invalid rom data received\n");
		return -1;
	}

	if (PCE.ROM != NULL)
		free(PCE.ROM);

	int offset = size & 0x1fff;

	// read ROM
	PCE.ROM = data;
	PCE.ROM_SIZE = (size - offset) / 0x2000;
	PCE.ROM_DATA = PCE.ROM + offset;
	PCE.ROM_CRC = crc32_le(0, PCE.ROM, size);

	uint32_t IDX = 0;
	uint32_t ROM_MASK = 1;

	while (ROM_MASK < PCE.ROM_SIZE) ROM_MASK <<= 1;
	ROM_MASK--;

	MESSAGE_INFO("ROM LOADED: OFFSET=%d, BANKS=%d, MASK=%03X, CRC=%08X\n",
		offset, (int)PCE.ROM_SIZE, (int)ROM_MASK, (int)PCE.ROM_CRC);

	while (romFlags[IDX].CRC) {
		if (PCE.ROM_CRC == romFlags[IDX].CRC)
			break;
		IDX++;
	}

	MESSAGE_INFO("Game Name: %s\n", romFlags[IDX].Name);

	// US Encrypted
	if ((romFlags[IDX].Flags & US_ENCODED) || PCE.ROM_DATA[0x1FFF] < 0xE0)
	{
		MESSAGE_INFO("This rom is probably US encrypted, decrypting...\n");

		unsigned char inverted_nibble[16] = {
			0, 8, 4, 12, 2, 10, 6, 14,
			1, 9, 5, 13, 3, 11, 7, 15
		};

		for (int x = 0; x < PCE.ROM_SIZE * 0x2000; x++) {
			unsigned char temp = PCE.ROM_DATA[x] & 15;

			PCE.ROM_DATA[x] &= ~0x0F;
			PCE.ROM_DATA[x] |= inverted_nibble[PCE.ROM_DATA[x] >> 4];

			PCE.ROM_DATA[x] &= ~0xF0;
			PCE.ROM_DATA[x] |= inverted_nibble[temp] << 4;
		}
	}

	// For example with Devil Crush 512Ko
	if (romFlags[IDX].Flags & TWO_PART_ROM)
		PCE.ROM_SIZE = 0x30;

	// Game ROM
	for (int i = 0; i < 0x80; i++) {
		if (PCE.ROM_SIZE == 0x30) {
			switch (i & 0x70) {
			case 0x00:
			case 0x10:
			case 0x50:
				PCE.MemoryMapR[i] = PCE.ROM_DATA + (i & ROM_MASK) * 0x2000;
				break;
			case 0x20:
			case 0x60:
				PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x20) & ROM_MASK) * 0x2000;
				break;
			case 0x30:
			case 0x70:
				PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x10) & ROM_MASK) * 0x2000;
				break;
			case 0x40:
				PCE.MemoryMapR[i] = PCE.ROM_DATA + ((i - 0x20) & ROM_MASK) * 0x2000;
				break;
			}
		} else {
			PCE.MemoryMapR[i] = PCE.ROM_DATA + (i & ROM_MASK) * 0x2000;
		}
		PCE.MemoryMapW[i] = PCE.NULLRAM;
	}

	// Allocate the card's onboard ram
	if (romFlags[IDX].Flags & ONBOARD_RAM) {
		if (!PCE.ExRAM)
			PCE.ExRAM = malloc(0x8000);
		PCE.MemoryMapR[0x40] = PCE.MemoryMapW[0x40] = PCE.ExRAM;
		PCE.MemoryMapR[0x41] = PCE.MemoryMapW[0x41] = PCE.ExRAM + 0x2000;
		PCE.MemoryMapR[0x42] = PCE.MemoryMapW[0x42] = PCE.ExRAM + 0x4000;
		PCE.MemoryMapR[0x43] = PCE.MemoryMapW[0x43] = PCE.ExRAM + 0x6000;
	}

	// Mapper for roms >= 1.5MB (SF2, homebrews)
	if (PCE.ROM_SIZE >= 192)
		PCE.MemoryMapW[0x00] = PCE.IOAREA;

	ResetPCE(0);

	return 0;
}


/**
 * Load card into memory and set its memory map
 */
int
LoadFile(const char *name)
{
	MESSAGE_INFO("Opening %s...\n", name);

	FILE *fp = fopen(name, "rb");
	if (fp == NULL)
	{
		MESSAGE_ERROR("Failed to open %s!\n", name);
		return -1;
	}

	// find file size
	fseek(fp, 0, SEEK_END);
	size_t fsize = ftell(fp);

	// read ROM
	void *data = malloc(fsize);
	if (data == NULL)
	{
		MESSAGE_ERROR("Failed to allocate ROM buffer!\n");
		fclose(fp);
		return -1;
	}

	fseek(fp, 0, SEEK_SET);
	fread(data, 1, fsize, fp);
	fclose(fp);

	return LoadCard(data, fsize);
}


// CD RAM sizes (bytes)
#define CD_RAM_SIZE      0x10000   // 64KB
#define SCD_RAM_SIZE     0x30000   // 192KB
#define ADPCM_RAM_SIZE   0x10000   // 64KB
#define ACD_RAM_SIZE     0x200000  // 2MB
#define BRAM_PAGE_SIZE   0x2000    // 8KB page, lower 2KB is real BRAM

// Internal helper: free anything cd_term() would free, plus any partially-
// installed BIOS in PCE.ROM. Used to undo allocations when LoadDisc fails.
static void unload_disc_state(void)
{
	cd_term();
	if (PCE.ROM) {
		frens_f_free(PCE.ROM);
		PCE.ROM = NULL;
		PCE.ROM_DATA = NULL;
		PCE.ROM_SIZE = 0;
	}
}

/**
 * Load a CD-ROM game.
 *
 * Steps:
 *   1. Locate a BIOS in /bios/ via cd_find_bios() (CRC-matched, picks the
 *      most capable variant).
 *   2. Load the BIOS into PSRAM and map it to hardware pages 0x00-0x1F.
 *   3. Allocate CD-ROM RAM, Super System Card RAM, ADPCM RAM, Arcade Card
 *      RAM and the BRAM page in PSRAM.
 *   4. Parse the CUE sheet and open the BIN file with a persistent handle.
 *   5. Install the CD memory map and reset the emulator.
 */
int
LoadDisc(const char *cue_path)
{
	if (!cue_path) return -2;

	// Start from a clean slate — if the previous game was also a CD this
	// frees the prior BIOS/CD allocations.
	unload_disc_state();
	if (cd_init() != 0) {
		MESSAGE_ERROR("cd_init failed\n");
		return -2;
	}

	// --- 1. Find BIOS ---
	// Static for stack safety (FF_MAX_LFN+8 buffer + ~600 B FIL would push
	// into core1's SCRATCH region under the 3 KB stack).
	static char bios_path[FF_MAX_LFN + 8];
	static FIL  fil;
	bios_variant_t variant = BIOS_UNKNOWN;
	if (cd_find_bios(bios_path, sizeof(bios_path), &variant) != 0) {
		MESSAGE_ERROR("No CD BIOS found in /bios/\n");
		return -1;
	}

	// --- 2. Load BIOS into PSRAM ---
	if (f_open(&fil, bios_path, FA_READ) != FR_OK) {
		MESSAGE_ERROR("Cannot open BIOS %s\n", bios_path);
		return -2;
	}
	FSIZE_t bios_size = f_size(&fil);
	if (bios_size < 0x2000 || bios_size > 0x100000) {
		MESSAGE_ERROR("BIOS size unreasonable: %lu\n", (unsigned long)bios_size);
		f_close(&fil);
		return -2;
	}
	PCE.ROM = (uint8_t *)frens_f_malloc(bios_size);
	if (!PCE.ROM) {
		MESSAGE_ERROR("Cannot allocate %lu bytes for BIOS\n", (unsigned long)bios_size);
		f_close(&fil);
		return -2;
	}
	UINT br = 0;
	if (f_read(&fil, PCE.ROM, bios_size, &br) != FR_OK || br != bios_size) {
		MESSAGE_ERROR("BIOS read failed (got %u of %lu)\n", br, (unsigned long)bios_size);
		f_close(&fil);
		unload_disc_state();
		return -2;
	}
	f_close(&fil);

	PCE.ROM_DATA = PCE.ROM;
	PCE.ROM_SIZE = bios_size / 0x2000;     // size in 8KB blocks
	PCE.ROM_CRC = 0;                       // BIOS CRC tracked in CD.bios_variant

	// Map BIOS at hardware pages 0x00 .. (rom_pages-1). 32 pages = 256KB.
	int rom_pages = (int)(bios_size / 0x2000);
	if (rom_pages > 0x80) rom_pages = 0x80;
	for (int i = 0; i < rom_pages; i++) {
		PCE.MemoryMapR[i] = PCE.ROM_DATA + i * 0x2000;
		PCE.MemoryMapW[i] = PCE.NULLRAM;   // BIOS is read-only
	}

	// --- 3. Allocate CD/SCD/ADPCM/Arcade Card RAM + BRAM page ---
	CD.cd_ram         = (uint8_t *)frens_f_malloc(CD_RAM_SIZE);
	CD.scd_ram        = (uint8_t *)frens_f_malloc(SCD_RAM_SIZE);
	CD.adpcm_ram      = (uint8_t *)frens_f_malloc(ADPCM_RAM_SIZE);
	CD.acd_ram        = (uint8_t *)frens_f_malloc(ACD_RAM_SIZE);
	CD.bram           = (uint8_t *)frens_f_malloc(BRAM_PAGE_SIZE);
	static uint8_t audio_ring_sram[4 * CD_RAW_SECTOR_SIZE];
	CD.audio_ring_buf = audio_ring_sram;
	if (!CD.cd_ram || !CD.scd_ram || !CD.adpcm_ram || !CD.acd_ram || !CD.bram
	    || !CD.audio_ring_buf) {
		MESSAGE_ERROR("CD/Arcade Card RAM allocation failed\n");
		unload_disc_state();
		return -2;
	}
	memset(CD.cd_ram,    0,    CD_RAM_SIZE);
	memset(CD.scd_ram,   0,    SCD_RAM_SIZE);
	memset(CD.adpcm_ram, 0,    ADPCM_RAM_SIZE);
	memset(CD.acd_ram,   0,    ACD_RAM_SIZE);
	memset(CD.bram,      0xFF, BRAM_PAGE_SIZE);   // "empty" pattern

	// --- 4. Parse CUE + open BIN ---
	if (cd_load_cue(cue_path) != 0) {
		MESSAGE_ERROR("cd_load_cue failed for %s\n", cue_path);
		unload_disc_state();
		return -2;
	}

	// --- 5. Wire memory map and reset ---
	cd_setup_memory_map();
	CD.cd_attached = true;
	CD.bram_locked = true;     // real hardware boots with BRAM locked

	MESSAGE_INFO("LoadDisc: BIOS=%s (variant=%d, %s), CD attached\n",
	             bios_path, (int)variant, CD.bios_is_us ? "US" : "JP");

	ResetPCE(true);
	return 0;
}


/**
 * Reset the emulator
 */
void
ResetPCE(bool hard)
{
	gfx_reset(hard);
	pce_reset(hard);
}


/**
 * Initialize the emulator (allocate memory, call osd_init* functions)
 */
int
InitPCE(int samplerate, bool stereo)
{
	if (gfx_init())
		return 1;

	if (psg_init(samplerate, stereo))
		return 1;

	if (pce_init())
		return 1;

	return 0;
}


/**
 * Returns a 256 colors palette in the chosen depth
 */
void *
PalettePCE(int bitdepth)
{
	if (bitdepth == 15) {
		uint16_t *palette = malloc(256 * 2);
		// ...
		return palette;
	}

	if (bitdepth == 16) {
		uint16_t *palette = malloc(256 * 2);
		for (int i = 0; i < 255; i++) {
			int r = (i & 0x1C) >> 1;
			int g = (i & 0xe0) >> 4;
			int b = (i & 0x03) << 2;
			palette[i] = (((r << 12) & 0xf800) + ((g << 7) & 0x07e0) + ((b << 1) & 0x001f));
		}
		palette[255] = 0xFFFF;
		return palette;
	}

	if (bitdepth == 24) {
		uint8_t *palette = malloc(256 * 3);
		uint8_t *ptr = palette;
		for (int i = 0; i < 255; i++) {
			*ptr++ = (i & 0x1C) << 2;
			*ptr++ = (i & 0xe0) >> 1;
			*ptr++ = (i & 0x03) << 4;
		}
		*ptr++ = 0xFF;
		*ptr++ = 0xFF;
		*ptr++ = 0xFF;
		return palette;
	}

	return NULL;
}


/**
 * Start the emulation
 */
void
RunPCE(void)
{
	running = true;

	while (running)
	{
		osd_input_read(PCE.Joypad.regs);
		pce_run();
		osd_vsync();
	}
}


/**
 * Load saved state
 */
int
LoadState(const char *name)
{
	MESSAGE_INFO("Loading state from %s...\n", name);

	char buffer[32];
	block_hdr_t block;
	int ret = -1;

	FILE *fp = fopen(name, "rb");
	if (fp == NULL)
		return -1;

	if (!fread(&buffer, 8, 1, fp) || memcmp(&buffer, SAVESTATE_HEADER, 8) != 0)
	{
		MESSAGE_ERROR("Loading state failed: Header mismatch\n");
		goto _cleanup;
	}

	while (fread(&block, sizeof(block), 1, fp))
	{
		size_t block_end = ftell(fp) + block.len;

		for (save_var_t *var = SaveStateVars; var->ptr; var++)
		{
			if (strncmp(var->desc.key, block.key, 12) == 0)
			{
				void *ptr = var->desc.type == 5 ? *((void**)var->ptr) : var->ptr;
				size_t len = MIN((size_t)var->desc.len, (size_t)block.len);
				if (!fread(ptr, len, 1, fp))
				{
					MESSAGE_ERROR("fread error reading block data\n");
					goto _cleanup;
				}
				if (len < var->desc.len)
				{
					memset(ptr + len, 0, var->desc.len - len);
				}
				MESSAGE_INFO("Loaded %s\n", var->desc.key);
				break;
			}
		}
		fseek(fp, block_end, SEEK_SET);
	}

	for (int i = 0; i < 8; i++)
		pce_bank_set(i, PCE.MMR[i]);

	gfx_reset(true);
	PCE.VDC.mode_chg = 1;
	ret = 0;

_cleanup:
	fclose(fp);

	return ret;
}


/**
 * Save current state
 */
int
SaveState(const char *name)
{
	MESSAGE_INFO("Saving state to %s...\n", name);

	int ret = -1;

	FILE *fp = fopen(name, "wb");
	if (fp == NULL)
		return -1;

	fwrite(SAVESTATE_HEADER, sizeof(SAVESTATE_HEADER), 1, fp);

	for (save_var_t *var = SaveStateVars; var->ptr; var++)
	{
		void *ptr = var->desc.type == 5 ? *((void**)var->ptr) : var->ptr;
		size_t len = var->desc.len;
		if (!fwrite(&var->desc, sizeof(var->desc), 1, fp))
		{
			MESSAGE_ERROR("fwrite error desc\n");
			goto _cleanup;
		}
		if (!fwrite(ptr, len, 1, fp))
		{
			MESSAGE_ERROR("fwrite error value\n");
			goto _cleanup;
		}
		MESSAGE_INFO("Saved %s\n", var->desc.key);
	}

	ret = 0;

_cleanup:
	fclose(fp);

	return ret;
}


/**
 * Cleanup and quit (not used in retro-go)
 */
void
ShutdownPCE()
{
	gfx_term();
	psg_term();
	pce_term();
}
