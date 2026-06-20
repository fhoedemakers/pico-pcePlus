// Host harness: run the pce-go core headless on Linux, dump frames as PPM.
// Usage: pce_host <rom.sgx|rom.pce> <frames> <dump-every> [outdir]
// Optional: env PCE_PRESS_RUN=<frame> taps the RUN button at that frame
// (held 10 frames) to get past title screens.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "pce-go.h"
#include "pce.h"
#include "gfx.h"
#include "cd.h"

int osd_gfx_render_line;
int pce_dbg_solo_vdc; // 0=normal mix, 1=VDC1 only, 2=VDC2 only (PCE_HOST_DEBUG)

// [16-byte scratch][line][16-byte scratch]... — XBUF_WIDTH already includes
// one 16-byte pad per line; add one extra pad before line 0 and after the
// last line (same layout main.cpp's line buffer provides per-line).
static uint8_t fb[XBUF_WIDTH * XBUF_HEIGHT + 32];

uint8_t *osd_gfx_framebuffer(int width, int height)
{
	(void)width; (void)height;
	return fb + 16;
}
void osd_gfx_lines_rendered(int first_line, int last_line) { (void)first_line; (void)last_line; }
void osd_input_read(uint8_t joypads[8]) { (void)joypads; }
void osd_vsync(void) {}
void osd_psg_scanline(void) {}
void osd_psg_sync(int frame_cycle) { (void)frame_cycle; }

// FNV-1a 32-bit over the active framebuffer region. Used by PCE_FREEZE_DETECT
// to flag consecutive identical frames — i.e. emulator-side freezes. We hash
// width × XBUF_HEIGHT (not just the active height) so a mode change still
// triggers a hash mismatch and resets the run.
static uint32_t fb_hash(int w)
{
	uint32_t h = 2166136261u;
	const uint8_t *src = fb + 16;
	for (int y = 0; y < XBUF_HEIGHT; y++) {
		const uint8_t *row = src + y * XBUF_WIDTH;
		for (int x = 0; x < w; x++) {
			h ^= row[x];
			h *= 16777619u;
		}
	}
	// Fold in the VCE color registers: the framebuffer stores palette
	// *indices*, so a fade/flash effect (game cycling $0402/$0403 colors)
	// leaves the indices identical while the visible colors change. Without
	// this a fade reads as a frozen frame (false positive). A genuine freeze
	// holds both indices AND colors static.
	for (int i = 0; i < 0x200; i++) {
		uint16_t c = PCE.VCE.regs[i].W;
		h ^= (uint8_t)(c & 0xFF);
		h *= 16777619u;
		h ^= (uint8_t)(c >> 8);
		h *= 16777619u;
	}
	return h;
}

