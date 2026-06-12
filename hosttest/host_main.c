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

	const char *ext = strrchr(rom_path, '.');
	int is_sgx = ext && strcasecmp(ext, ".sgx") == 0;

	FILE *f = fopen(rom_path, "rb");
	if (!f) { perror(rom_path); return 1; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *rom = malloc(size);
	if (fread(rom, 1, size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
	fclose(f);

	SetSgxModePCE(is_sgx);
	if (InitPCE(44100, true)) { fprintf(stderr, "InitPCE failed\n"); return 1; }
	if (LoadCard(rom, size)) { fprintf(stderr, "LoadCard failed\n"); return 1; }

	printf("ROM %s loaded (%ld bytes, sgx=%d). Running %d frames.\n",
		rom_path, size, is_sgx, total_frames);

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
		pce_run();
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
	printf("done.\n");
	return 0;
}