static void dump_ppm(const char *dir, int frame)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, frame);
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }

	int w = PCE.VDC.screen_width > 0 ? PCE.VDC.screen_width : 256;
	if (w > 352) w = 352;
	int h = XBUF_HEIGHT;

	fprintf(f, "P6\n%d %d\n255\n", w, h);
	const uint8_t *src = fb + 16;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t i = src[y * XBUF_WIDTH + x];
			// Same GRB 3-3-2 expansion as PalettePCE(24).
			uint8_t rgb[3] = {
				(uint8_t)((i & 0x1C) << 2),
				(uint8_t)((i & 0xE0) >> 1),
				(uint8_t)((i & 0x03) << 4),
			};
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <rom> <frames> <dump-every> [outdir]\n", argv[0]);
		return 1;
	}
	const char *rom_path = argv[1];
	int total_frames = atoi(argv[2]);
	int dump_every = atoi(argv[3]);
	const char *outdir = argc > 4 ? argv[4] : "hosttest/out";
	mkdir(outdir, 0755);

	int press_run = -1;
	if (getenv("PCE_PRESS_RUN"))
		press_run = atoi(getenv("PCE_PRESS_RUN"));
	if (getenv("PCE_SOLO_VDC"))
		pce_dbg_solo_vdc = atoi(getenv("PCE_SOLO_VDC"));

	// PCE_FREEZE_DETECT=N — flag any run of >=N consecutive identical hashed
	// framebuffers as a freeze. Prints once at threshold crossing and once on
	// recovery. 0 (default) disables. PCE_TRACE_CD=1 prints per-frame
	// data-sector read count + first LBA via cd.c's CD_DEBUG_READ counters.
	int freeze_detect = getenv("PCE_FREEZE_DETECT")
		? atoi(getenv("PCE_FREEZE_DETECT")) : 0;
	int trace_cd = getenv("PCE_TRACE_CD") ? atoi(getenv("PCE_TRACE_CD")) : 0;

	// PCE_PRESS_KEYS=<frame>:<hex>[,<frame>:<hex>...] — hold the given JOY_* mask
	// for 10 frames at each frame, in addition to PCE_PRESS_RUN/PCE_HOLD_FIRE.
	struct { int frame; uint8_t mask; } keys[16];
	int keys_n = 0;
	if (getenv("PCE_PRESS_KEYS")) {
		char buf[256];
		strncpy(buf, getenv("PCE_PRESS_KEYS"), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		char *tok = strtok(buf, ",");
		while (tok && keys_n < 16) {
			char *colon = strchr(tok, ':');
			if (colon) {
				*colon = 0;
				keys[keys_n].frame = atoi(tok);
				keys[keys_n].mask = (uint8_t)strtoul(colon + 1, NULL, 16);
				keys_n++;
			}
			tok = strtok(NULL, ",");
		}
	}

	const char *ext = strrchr(rom_path, '.');
	int is_sgx = ext && strcasecmp(ext, ".sgx") == 0;
	int is_cd  = ext && (strcasecmp(ext, ".cue") == 0 || strcasecmp(ext, ".chd") == 0);

	SetSgxModePCE(is_sgx);
	if (InitPCE(44100, true)) { fprintf(stderr, "InitPCE failed\n"); return 1; }

	if (is_cd) {
		// CD path: LoadDisc reads the CUE/CHD via the FatFs shim (which maps
		// absolute paths under $PCE_SD_ROOT — default ".") and scans /bios/
		// for a System Card. The path argv[1] passes through verbatim and
		// becomes the FatFs path; if it's absolute, $PCE_SD_ROOT applies.
		int rc = LoadDisc(rom_path);
		if (rc != 0) { fprintf(stderr, "LoadDisc(%s) failed (rc=%d)\n", rom_path, rc); return 1; }
	} else {
		FILE *f = fopen(rom_path, "rb");
		if (!f) { perror(rom_path); return 1; }
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *rom = malloc(size);
		if (fread(rom, 1, size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
		fclose(f);
		if (LoadCard(rom, size)) { fprintf(stderr, "LoadCard failed\n"); return 1; }
	}

	// PCE_QUIRK=<hex> ORs bits into PCE.Quirks (force-on a quirk that the
	// CRC match didn't set). PCE_QUIRK_CLEAR=<hex> masks bits out (turn off
	// a quirk that the CRC match did set, e.g. to isolate which sub-bit of
	// PCE_QUIRK_HW_VDC causes a regression). On Pico the CRC is real and
	// these env vars are not consulted.
	if (getenv("PCE_QUIRK"))
		PCE.Quirks |= strtoul(getenv("PCE_QUIRK"), NULL, 0);
	if (getenv("PCE_QUIRK_CLEAR"))
		PCE.Quirks &= ~strtoul(getenv("PCE_QUIRK_CLEAR"), NULL, 0);

	printf("Loaded %s (sgx=%d, cd=%d, quirks=0x%x). Running %d frames.\n",
		rom_path, is_sgx, is_cd, (unsigned)PCE.Quirks, total_frames);

	for (int frame = 0; frame < total_frames; frame++) {
		if (press_run >= 0) {
			if (frame >= press_run && frame < press_run + 10)
				PCE.Joypad.regs[0] = JOY_RUN;
			else
				PCE.Joypad.regs[0] = 0;
		}
		// Autofire button II from PCE_HOLD_FIRE onward (4 frames on / 4 off).
		static int hold_fire = -2;
		if (hold_fire == -2)
			hold_fire = getenv("PCE_HOLD_FIRE") ? atoi(getenv("PCE_HOLD_FIRE")) : -1;
		if (hold_fire >= 0 && frame >= hold_fire && (frame & 4))
			PCE.Joypad.regs[0] |= JOY_A;
		for (int k = 0; k < keys_n; k++) {
			if (frame >= keys[k].frame && frame < keys[k].frame + 10)
				PCE.Joypad.regs[0] |= keys[k].mask;
		}
		pce_run();
		// CD-DA / ADPCM keepalive. The generators advance audio_cur_lba,
		// fire end-of-track IRQs, drive the $1805 sample latch, and tick
		// the ADPCM playing flag. Games hang waiting on those side
		// effects if we never call them. Samples themselves go nowhere
		// (no audio sink); 44100/60 ≈ 735 samples/frame matches firmware.
		if (CD.cd_attached) {
			static int16_t cd_buf[735 * 2];
			cd_audio_update();
			cd_audio_generate_samples(cd_buf, 735);
			cd_adpcm_generate_samples(cd_buf, 735, 44100);
		}

		if (trace_cd) {
			uint32_t reads = 0, first_lba = 0;
			cd_get_and_clear_reads_this_frame(&reads, &first_lba);
			if (reads)
				printf("[cd-trace] frame=%05d reads=%u first_lba=%u\n",
					frame, reads, first_lba);
		}

#if GFX_DEBUG_LOAD
		// Render-load proxy: sprite scanline-rows rasterized this frame. High
		// values are the "intensive action" frames that trip the Pico's
		// frameskip (frameWorkUs overrun) and stall the picture on hardware.
		extern uint32_t gfx_sprite_rows_this_frame;
		extern uint32_t gfx_sphit_en_this_frame, gfx_sphit_true_this_frame;
		if (getenv("PCE_TRACE_LOAD")) {
			printf("[load] frame=%05d sprite_rows=%u sphit_en=%u sphit_hit=%u\n",
				frame, gfx_sprite_rows_this_frame,
				gfx_sphit_en_this_frame, gfx_sphit_true_this_frame);
		}
		gfx_sprite_rows_this_frame = 0;
		gfx_sphit_en_this_frame = 0;
		gfx_sphit_true_this_frame = 0;
#endif

		// Per-frame: clear+capture CD-port read histogram so we can attribute
		// it to the freeze trigger when one fires. Zero overhead when
		// CD_DEBUG_IO is off (the underlying counters are inert).
		uint8_t  hot_addr[3]  = {0,0,0};
		uint32_t hot_count[3] = {0,0,0};
		if (CD.cd_attached)
			cd_get_and_clear_io_hotspots(hot_addr, hot_count, 3);

		if (freeze_detect > 0) {
			static uint32_t prev_hash = 0;
			static int dupe_run = 0;
			static int reported = 0;
			int hw = PCE.VDC.screen_width > 0 ? PCE.VDC.screen_width : 256;
			if (hw > XBUF_WIDTH) hw = XBUF_WIDTH;
			uint32_t h = fb_hash(hw);
			if (frame > 0 && h == prev_hash) {
				dupe_run++;
				if (dupe_run == freeze_detect) {
					// Dump 16 bytes starting at PC-2 so we see the LDA opcode
					// of the poll loop (PC sample is typically at the BNE).
					uint16_t base = (CPU.PC >= 2) ? (CPU.PC - 2) : 0;
					char dump[64]; dump[0] = 0;
					int off = 0;
					for (int i = 0; i < 16 && off < (int)sizeof(dump) - 4; i++) {
						uint8_t *pg = PageR[(base + i) >> 13];
						uint8_t b = (pg && pg != PCE.IOAREA)
							? pg[(uint16_t)(base + i)] : 0xFF;
						off += snprintf(dump + off, sizeof(dump) - off,
							"%02X ", b);
					}
					printf("[freeze] frame=%05d dupe_run=%d "
						"pc=$%04X halted=%u "
						"adpcm: play=%u len=$%04X mask=$%02X ai=$%02X "
						"audio_st=%u "
						"io: $%02X=%u $%02X=%u $%02X=%u  "
						"@%04X: %s\n",
						frame, dupe_run,
						CPU.PC, (unsigned)CPU.halted,
						(unsigned)CD.adpcm_playing,
						(unsigned)CD.adpcm_length,
						(unsigned)CD.irq_mask,
						(unsigned)(CD.irq_status),
						(unsigned)CD.audio_status,
						hot_addr[0], hot_count[0],
						hot_addr[1], hot_count[1],
						hot_addr[2], hot_count[2],
						base, dump);
					reported = 1;
				}
			} else {
				if (reported)
					printf("[freeze-end] frame=%05d dupe_run=%d\n",
						frame, dupe_run);
				dupe_run = 0;
				reported = 0;
			}
			prev_hash = h;
		}

		if (dump_every > 0 && frame % dump_every == 0)
			dump_ppm(outdir, frame);
		if (getenv("PCE_DUMP_REGS") && frame % 100 == 0) {
			int nz1 = 0, nz2 = 0;
			for (int i = 0; i < 0x8000; i++) {
				if (PCE.VRAM[i]) nz1++;
				if (PCE.VRAM2 && PCE.VRAM2[i]) nz2++;
			}
			printf("F%04d VPR1=%04X(min=%d) VDW1=%04X VPR2=%04X VDW2=%04X | ",
				frame, PCE.VDC.regs[VPR].W,
				PCE.VDC.regs[VPR].B.h + PCE.VDC.regs[VPR].B.l,
				PCE.VDC.regs[VDW].W, PCE.VDC2.regs[VPR].W, PCE.VDC2.regs[VDW].W);
			printf("F%04d VDC1: CR=%04X BXR=%04X BYR=%04X MWR=%04X SATB=%04X DCR=%04X | "
				"VDC2: CR=%04X BXR=%04X BYR=%04X MWR=%04X SATB=%04X DCR=%04X | "
				"VPC: pri=%02X/%02X w1=%03X w2=%03X cfg=%X%X%X%X st2=%d | vramNZ=%d/%d\n",
				frame,
				PCE.VDC.regs[CR].W, PCE.VDC.regs[BXR].W, PCE.VDC.regs[BYR].W,
				PCE.VDC.regs[MWR].W, PCE.VDC.regs[SATB].W, PCE.VDC.regs[DCR].W,
				PCE.VDC2.regs[CR].W, PCE.VDC2.regs[BXR].W, PCE.VDC2.regs[BYR].W,
				PCE.VDC2.regs[MWR].W, PCE.VDC2.regs[SATB].W, PCE.VDC2.regs[DCR].W,
				PCE.VPC.priority1, PCE.VPC.priority2,
				PCE.VPC.window1, PCE.VPC.window2,
				PCE.VPC.window_cfg[0], PCE.VPC.window_cfg[1],
				PCE.VPC.window_cfg[2], PCE.VPC.window_cfg[3],
				PCE.VPC.st_to_vdc2, nz1, nz2);
		}
	}
	dump_ppm(outdir, total_frames);
	if (getenv("PCE_DUMP_VRAM")) {
		char p[512];
		snprintf(p, sizeof(p), "%s/vram1.bin", outdir);
		FILE *d = fopen(p, "wb"); fwrite(PCE.VRAM, 2, 0x8000, d); fclose(d);
		if (PCE.VRAM2) {
			snprintf(p, sizeof(p), "%s/vram2.bin", outdir);
			d = fopen(p, "wb"); fwrite(PCE.VRAM2, 2, 0x8000, d); fclose(d);
		}
		snprintf(p, sizeof(p), "%s/spram1.bin", outdir);
		d = fopen(p, "wb"); fwrite(PCE.SPRAM, 2, 256, d); fclose(d);
		snprintf(p, sizeof(p), "%s/spram2.bin", outdir);
		d = fopen(p, "wb"); fwrite(PCE.SPRAM2, 2, 256, d); fclose(d);
	}
	if (CD.cd_attached)
		cd_term();
	printf("done.\n");
	return 0;
}
