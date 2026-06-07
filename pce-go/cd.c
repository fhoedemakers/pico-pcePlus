// cd.c - PC Engine CD-ROM + Arcade Card subsystem
//
// Step 1: lifecycle stubs (cd_init/reset/term).
// Step 2: CRC-based BIOS discovery, CUE parser, BIN file handle management.
//
// Real CDC / SCSI / Arcade Card register handling, sector I/O, and BRAM
// persistence are filled in by later steps of the implementation plan.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "pico.h"
#include "pico/mutex.h"
#include "ff.h"

#include "pce-go.h"
#include "pce.h"
#include "cd.h"
#include "cd_chd.h"

// Persistent BIN file handle, allocated from PSRAM while a disc is open.
static FIL *bin_fil = NULL;
static char current_bin_name[CD_BIN_NAME_MAX];

// Separate file handle for CD audio streaming (independent of data reads).
static FIL *audio_fil = NULL;
static char audio_bin_name[CD_BIN_NAME_MAX];

// C-side allocator wrappers from pico_shared/FrensHelpers.cpp. They route to
// PSRAM via lwmem when available and fall back to plain malloc otherwise.
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

// Global CD state (zero-initialized).
cd_state_t CD;

// Mutex protecting all FatFS calls so core1 can do audio reads safely.
static mutex_t sd_mutex;
static bool sd_mutex_inited = false;

// SCSI bus signal state
static bool scsi_req = false;
static bool scsi_ack = false;
static uint8_t scsi_data_port = 0;
static uint8_t scsi_read_port = 0;
static bool    scsi_msg_done  = false;
static uint8_t cdc_active_irqs = 0;
static uint8_t cdc_reset_reg = 0;

// ---------------------------------------------------------------------------
// CRC32 (polynomial 0xEDB88320, standard / "le" variant). Self-contained so
// cd.c does not need to link against the C++ crc32 helper in pico_shared.
// ---------------------------------------------------------------------------

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = -(int32_t)(crc & 1);
			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

// ---------------------------------------------------------------------------
// Known BIOS database. CRC32 values reflect commonly-cited hashes for PCE/
// TG-CD BIOS dumps. If a user-supplied BIOS has a different CRC it will be
// logged as "unknown" and we fall back to System Card 3.0 behaviour — the
// printed CRC can be added to this table later.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t       crc;
	const char    *name;
	bios_variant_t variant;
	uint8_t        priority;   // higher wins when multiple BIOS files present
	bool           is_us;
} bios_entry_t;

// CRCs below are from Redump-style dumps; values marked "unverified" were not
// available in any of the test sets seen so far and may still be wrong — if
// you have one and it shows as "unknown" in serial output, paste the printed
// CRC in here.
static const bios_entry_t known_bios[] = {
	// Arcade Card variants (top priority — supersets of SC3)
	{ 0x1F240E6E, "Arcade Card Pro (JP)",            BIOS_ACD_PRO, 100, false }, // unverified
	{ 0x8C4588E2, "Arcade Card Duo (JP)",            BIOS_ACD_DUO,  90, false }, // unverified

	// Super System Card 3.0 (preferred when no Arcade Card)
	{ 0x6D9A73EF, "Super CD-ROM System (JP, v3.0)",  BIOS_SC3,      80, false },
	{ 0x2B5B75FE, "TG-CD Super System Card (US)",    BIOS_SC3,      80, true  },

	// Games Express CD Card (separate ecosystem)
	{ 0x51A12D90, "Games Express CD Card (JP)",      BIOS_GEX,      70, false },
	{ 0x9D1E81B8, "Games Express CD Card (JP, Alt)", BIOS_GEX,      70, false },

	// Original System Card 1.x / 2.x (fewer features)
	{ 0x283B74E0, "CD-ROM System (JP, v2.1)",        BIOS_SC2,      60, false },
	{ 0x52520BC6, "CD-ROM System (JP, v2.0)",        BIOS_SC2,      60, false },
	{ 0xFF2A5EC3, "TurboGrafx CD System (US, v2.0)", BIOS_SC2,      60, true  },
	{ 0x3F9F95A4, "CD-ROM System (JP, v1.0)",        BIOS_SC1,      40, false },
};
#define KNOWN_BIOS_COUNT (sizeof(known_bios) / sizeof(known_bios[0]))

static const bios_entry_t *lookup_bios(uint32_t crc)
{
	for (size_t i = 0; i < KNOWN_BIOS_COUNT; i++) {
		if (known_bios[i].crc == crc)
			return &known_bios[i];
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int
cd_init(void)
{
	memset(&CD, 0, sizeof(CD));
	CD.audio_status = 1; // Inactive
	if (!sd_mutex_inited) {
		mutex_init(&sd_mutex);
		sd_mutex_inited = true;
	}
	return 0;
}

void
cd_reset(void)
{
	if (!CD.cd_attached)
		return;

	CD.phase = CDC_PHASE_IDLE;
	CD.scsi_cmd_len = 0;
	CD.scsi_cmd_idx = 0;
	CD.data_index = 0;
	CD.data_length = 0;
	CD.read_remaining = 0;
	CD.bram_locked = true;
	CD.audio_status = 1;   // Inactive
	CD.audio_start_lba = 0;
	CD.audio_end_lba = 0;
	CD.audio_cur_lba = 0;
	CD.audio_cur_sample = 0;
	CD.audio_end_mode = 0;
	CD.audio_ring_write = 0;
	CD.audio_ring_read = 0;
	CD.audio_ring_count = 0;
	CD.irq_status = 0;
	CD.sense_key = 0;
	CD.sense_asc = 0;
	CD.sense_ascq = 0;

	CD.irq_mask = 0;

	scsi_req = false;
	scsi_ack = false;
	scsi_data_port = 0;
	scsi_read_port = 0;
	scsi_msg_done = false;
	cdc_active_irqs = 0;
	cdc_reset_reg = 0;

	CD.adpcm_ctrl = 0;
	CD.adpcm_dma_ctrl = 0;
	CD.adpcm_status = 0;
	CD.adpcm_rate = 0;
	CD.adpcm_fade = 0;
	CD.adpcm_addr_port = 0;
	CD.adpcm_read_addr = 0;
	CD.adpcm_write_addr = 0;
	CD.adpcm_length = 0;
	CD.adpcm_read_buf = 0;
	CD.adpcm_playing = 0;
	CD.adpcm_nibble = 0;
	CD.adpcm_magnitude = 0;
	CD.adpcm_cur_output = 2048;
	CD.adpcm_play_addr = 0;
	CD.adpcm_play_len = 0;
	CD.adpcm_resample_acc = 0;

	memset(CD.acd_port, 0, sizeof(CD.acd_port));
	CD.acd_value = 0;
	CD.acd_shift = 0;
	CD.acd_rotate = 0;
}

void
cd_term(void)
{
	bool was_attached = CD.cd_attached;

	cd_close();

	if (CD.cd_ram)         { frens_f_free(CD.cd_ram);         CD.cd_ram = NULL; }
	if (CD.scd_ram)        { frens_f_free(CD.scd_ram);        CD.scd_ram = NULL; }
	if (CD.adpcm_ram)      { frens_f_free(CD.adpcm_ram);      CD.adpcm_ram = NULL; }
	if (CD.acd_ram)        { frens_f_free(CD.acd_ram);        CD.acd_ram = NULL; }
	if (CD.bram)           { frens_f_free(CD.bram);           CD.bram = NULL; }
	// audio_ring_buf is a static SRAM array — don't free it

	// LoadDisc() allocated PCE.ROM via frens_f_malloc (PSRAM), so we own
	// that buffer when a CD was attached. Releasing it here — and clearing
	// PCE.ROM — keeps the subsequent free(PCE.ROM) inside pce_term() safe.
	if (was_attached && PCE.ROM) {
		frens_f_free(PCE.ROM);
		PCE.ROM = NULL;
		PCE.ROM_DATA = NULL;
		PCE.ROM_SIZE = 0;
	}

	CD.cd_attached = false;
	CD.bios_variant = BIOS_UNKNOWN;
	CD.bios_is_us = false;
}

// ---------------------------------------------------------------------------
// BIOS discovery: scan /bios/, CRC each .pce, pick highest-priority match.
// ---------------------------------------------------------------------------

static bool has_pce_extension(const char *name)
{
	size_t n = strlen(name);
	if (n < 4) return false;
	if (strcasecmp(name + n - 4, ".pce") == 0) return true;
	// Common alternate naming for system-card BIOS dumps shipped alongside
	// CUE+BIN / CHD images: accept "cd_bios.rom" (case-insensitive) as a
	// per-game BIOS so users can drop one into the disc folder without
	// renaming. CRC lookup still decides which BIOS variant it is.
	if (strcasecmp(name, "cd_bios.rom") == 0) return true;
	return false;
}

static uint32_t crc32_of_file(const char *path)
{
	// Both buffers are static: the chunked read buffer is too large for the
	// 3 KB stack configured in CMakeLists.txt (PICO_STACK_SIZE=3072), and
	// FatFS's FIL is itself ~600 bytes — combined with the caller's DIR /
	// FILINFO / path locals, a stack-resident buf[4096] would overflow and
	// corrupt SRAM (we saw a core1 hardfault from exactly that).
	// CRC computation is single-threaded, so static is safe.
	static FIL     fil;
	static uint8_t buf[512];

	if (f_open(&fil, path, FA_READ) != FR_OK)
		return 0;

	uint32_t crc = 0;
	UINT br;
	while (f_read(&fil, buf, sizeof(buf), &br) == FR_OK && br > 0) {
		crc = crc32_update(crc, buf, br);
	}
	f_close(&fil);
	return crc;
}

// Scan a single directory for .pce files and pick the best BIOS in it. dir_in
// is a path WITH trailing '/' (so we can snprintf "%s%s" against fname). On
// success writes path into out_path and updates CD.bios_variant/is_us; returns
// 0. Returns -1 if the dir cannot be opened or contains no .pce files.
//
// All FatFS-heavy locals are static: a DIR is ~570 B and a FILINFO is ~278 B
// on this FatFS configuration (FF_MAX_LFN=255), and several FF_MAX_LFN-sized
// path buffers compound the problem. With PICO_STACK_SIZE=3072 the deep call
// chain from main would otherwise overflow into core1's SCRATCH region.
// cd_find_bios is single-threaded and only runs at boot / on disc load.
static int scan_bios_dir(const char *dir_in,
                         char *out_path, size_t out_size,
                         bios_variant_t *out_variant)
{
	static DIR     dir;
	static FILINFO fno;
	static char    best_path[FF_MAX_LFN + 8];
	static char    fallback_path[FF_MAX_LFN + 8];
	static char    full[FF_MAX_LFN + 8];
	static char    open_path[FF_MAX_LFN + 8];

	best_path[0] = fallback_path[0] = '\0';

	// f_opendir wants a path WITHOUT a trailing slash (except for root).
	size_t L = strlen(dir_in);
	if (L == 0) return -1;
	strncpy(open_path, dir_in, sizeof(open_path) - 1);
	open_path[sizeof(open_path) - 1] = '\0';
	size_t n = strlen(open_path);
	if (n > 1 && open_path[n - 1] == '/') open_path[n - 1] = '\0';

	printf("cd_find_bios: scanning %s for BIOS files\n", open_path);

	if (f_opendir(&dir, open_path) != FR_OK) {
		printf("cd_find_bios: %s does not exist or cannot be opened\n",
		       open_path);
		return -1;
	}

	int best_priority = -1;
	uint32_t best_crc = 0;
	const bios_entry_t *best_entry = NULL;
	uint32_t fallback_crc = 0;
	int candidates = 0;

	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
		if (fno.fattrib & (AM_DIR | AM_HID))
			continue;
		if (!has_pce_extension(fno.fname))
			continue;

		candidates++;
		snprintf(full, sizeof(full), "%s%s", dir_in, fno.fname);

		uint32_t crc = crc32_of_file(full);
		const bios_entry_t *e = lookup_bios(crc);

		if (e) {
			printf("cd_find_bios: %s  CRC=0x%08lX  -> %s\n",
			       full, (unsigned long)crc, e->name);
			if ((int)e->priority > best_priority) {
				best_priority = e->priority;
				strncpy(best_path, full, sizeof(best_path) - 1);
				best_path[sizeof(best_path) - 1] = '\0';
				best_crc = crc;
				best_entry = e;
			}
		} else {
			printf("cd_find_bios: %s  CRC=0x%08lX  -> unknown\n",
			       full, (unsigned long)crc);
			if (!fallback_path[0]) {
				strncpy(fallback_path, full, sizeof(fallback_path) - 1);
				fallback_path[sizeof(fallback_path) - 1] = '\0';
				fallback_crc = crc;
			}
		}
	}
	f_closedir(&dir);

	if (best_entry) {
		strncpy(out_path, best_path, out_size - 1);
		out_path[out_size - 1] = '\0';
		if (out_variant) *out_variant = best_entry->variant;
		CD.bios_variant = best_entry->variant;
		CD.bios_is_us = best_entry->is_us;
		printf("cd_find_bios: selected %s (CRC=0x%08lX, %s, region=%s)\n",
		       best_path, (unsigned long)best_crc, best_entry->name,
		       best_entry->is_us ? "US" : "JP");
		return 0;
	}

	if (fallback_path[0]) {
		strncpy(out_path, fallback_path, out_size - 1);
		out_path[out_size - 1] = '\0';
		if (out_variant) *out_variant = BIOS_UNKNOWN;
		CD.bios_variant = BIOS_UNKNOWN;
		CD.bios_is_us = false;
		printf("cd_find_bios: no known BIOS in %s, falling back to %s "
		       "(CRC=0x%08lX, assuming System Card 3.0 / JP)\n",
		       open_path, fallback_path, (unsigned long)fallback_crc);
		return 0;
	}

	printf("cd_find_bios: no .pce files found in %s (%d entries scanned)\n",
	       open_path, candidates);
	return -1;
}

int
cd_find_bios(char *out_path, size_t size, const char *primary_dir,
             bios_variant_t *out_variant)
{
	if (!out_path || size < 32)
		return -1;

	// Per-game override: if the CUE's own folder contains a BIOS, use it
	// (lets users ship a region-specific BIOS alongside a patched game
	// without touching /bios/). Falls through to /bios/ when the primary
	// dir has no .pce files at all.
	if (primary_dir && primary_dir[0]) {
		printf("cd_find_bios: trying CUE folder %s first\n", primary_dir);
		if (scan_bios_dir(primary_dir, out_path, size, out_variant) == 0) {
			printf("cd_find_bios: using BIOS from CUE folder (skipping /bios/)\n");
			return 0;
		}
		printf("cd_find_bios: no BIOS in %s, falling back to /bios/\n",
		       primary_dir);
	} else {
		printf("cd_find_bios: no CUE folder provided, scanning /bios/ only\n");
	}

	if (scan_bios_dir("/bios/", out_path, size, out_variant) == 0)
		return 0;

	printf("cd_find_bios: FAILED — no usable BIOS in CUE folder or /bios/\n");
	if (out_variant) *out_variant = BIOS_UNKNOWN;
	return -1;
}

bool
cd_bios_is_us(void)
{
	return CD.cd_attached && CD.bios_is_us;
}

// ---------------------------------------------------------------------------
// CUE/BIN parsing
// ---------------------------------------------------------------------------

// Strip directory component from a path (in place). Returns pointer to the
// path (now containing only the directory part, with trailing slash). If the
// path has no directory, sets path[0] to '\0'.
static void path_dirname(char *path)
{
	char *slash = strrchr(path, '/');
	if (slash) {
		slash[1] = '\0';   // keep the trailing slash
	} else {
		path[0] = '\0';
	}
}

// Skip leading whitespace.
static char *ltrim(char *s)
{
	while (*s && isspace((unsigned char)*s)) s++;
	return s;
}

// Parse MM:SS:FF (Minutes:Seconds:Frames @ 75fps) into LBA.
static int parse_msf(const char *s, uint32_t *out_lba)
{
	int m, sec, f;
	if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3)
		return -1;
	if (sec < 0 || sec >= 60 || f < 0 || f >= 75 || m < 0)
		return -1;
	*out_lba = (uint32_t)(m * 60 * 75 + sec * 75 + f);
	return 0;
}

// Extract a quoted filename from a `FILE "name.bin" BINARY` line.
static int extract_filename(const char *line, char *out, size_t size)
{
	const char *p = strchr(line, '"');
	if (!p) return -1;
	p++;
	const char *q = strchr(p, '"');
	if (!q) return -1;
	size_t n = (size_t)(q - p);
	if (n + 1 > size) return -1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 0;
}

// Open a BIN file (relative to CD.cue_dir) and return its size via *out_size.
// Returns 0 on success, -1 on failure. Used only by cd_load_cue's fixup pass.
// Probe a BIN/WAV file: returns its total size, and — for canonical RIFF/WAVE
// files — the byte offset of the PCM data chunk's payload (so audio reads
// skip the RIFF/fmt/etc. header bytes instead of playing them as samples).
// Returns 0 in *out_wav_offset for non-WAV files, leaving callers free to use
// it as an unconditional bias on bin_offset.
static int probe_bin_file(const char *bin_name, FSIZE_t *out_size, uint32_t *out_wav_offset)
{
	static FIL  s_fil;
	static char s_path[256];
	if (snprintf(s_path, sizeof(s_path), "%s%s", CD.cue_dir, bin_name)
	        >= (int)sizeof(s_path))
		return -1;
	if (f_open(&s_fil, s_path, FA_READ) != FR_OK)
		return -1;
	*out_size = f_size(&s_fil);
	if (out_wav_offset) *out_wav_offset = 0;

	// Try to identify a RIFF/WAVE container and walk its chunks to find the
	// PCM payload. A canonical CD-ripped WAV has a 44-byte header (RIFF+WAVE
	// + fmt + data), but extra chunks (LIST/INFO, JUNK, PEAK, …) can push
	// the payload further in. Walk until we find "data" or run out of file.
	if (out_wav_offset && *out_size >= 12) {
		uint8_t hdr[12];
		UINT br = 0;
		if (f_read(&s_fil, hdr, 12, &br) == FR_OK && br == 12 &&
		    hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
		    hdr[8] == 'W' && hdr[9] == 'A' && hdr[10] == 'V' && hdr[11] == 'E') {
			uint32_t pos = 12;
			for (int i = 0; i < 32 && pos + 8 <= *out_size; i++) {
				uint8_t ck[8];
				if (f_lseek(&s_fil, pos) != FR_OK) break;
				if (f_read(&s_fil, ck, 8, &br) != FR_OK || br != 8) break;
				uint32_t ck_size = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
				                   ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
				if (ck[0] == 'd' && ck[1] == 'a' && ck[2] == 't' && ck[3] == 'a') {
					*out_wav_offset = pos + 8;
					break;
				}
				// Chunks are word-aligned: pad odd sizes up to even.
				pos += 8 + ck_size + (ck_size & 1);
			}
		}
	}

	f_close(&s_fil);
	return 0;
}

int
cd_load_cue(const char *cue_path)
{
	if (!cue_path) return -1;

	// Static for stack safety (FIL ~600 B + line buffer ~256 B).
	static FIL  cue;
	static char line[256];
	static char cur_bin[CD_BIN_NAME_MAX];   // current FILE during parse

	if (f_open(&cue, cue_path, FA_READ) != FR_OK) {
		printf("cd_load_cue: cannot open %s\n", cue_path);
		return -1;
	}

	memset(CD.tracks, 0, sizeof(CD.tracks));
	CD.num_tracks  = 0;
	CD.first_track = 0;
	CD.last_track  = 0;
	CD.total_lba   = 0;
	cur_bin[0]     = '\0';
	int current_track = -1;

	// --- Parse pass ---
	// We accept CUE files with either a single FILE covering all tracks
	// (older / hand-crafted dumps) or one FILE per track (Redump style).
	// INDEX 01 timestamps are recorded as-is into file_lba — they are LBAs
	// *within the current FILE*, not absolute disc LBAs. The fixup pass
	// below converts them to disc-level lba_start / lba_end.
	while (f_gets(line, sizeof(line), &cue)) {
		char *p = ltrim(line);

		if (strncasecmp(p, "FILE", 4) == 0) {
			if (extract_filename(p, cur_bin, sizeof(cur_bin)) != 0) {
				printf("cd_load_cue: bad FILE line: %s", line);
				f_close(&cue);
				return -1;
			}
		} else if (strncasecmp(p, "TRACK", 5) == 0) {
			int trk; char type[32] = {0};
			if (sscanf(p + 5, " %d %31s", &trk, type) != 2) continue;
			if (CD.num_tracks >= CD_MAX_TRACKS) {
				printf("cd_load_cue: too many tracks (>%d)\n", CD_MAX_TRACKS);
				break;
			}
			if (cur_bin[0] == '\0') {
				printf("cd_load_cue: TRACK before any FILE\n");
				f_close(&cue);
				return -1;
			}
			current_track = CD.num_tracks++;
			cd_track_t *t = &CD.tracks[current_track];
			t->track_no = (uint8_t)trk;
			strncpy(t->bin_name, cur_bin, sizeof(t->bin_name) - 1);
			t->bin_name[sizeof(t->bin_name) - 1] = '\0';

			if (strcasecmp(type, "AUDIO") == 0) {
				t->type = 0; t->sector_size = 1;
			} else if (strcasecmp(type, "MODE1/2048") == 0) {
				t->type = 1; t->sector_size = 0;
			} else if (strcasecmp(type, "MODE1/2352") == 0 ||
			           strcasecmp(type, "MODE2/2352") == 0) {
				t->type = 1; t->sector_size = 1;
			} else {
				printf("cd_load_cue: unknown TRACK type '%s', assuming MODE1/2352\n", type);
				t->type = 1; t->sector_size = 1;
			}

			if (CD.first_track == 0 || trk < CD.first_track) CD.first_track = (uint8_t)trk;
			if (trk > CD.last_track) CD.last_track = (uint8_t)trk;
		} else if (strncasecmp(p, "INDEX", 5) == 0) {
			int idx; char msf[32] = {0};
			if (sscanf(p + 5, " %d %31s", &idx, msf) != 2) continue;
			if (idx != 1 || current_track < 0) continue;  // we only care about INDEX 01
			uint32_t lba;
			if (parse_msf(msf, &lba) != 0) {
				printf("cd_load_cue: bad MSF: %s\n", msf);
				continue;
			}
			CD.tracks[current_track].file_lba = lba;
		} else if (strncasecmp(p, "PREGAP", 6) == 0) {
			// Pregap declared as a directive — no bytes in the BIN for it.
			// We must still account for the gap in disc-level LBAs so the
			// emulator's TOC matches what the BIOS expects. Without this,
			// audio plays seek into the *data* of the next track and lose
			// the first PREGAP seconds. The other CUE shape (pregap baked
			// into the BIN via INDEX 00 + INDEX 01) carries the offset in
			// file_lba and works without this directive.
			if (current_track < 0) continue;
			char msf[32] = {0};
			if (sscanf(p + 6, " %31s", msf) != 1) continue;
			uint32_t lba;
			if (parse_msf(msf, &lba) != 0) {
				printf("cd_load_cue: bad PREGAP MSF: %s\n", msf);
				continue;
			}
			CD.tracks[current_track].pregap_lbas = lba;
		}
	}
	f_close(&cue);

	if (CD.num_tracks == 0) {
		printf("cd_load_cue: no tracks found\n");
		return -1;
	}

	// Capture the CUE's directory (with trailing /) so subsequent BIN opens
	// can resolve relative filenames.
	{
		strncpy(CD.cue_dir, cue_path, sizeof(CD.cue_dir) - 1);
		CD.cue_dir[sizeof(CD.cue_dir) - 1] = '\0';
		path_dirname(CD.cue_dir);
	}

	// --- Fixup pass ---
	// Compute lba_start / lba_end / bin_offset for each track:
	//   - lba_start: running cumulative disc LBA, including pregap_lbas
	//     (PREGAP directive) and file_lba (pregap-in-BIN form). A given
	//     CUE uses only one form per track; the other is zero.
	//   - bin_offset: byte offset of track within its OWN BIN file:
	//     file_lba contributes (PREGAP directives reserve disc LBAs but
	//     consume no bytes in the BIN), plus a RIFF/WAVE header bias for
	//     .wav audio so playback skips header bytes instead of clicking.
	//   - lba_end:
	//       * if next track shares this BIN: lba_end = lba_start + delta(file_lba)
	//       * otherwise (last track in its BIN): derive length from BIN size
	uint32_t running_lba = 0;
	for (int i = 0; i < CD.num_tracks; i++) {
		cd_track_t *t = &CD.tracks[i];
		uint32_t raw = t->sector_size ? CD_RAW_SECTOR_SIZE : CD_SECTOR_SIZE;

		// Probe every track's BIN so we can bias bin_offset past any WAV
		// header. Also gives us the file size, used below to derive the
		// length for the last track in each BIN.
		FSIZE_t bin_size = 0;
		uint32_t wav_off = 0;
		bool have_bin = (probe_bin_file(t->bin_name, &bin_size, &wav_off) == 0);

		// In a single-BIN multi-track CUE, running_lba already covers the
		// previous track's bytes (we built it via lba_end = lba_start +
		// (next.file_lba - this.file_lba)), so running_lba already equals
		// this track's file_lba — adding file_lba again would double-count
		// and push every subsequent track further out of place. Only the
		// FIRST track in a new BIN file gets a file_lba contribution (the
		// position within that newly-opened file).
		const bool same_bin_prev =
			(i > 0) &&
			(strcmp(t->bin_name, CD.tracks[i - 1].bin_name) == 0);

		t->lba_start  = running_lba + t->pregap_lbas
		              + (same_bin_prev ? 0 : t->file_lba);
		t->bin_offset = (uint64_t)wav_off + (uint64_t)t->file_lba * raw;

		uint32_t length_lbas = 0;
		const bool same_bin_next =
			(i + 1 < CD.num_tracks) &&
			(strcmp(t->bin_name, CD.tracks[i + 1].bin_name) == 0);

		if (same_bin_next) {
			uint32_t next_fl = CD.tracks[i + 1].file_lba;
			if (next_fl >= t->file_lba)
				length_lbas = next_fl - t->file_lba;
		} else if (have_bin && bin_size > t->bin_offset) {
			length_lbas = (uint32_t)((bin_size - t->bin_offset) / raw);
		} else {
			printf("cd_load_cue: cannot probe BIN %s%s\n",
			       CD.cue_dir, t->bin_name);
		}

		t->lba_end  = t->lba_start + length_lbas;
		running_lba = t->lba_end;
	}
	CD.total_lba = running_lba;

	// --- Diagnostic dump ---
	printf("cd_load_cue: %s  cue_dir=%s  tracks=%u  total_lba=%lu (~%lu MB)\n",
	       cue_path, CD.cue_dir, CD.num_tracks,
	       (unsigned long)CD.total_lba,
	       (unsigned long)((uint64_t)CD.total_lba * CD_RAW_SECTOR_SIZE / (1024 * 1024)));
	for (int i = 0; i < CD.num_tracks; i++) {
		const cd_track_t *t = &CD.tracks[i];
		printf("  T%02u %s lba=[%6lu..%6lu) flba=%6lu pre=%4lu off=%9llu  %s\n",
		       t->track_no,
		       t->type ? "DATA " : "AUDIO",
		       (unsigned long)t->lba_start,
		       (unsigned long)t->lba_end,
		       (unsigned long)t->file_lba,
		       (unsigned long)t->pregap_lbas,
		       (unsigned long long)t->bin_offset,
		       t->bin_name);
	}

	return 0;
}

void
cd_close(void)
{
	if (bin_fil) {
		f_close(bin_fil);
		frens_f_free(bin_fil);
		bin_fil = NULL;
	}
	current_bin_name[0] = '\0';
	if (audio_fil) {
		f_close(audio_fil);
		frens_f_free(audio_fil);
		audio_fil = NULL;
	}
	audio_bin_name[0] = '\0';
	// CHD-backed disc: release libchdr's state + hunk cache.
	cd_chd_close();
	memset(CD.tracks, 0, sizeof(CD.tracks));
	CD.num_tracks = 0;
	CD.first_track = 0;
	CD.last_track = 0;
	CD.total_lba = 0;
}

// ---------------------------------------------------------------------------
// BIN file access (sector reads from disc image)
// ---------------------------------------------------------------------------

static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
	lba += 150;
	*f = lba % 75;
	*s = (lba / 75) % 60;
	*m = lba / 4500;
}

static int find_track(uint32_t lba)
{
	for (int i = 0; i < CD.num_tracks; i++) {
		if (lba >= CD.tracks[i].lba_start && lba < CD.tracks[i].lba_end)
			return i;
	}
	return -1;
}

static int scsi_cmd_length(uint8_t opcode)
{
	switch (opcode) {
	case 0x00: case 0x08:
		return 6;
	case 0x03:
		return 6;
	case 0xD8: case 0xD9: case 0xDA: case 0xDD: case 0xDE:
		return 10;
	default:
		return 0;
	}
}

static int open_bin_for_track(int idx)
{
	cd_track_t *t = &CD.tracks[idx];
	if (bin_fil && strcmp(current_bin_name, t->bin_name) == 0)
		return 0;
	if (bin_fil)
		f_close(bin_fil);
	else {
		bin_fil = frens_f_malloc(sizeof(FIL));
		if (!bin_fil) return -1;
	}
	static char path[256];
	snprintf(path, sizeof(path), "%s%s", CD.cue_dir, t->bin_name);
	if (f_open(bin_fil, path, FA_READ) != FR_OK) {
		printf("CDC: open failed: %s\n", path);
		current_bin_name[0] = '\0';
		return -1;
	}
	strncpy(current_bin_name, t->bin_name, CD_BIN_NAME_MAX - 1);
	current_bin_name[CD_BIN_NAME_MAX - 1] = '\0';
	return 0;
}

static int read_sector(uint32_t lba)
{
	int idx = find_track(lba);
	if (idx < 0) return -1;
	cd_track_t *t = &CD.tracks[idx];

	// CHD-backed disc: pull a raw 2352-byte sector through the hunk cache
	// (libchdr decompression) and skip the 16-byte sync header to give the
	// SCSI layer the same 2048-byte MODE1 payload it gets for CUE+BIN.
	if (CD.is_chd) {
		static uint8_t raw_sector[CD_RAW_SECTOR_SIZE];
		mutex_enter_blocking(&sd_mutex);
		int rc = cd_chd_read_raw_sector(lba, raw_sector);
		mutex_exit(&sd_mutex);
		if (rc != 0) return -1;
		memcpy(CD.sector_buf, raw_sector + 16, CD_SECTOR_SIZE);
		return 0;
	}

	if (open_bin_for_track(idx) != 0) return -1;

	uint32_t raw = t->sector_size ? CD_RAW_SECTOR_SIZE : CD_SECTOR_SIZE;
	uint64_t off = t->bin_offset + (uint64_t)(lba - t->lba_start) * raw;
	if (raw == CD_RAW_SECTOR_SIZE) off += 16;

	mutex_enter_blocking(&sd_mutex);
	int ok = (f_lseek(bin_fil, off) == FR_OK);
	UINT br = 0;
	if (ok)
		ok = (f_read(bin_fil, CD.sector_buf, CD_SECTOR_SIZE, &br) == FR_OK
		      && br == CD_SECTOR_SIZE);
	mutex_exit(&sd_mutex);
	return ok ? 0 : -1;
}

// ---------------------------------------------------------------------------
// SCSI command processing
// ---------------------------------------------------------------------------

static void cdc_enter_phase(cdc_phase_t phase);
static void cdc_set_good_status(void);
static void cdc_update_irqs(void);
static uint32_t parse_audio_lba(const uint8_t *cmd);
static void cd_audio_flush_ring(void);

static void cdc_data_in(const uint8_t *data, uint32_t len)
{
	if (len > CD_SECTOR_SIZE) len = CD_SECTOR_SIZE;
	memcpy(CD.sector_buf, data, len);
	CD.data_index  = 0;
	CD.data_length = len;
	CD.read_remaining = 0;
	cdc_enter_phase(CDC_PHASE_DATA_IN);
}

static void cdc_set_error(uint8_t key, uint8_t asc, uint8_t ascq)
{
	CD.sense_key  = key;
	CD.sense_asc  = asc;
	CD.sense_ascq = ascq;
	CD.data_index = CD.data_length = 0;
	CD.read_remaining = 0;
	cdc_enter_phase(CDC_PHASE_STATUS);
}

static void cdc_process_command(void)
{
	uint8_t cmd = CD.scsi_command[0];
	switch (cmd) {
	case 0x00: // TEST UNIT READY
		if (CD.num_tracks > 0) {
			cdc_set_good_status();
		} else {
			cdc_set_error(0x02, 0x3A, 0x00);
		}
		break;

	case 0x03: { // REQUEST SENSE
		uint8_t sense[18];
		memset(sense, 0, sizeof(sense));
		sense[0]  = 0x70;
		sense[2]  = CD.sense_key;
		sense[7]  = 0x0A;
		sense[12] = CD.sense_asc;
		sense[13] = CD.sense_ascq;
		CD.sense_key = CD.sense_asc = CD.sense_ascq = 0;
		cdc_data_in(sense, 18);
		break;
	}

	case 0x08: { // READ(6)
		uint32_t lba = ((CD.scsi_command[1] & 0x1F) << 16)
		             | (CD.scsi_command[2] << 8)
		             | CD.scsi_command[3];
		uint32_t count = CD.scsi_command[4];
		if (count == 0) {
			cdc_set_good_status();
			break;
		}

		if (read_sector(lba) != 0) {
			cdc_set_error(0x03, 0x11, 0x05);
			break;
		}
		CD.read_lba       = lba + 1;
		CD.read_remaining = count - 1;
		CD.data_index  = 0;
		CD.data_length = CD_SECTOR_SIZE;
		cdc_enter_phase(CDC_PHASE_DATA_IN);
		break;
	}

	case 0xD8: { // SAPSP — Set Audio Playback Start Position
		uint32_t lba = parse_audio_lba(CD.scsi_command);
		if (lba >= CD.total_lba) {
			cdc_set_good_status();
			break;
		}
		CD.audio_start_lba = lba;
		CD.audio_cur_lba   = lba;
		CD.audio_cur_sample = 0;
		cd_audio_flush_ring();
		if (CD.scsi_command[1] == 0) {
			CD.audio_status = 0; // Playing
		} else {
			CD.audio_status = 2; // Paused
		}
		cdc_set_good_status();
		break;
	}

	case 0xD9: { // SAPEP — Set Audio Playback End Position
		uint32_t lba = parse_audio_lba(CD.scsi_command);
		switch (CD.scsi_command[1]) {
		case 0:
			CD.audio_status = 3; // Stopped
			cdc_set_good_status();
			break;
		case 1:
			CD.audio_end_lba  = lba;
			CD.audio_end_mode = 1; // Loop
			CD.audio_status   = 0; // Playing
			CD.phase = CDC_PHASE_BUSY;
			break;
		case 2:
			CD.audio_end_lba  = lba;
			CD.audio_end_mode = 2; // IRQ
			CD.audio_status   = 0; // Playing
			CD.phase = CDC_PHASE_BUSY;
			break;
		case 3:
			CD.audio_end_lba  = lba;
			CD.audio_end_mode = 3; // Stop
			cdc_set_good_status();
			break;
		default:
			cdc_set_good_status();
			break;
		}
		break;
	}

	case 0xDA: // PAUSE
		CD.audio_status = 2; // Paused
		cdc_set_good_status();
		break;

	case 0xDD: { // READ SUBQ
		uint8_t subq[10];
		memset(subq, 0, sizeof(subq));
		subq[0] = CD.audio_status;
		int ti = find_track(CD.audio_cur_lba);
		if (ti < 0) ti = 0;
		cd_track_t *qt = &CD.tracks[ti];
		subq[1] = bin2bcd(qt->track_no);
		subq[2] = 0x01;
		uint32_t rel = CD.audio_cur_lba - qt->lba_start;
		subq[3] = bin2bcd(rel / 4500);
		subq[4] = bin2bcd((rel / 75) % 60);
		subq[5] = bin2bcd(rel % 75);
		uint8_t am, as, af;
		lba_to_msf(CD.audio_cur_lba, &am, &as, &af);
		subq[6] = bin2bcd(am);
		subq[7] = bin2bcd(as);
		subq[8] = bin2bcd(af);
		cdc_data_in(subq, 10);
		break;
	}

	case 0xDE: { // GET DIR INFO (TOC)
		uint8_t sub = CD.scsi_command[1];
		switch (sub) {
		case 0x00: { // number of tracks
			uint8_t r[4] = {
				1,
				bin2bcd(CD.num_tracks),
				0, 0
			};
			cdc_data_in(r, 4);
			break;
		}
		case 0x01: { // total disc length
			uint8_t m, s, f;
			lba_to_msf(CD.total_lba, &m, &s, &f);
			uint8_t r[4] = { bin2bcd(m), bin2bcd(s), bin2bcd(f), 0 };
			cdc_data_in(r, 4);
			break;
		}
		case 0x02: { // track N start time
			uint8_t raw = CD.scsi_command[2];
			int trk_no = (raw == 0xAA) ? 0xAA : bcd2bin(raw);
			if (trk_no == 0) trk_no = 1;

			uint8_t r[4];
			if (trk_no > CD.num_tracks) {
				uint8_t m, s, f;
				lba_to_msf(CD.total_lba, &m, &s, &f);
				r[0] = bin2bcd(m);
				r[1] = bin2bcd(s);
				r[2] = bin2bcd(f);
				r[3] = 0x04;
			} else {
				int found = -1;
				for (int i = 0; i < CD.num_tracks; i++)
					if (CD.tracks[i].track_no == trk_no)
						{ found = i; break; }
				if (found >= 0) {
					uint8_t m, s, f;
					lba_to_msf(CD.tracks[found].lba_start,
					           &m, &s, &f);
					r[0] = bin2bcd(m);
					r[1] = bin2bcd(s);
					r[2] = bin2bcd(f);
					r[3] = CD.tracks[found].type ? 0x04 : 0x00;
				} else {
					memset(r, 0, 4);
				}
			}
			cdc_data_in(r, 4);
			break;
		}
		default:
			cdc_set_error(0x05, 0x24, 0x00);
			break;
		}
		break;
	}

	default:
		cdc_set_good_status();
		break;
	}
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SCSI signal-based CDC model (matches Mesen2 / real HW behaviour)
//
// $1800 write = SEL pulse   → enter COMMAND phase
// $1801 write = data port   → CPU places data on SCSI bus
// $1802 write = ACK signal  → bit 7 drives REQ/ACK handshake;
//                              bits 0-6 = IRQ enable mask
// $1808 read  = data port with auto-ACK (DATA_IN bulk reads)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Arcade Card port helpers (matching Mesen2 PceArcadeCard)
// ---------------------------------------------------------------------------

static uint32_t acd_get_address(int p)
{
	uint32_t addr = CD.acd_port[p].base;
	if (CD.acd_port[p].add_offset) {
		addr += CD.acd_port[p].offset;
		if (CD.acd_port[p].sign_offset)
			addr += 0xFF0000;
	}
	return addr & 0x1FFFFF;
}

static void acd_auto_inc(int p)
{
	if (!CD.acd_port[p].auto_inc) return;
	if (CD.acd_port[p].inc_to_base)
		CD.acd_port[p].base = (CD.acd_port[p].base
		                     + CD.acd_port[p].increment) & 0xFFFFFF;
	else
		CD.acd_port[p].offset += CD.acd_port[p].increment;
}

static void acd_add_offset_to_base(int p)
{
	uint32_t a = CD.acd_port[p].base + CD.acd_port[p].offset;
	if (CD.acd_port[p].sign_offset)
		a += 0xFF0000;
	CD.acd_port[p].base = a & 0xFFFFFF;
}

static uint8_t acd_read_port(uint8_t p, uint8_t reg)
{
	switch (reg) {
	case 0x00:
	case 0x01: {
		uint32_t a = acd_get_address(p);
		acd_auto_inc(p);
		return CD.acd_ram[a];
	}
	case 0x02: return CD.acd_port[p].base & 0xFF;
	case 0x03: return (CD.acd_port[p].base >> 8) & 0xFF;
	case 0x04: return (CD.acd_port[p].base >> 16) & 0xFF;
	case 0x05: return CD.acd_port[p].offset & 0xFF;
	case 0x06: return (CD.acd_port[p].offset >> 8) & 0xFF;
	case 0x07: return CD.acd_port[p].increment & 0xFF;
	case 0x08: return (CD.acd_port[p].increment >> 8) & 0xFF;
	case 0x09: return CD.acd_port[p].control;
	case 0x0A: return 0;
	}
	return 0xFF;
}

static void acd_write_port(uint8_t p, uint8_t reg, uint8_t val)
{
	switch (reg) {
	case 0x00:
	case 0x01: {
		uint32_t a = acd_get_address(p);
		acd_auto_inc(p);
		CD.acd_ram[a] = val;
		break;
	}
	case 0x02:
		CD.acd_port[p].base = (CD.acd_port[p].base & 0xFFFF00)
		                    | val;
		break;
	case 0x03:
		CD.acd_port[p].base = (CD.acd_port[p].base & 0xFF00FF)
		                    | ((uint32_t)val << 8);
		break;
	case 0x04:
		CD.acd_port[p].base = (CD.acd_port[p].base & 0x00FFFF)
		                    | ((uint32_t)val << 16);
		break;
	case 0x05:
		CD.acd_port[p].offset = (CD.acd_port[p].offset & 0xFF00) | val;
		if (CD.acd_port[p].off_trigger == 1)
			acd_add_offset_to_base(p);
		break;
	case 0x06:
		CD.acd_port[p].offset = (CD.acd_port[p].offset & 0x00FF)
		                      | ((uint16_t)val << 8);
		if (CD.acd_port[p].off_trigger == 2)
			acd_add_offset_to_base(p);
		break;
	case 0x07:
		CD.acd_port[p].increment = (CD.acd_port[p].increment & 0xFF00)
		                         | val;
		break;
	case 0x08:
		CD.acd_port[p].increment = (CD.acd_port[p].increment & 0x00FF)
		                         | ((uint16_t)val << 8);
		break;
	case 0x09:
		CD.acd_port[p].control       = val & 0x7F;
		CD.acd_port[p].auto_inc      = (val & 0x01) != 0;
		CD.acd_port[p].add_offset    = (val & 0x02) != 0;
		CD.acd_port[p].sign_offset   = (val & 0x08) != 0;
		CD.acd_port[p].inc_to_base   = (val & 0x10) != 0;
		CD.acd_port[p].off_trigger   = (val >> 5) & 0x03;
		break;
	case 0x0A:
		if (CD.acd_port[p].off_trigger == 3)
			acd_add_offset_to_base(p);
		break;
	}
}

// ---------------------------------------------------------------------------
// Arcade Card bank $40-$43 memory-mapped access (called from pce_readIO/writeIO)

uint8_t cd_acd_read_bank(uint8_t port)
{
	uint32_t a = acd_get_address(port);
	acd_auto_inc(port);
	return CD.acd_ram[a];
}

void cd_acd_write_bank(uint8_t port, uint8_t val)
{
	uint32_t a = acd_get_address(port);
	acd_auto_inc(port);
	CD.acd_ram[a] = val;
}

// Register read/write
// ---------------------------------------------------------------------------

// Forward declarations for DMA
static void cdc_update_state(void);

static void adpcm_set_end_reached(bool val)
{
	if (val) {
		CD.adpcm_status |= 0x01;
		cdc_active_irqs |= 0x08;
	} else {
		CD.adpcm_status &= ~0x01;
		cdc_active_irqs &= ~0x08;
	}
	cdc_update_irqs();
}

static void adpcm_set_half_reached(bool val)
{
	// Stubbed: without ADPCM playback the length counter never counts
	// down, so the half-reached flag would stay set forever after a
	// length latch — blocking BIOS code that polls $1803 for zero.
	(void)val;
}

static bool adpcm_length_latch_enabled(void)
{
	return (CD.adpcm_ctrl & 0x10) != 0;
}

static void adpcm_process_write(uint8_t data)
{
	CD.adpcm_ram[CD.adpcm_write_addr++] = data;

	if (!adpcm_length_latch_enabled()) {
		CD.adpcm_length = (CD.adpcm_length + 1) & 0x1FFFF;
		if (CD.adpcm_length == 0)
			adpcm_set_end_reached(true);
		adpcm_set_half_reached(CD.adpcm_length < 0x8000);
	}
}

static void adpcm_run_dma(void)
{
	if (!(CD.adpcm_dma_ctrl & 0x03)) return;
	if (CD.phase != CDC_PHASE_DATA_IN) return;
	if (!scsi_req) return;
	if (!CD.adpcm_ram) return;

	while (scsi_req && CD.phase == CDC_PHASE_DATA_IN) {
		CD.adpcm_ram[CD.adpcm_write_addr++] = scsi_read_port;
		if (!adpcm_length_latch_enabled()) {
			CD.adpcm_length = (CD.adpcm_length + 1) & 0x1FFFF;
			if (CD.adpcm_length == 0)
				adpcm_set_end_reached(true);
			adpcm_set_half_reached(CD.adpcm_length < 0x8000);
		}
		scsi_ack = true;
		cdc_update_state();
		scsi_ack = false;
		cdc_update_state();
	}
	CD.adpcm_dma_ctrl &= ~0x01;
}

static void cdc_update_irqs(void)
{
	uint8_t prev = cdc_active_irqs;

	if (scsi_req && CD.phase != CDC_PHASE_COMMAND &&
	    CD.phase != CDC_PHASE_IDLE) {
		switch (CD.phase) {
		case CDC_PHASE_STATUS:
		case CDC_PHASE_MESSAGE:
			cdc_active_irqs |=  0x20;
			cdc_active_irqs &= ~0x40;
			break;
		case CDC_PHASE_DATA_IN:
			cdc_active_irqs |=  0x40;
			cdc_active_irqs &= ~0x20;
			break;
		default:
			cdc_active_irqs &= ~(0x20 | 0x40);
			break;
		}
	} else {
		cdc_active_irqs &= ~(0x20 | 0x40);
	}

	if ((CD.irq_mask & cdc_active_irqs) != 0)
		CPU.irq_lines |= INT_IRQ2;
	else
		CPU.irq_lines &= ~INT_IRQ2;
}

static void cdc_enter_phase(cdc_phase_t phase)
{
	CD.phase = phase;
	scsi_req = false;
	scsi_msg_done = false;

	switch (phase) {
	case CDC_PHASE_IDLE:
		break;
	case CDC_PHASE_COMMAND:
		CD.scsi_cmd_idx = 0;
		CD.read_remaining = 0;
		scsi_req = true;
		break;
	case CDC_PHASE_DATA_IN:
		if (CD.data_index < CD.data_length) {
			scsi_read_port = CD.sector_buf[CD.data_index++];
			scsi_req = true;
		}
		break;
	// Note: adpcm_run_dma() is called after cdc_update_irqs() below
	case CDC_PHASE_STATUS:
		scsi_read_port = CD.sense_key ? 0x02 : 0x00;
		scsi_req = true;
		break;
	case CDC_PHASE_MESSAGE:
		scsi_read_port = 0x00;
		scsi_req = true;
		break;
	default:
		break;
	}
	cdc_update_irqs();
	if (phase == CDC_PHASE_DATA_IN)
		adpcm_run_dma();
}

static void cdc_set_good_status(void)
{
	CD.sense_key = CD.sense_asc = CD.sense_ascq = 0;
	CD.data_index = CD.data_length = 0;
	CD.read_remaining = 0;
	cdc_enter_phase(CDC_PHASE_STATUS);
}

static void cdc_process_command_phase(void)
{
	if (scsi_req && scsi_ack) {
		scsi_req = false;
		if (CD.scsi_cmd_idx < sizeof(CD.scsi_command))
			CD.scsi_command[CD.scsi_cmd_idx++] = scsi_data_port;
		cdc_update_irqs();
	} else if (!scsi_req && !scsi_ack && CD.scsi_cmd_idx > 0) {
		uint8_t opcode = CD.scsi_command[0];
		int cmd_size = scsi_cmd_length(opcode);
		if (cmd_size == 0) {
			cdc_set_good_status();
		} else if (CD.scsi_cmd_idx >= (uint8_t)cmd_size) {
			CD.scsi_cmd_len = CD.scsi_cmd_idx;
			cdc_process_command();
			cdc_update_irqs();
		} else {
			scsi_req = true;
			cdc_update_irqs();
		}
	}
}

static void cdc_process_datain_phase(void)
{
	if (scsi_req && scsi_ack) {
		scsi_req = false;
		cdc_update_irqs();
	} else if (!scsi_req && !scsi_ack) {
		if (CD.data_index < CD.data_length) {
			scsi_read_port = CD.sector_buf[CD.data_index++];
			scsi_req = true;
			cdc_update_irqs();
		} else if (CD.read_remaining > 0) {
			if (read_sector(CD.read_lba) == 0) {
				CD.read_lba++;
				CD.read_remaining--;
				CD.data_index = 0;
				CD.data_length = CD_SECTOR_SIZE;
				scsi_read_port = CD.sector_buf[CD.data_index++];
				scsi_req = true;
				cdc_update_irqs();
			} else {
				cdc_set_error(0x03, 0x11, 0x05);
				cdc_update_irqs();
			}
		} else {
			cdc_set_good_status();
		}
	}
}

static void cdc_process_status_phase(void)
{
	if (scsi_req && scsi_ack) {
		scsi_req = false;
		cdc_update_irqs();
	} else if (!scsi_req && !scsi_ack) {
		cdc_enter_phase(CDC_PHASE_MESSAGE);
	}
}

static void cdc_process_message_phase(void)
{
	if (scsi_req && scsi_ack) {
		scsi_req = false;
		scsi_msg_done = true;
		cdc_update_irqs();
	} else if (!scsi_req && !scsi_ack && scsi_msg_done) {
		scsi_msg_done = false;
		cdc_enter_phase(CDC_PHASE_IDLE);
	}
}

static void cdc_update_state(void)
{
	switch (CD.phase) {
	case CDC_PHASE_COMMAND:  cdc_process_command_phase(); break;
	case CDC_PHASE_DATA_IN:  cdc_process_datain_phase();  break;
	case CDC_PHASE_STATUS:   cdc_process_status_phase();  break;
	case CDC_PHASE_MESSAGE:  cdc_process_message_phase(); break;
	default: break;
	}
}

uint8_t
cd_read(uint16_t addr)
{
	switch (addr) {

	// --- $1800: SCSI bus status (signal-based) ---
	case 0x1800: {
		uint8_t s = 0;
		switch (CD.phase) {
		case CDC_PHASE_IDLE:     break;
		case CDC_PHASE_COMMAND:  s = 0x90; break;  // BSY + CD
		case CDC_PHASE_DATA_IN:  s = 0x88; break;  // BSY + IO
		case CDC_PHASE_STATUS:   s = 0x98; break;  // BSY + CD + IO
		case CDC_PHASE_MESSAGE:  s = 0xB8; break;  // BSY + MSG + CD + IO
		case CDC_PHASE_BUSY:     s = 0x80; break;  // BSY only (audio playing)
		default: break;
		}
		if (scsi_req) s |= 0x40;
		return s;
	}

	// --- $1801: SCSI data port read (no auto-ACK) ---
	case 0x1801:
		switch (CD.phase) {
		case CDC_PHASE_STATUS:
		case CDC_PHASE_DATA_IN:
		case CDC_PHASE_MESSAGE:
			return scsi_read_port;
		default:
			return scsi_data_port;
		}

	// --- $1802: IRQ enable mask + ACK bit ---
	case 0x1802:
		return CD.irq_mask | (scsi_ack ? 0x80 : 0x00);

	// --- $1803: active IRQs + BRAM lock ---
	case 0x1803: {
		uint8_t v = cdc_active_irqs;
		// Clear event-based IRQ bits on read. Bits 0x20/0x40 are level-
		// sensitive (SCSI phase); 0x04/0x08/0x10 are events (ADPCM half,
		// ADPCM end, SubCode). Without ADPCM playback the half/end flags
		// would stay set forever after a length latch.
		cdc_active_irqs &= ~(0x04 | 0x08 | 0x10);
		cdc_update_irqs();
		CD.bram_locked = true;
		return v;
	}

	case 0x1804: return 0x00;
	case 0x1805: return 0x00;
	case 0x1806: return 0x00;

	// --- $1808: SCSI data port read WITH auto-ACK (DATA_IN) ---
	case 0x1808: {
		uint8_t v;
		switch (CD.phase) {
		case CDC_PHASE_STATUS:
		case CDC_PHASE_DATA_IN:
		case CDC_PHASE_MESSAGE:
			v = scsi_read_port;
			break;
		default:
			v = scsi_data_port;
			break;
		}
		if (scsi_req && CD.phase == CDC_PHASE_DATA_IN) {
			scsi_ack = true;
			cdc_update_state();
			scsi_ack = false;
			cdc_update_state();
		}
		return v;
	}

	// --- ADPCM registers ---
	case 0x180A: {
		uint8_t v = CD.adpcm_read_buf;
		if (CD.adpcm_ram) {
			CD.adpcm_read_buf = CD.adpcm_ram[CD.adpcm_read_addr];
			CD.adpcm_read_addr++;
			if (!adpcm_length_latch_enabled()) {
				if (CD.adpcm_length > 0) {
					CD.adpcm_length--;
					adpcm_set_half_reached(CD.adpcm_length < 0x8000);
				} else {
					adpcm_set_end_reached(true);
					adpcm_set_half_reached(false);
				}
			}
		}
		return v;
	}
	case 0x180B: return CD.adpcm_dma_ctrl;
	case 0x180C:
		return CD.adpcm_status
		     | (CD.adpcm_playing ? 0x08 : 0);
	case 0x180D: return 0x00;
	case 0x180E: return CD.adpcm_rate;

	// --- Super System Card identification ---
	case 0x18C0: return 0x00;
	case 0x18C1: return 0xAA;
	case 0x18C2: return 0x55;
	case 0x18C3: return 0x03;

	// --- Arcade Card registers ($1A00-$1AFF) ---
	default:
		if (addr >= 0x1A00 && addr <= 0x1AFF && CD.acd_ram) {
			if (addr <= 0x1A7F) {
				uint8_t port = (addr >> 4) & 0x03;
				uint8_t reg  = addr & 0x0F;
				return acd_read_port(port, reg);
			}
			switch (addr) {
			case 0x1AE0: return CD.acd_value & 0xFF;
			case 0x1AE1: return (CD.acd_value >> 8) & 0xFF;
			case 0x1AE2: return (CD.acd_value >> 16) & 0xFF;
			case 0x1AE3: return (CD.acd_value >> 24) & 0xFF;
			case 0x1AE4: return CD.acd_shift;
			case 0x1AE5: return CD.acd_rotate;
			case 0x1AFE: return 0x10;
			case 0x1AFF: return 0x51;
			}
		}
		break;
	}

	return 0xFF;
}

void
cd_write(uint16_t addr, uint8_t val)
{
	switch (addr) {

	// --- $1800: SCSI bus control — SEL pulse → enter COMMAND ---
	case 0x1800:
		if (CD.phase != CDC_PHASE_DATA_IN) {
			cdc_enter_phase(CDC_PHASE_COMMAND);
		} else {
			cdc_set_good_status();
		}
		break;

	// --- $1801: SCSI data port write ---
	case 0x1801:
		scsi_data_port = val;
		break;

	// --- $1802: ACK signal (bit 7) + IRQ enable mask (bits 0-6) ---
	case 0x1802: {
		bool new_ack = (val & 0x80) != 0;
		if (new_ack != scsi_ack) {
			scsi_ack = new_ack;
			cdc_update_state();
		}
		CD.irq_mask = val & 0x7F;
		cdc_update_irqs();
		break;
	}

	// --- $1804: CDC reset ---
	case 0x1804:
		cdc_reset_reg = val & 0x0F;
		if (val & 0x02) {
			CD.phase = CDC_PHASE_IDLE;
			CD.scsi_cmd_idx = 0;
			CD.data_index = CD.data_length = 0;
			CD.read_remaining = 0;
			scsi_req = false;
			scsi_ack = false;
			scsi_msg_done = false;
			cdc_active_irqs = 0;
			CD.irq_mask &= 0x8F;
			CD.audio_status = 3; // Stopped
			cd_audio_flush_ring();
			cdc_update_irqs();
		}
		break;

	// --- $1807: BRAM unlock ---
	case 0x1807:
		if (val & 0x80) CD.bram_locked = false;
		break;

	// --- $1808/$1809: ADPCM address port ---
	case 0x1808:
		CD.adpcm_addr_port = (CD.adpcm_addr_port & 0xFF00) | val;
		break;
	case 0x1809:
		CD.adpcm_addr_port = (CD.adpcm_addr_port & 0x00FF)
		                   | ((uint16_t)val << 8);
		break;

	// --- ADPCM data write ---
	case 0x180A:
		if (CD.adpcm_ram)
			adpcm_process_write(val);
		break;

	// --- ADPCM control ---
	case 0x180B:
		CD.adpcm_dma_ctrl = val;
		adpcm_run_dma();
		break;
	case 0x180D: {
		uint8_t prev = CD.adpcm_ctrl;
		if ((val & 0x02) && !(prev & 0x02))
			CD.adpcm_write_addr = CD.adpcm_addr_port
			                    - ((val & 0x01) ? 0 : 1);
		if ((val & 0x08) && !(prev & 0x08))
			CD.adpcm_read_addr = CD.adpcm_addr_port
			                   - ((val & 0x04) ? 0 : 1);
		CD.adpcm_ctrl = val;
		if (val & 0x10) {
			CD.adpcm_length = CD.adpcm_addr_port;
			adpcm_set_end_reached(false);
			adpcm_set_half_reached(CD.adpcm_length < 0x8000);
		}
		// Bit 5 rising: start ADPCM playback. Snapshot the read pointer
		// and length into the decoder and reset its state.
		if ((val & 0x20) && !(prev & 0x20)) {
			CD.adpcm_play_addr = CD.adpcm_read_addr;
			CD.adpcm_play_len  = CD.adpcm_length;
			CD.adpcm_cur_output = 2048;
			CD.adpcm_magnitude = 0;
			CD.adpcm_nibble = 0;
			CD.adpcm_resample_acc = 0;
			CD.adpcm_playing = (CD.adpcm_play_len > 0) ? 1 : 0;
			adpcm_set_end_reached(false);
		}
		if (val & 0x80) {
			CD.adpcm_read_addr = 0;
			CD.adpcm_write_addr = 0;
			CD.adpcm_addr_port = 0;
			CD.adpcm_length = 0;
			CD.adpcm_status = 0;
			CD.adpcm_read_buf = 0;
			CD.adpcm_playing = 0;
			adpcm_set_end_reached(false);
			adpcm_set_half_reached(false);
		}
		break;
	}
	case 0x180E: CD.adpcm_rate = val; break;
	case 0x180F: CD.adpcm_fade = val; break;

	// --- Arcade Card registers ($1A00-$1AFF) ---
	default:
		if (addr >= 0x1A00 && addr <= 0x1AFF && CD.acd_ram) {
			if (addr <= 0x1A7F) {
				uint8_t port = (addr >> 4) & 0x03;
				uint8_t reg  = addr & 0x0F;
				acd_write_port(port, reg, val);
			} else {
				switch (addr) {
				case 0x1AE0:
					CD.acd_value = (CD.acd_value & 0xFFFFFF00u)
					             | val;
					break;
				case 0x1AE1:
					CD.acd_value = (CD.acd_value & 0xFFFF00FFu)
					             | ((uint32_t)val << 8);
					break;
				case 0x1AE2:
					CD.acd_value = (CD.acd_value & 0xFF00FFFFu)
					             | ((uint32_t)val << 16);
					break;
				case 0x1AE3:
					CD.acd_value = (CD.acd_value & 0x00FFFFFFu)
					             | ((uint32_t)val << 24);
					break;
				case 0x1AE4:
					CD.acd_shift = val;
					if (val) {
						if (val & 0x08)
							CD.acd_value >>= (~val & 0x07) + 1;
						else
							CD.acd_value <<= (val & 0x07);
					}
					break;
				case 0x1AE5:
					CD.acd_rotate = val;
					if (val) {
						if (val & 0x08) {
							uint8_t r = (~val & 0x07) + 1;
							CD.acd_value = (CD.acd_value >> r)
							             | (CD.acd_value << (32 - r));
						} else {
							uint8_t r = val & 0x07;
							CD.acd_value = (CD.acd_value << r)
							             | (CD.acd_value >> (32 - r));
						}
					}
					break;
				}
			}
		}
		break;
	}
}

void
cd_setup_memory_map(void)
{
	// CD-ROM RAM: 64KB at hardware pages 0x80-0x87.
	if (CD.cd_ram) {
		for (int i = 0; i < 8; i++) {
			PCE.MemoryMapR[0x80 + i] = CD.cd_ram + i * 0x2000;
			PCE.MemoryMapW[0x80 + i] = CD.cd_ram + i * 0x2000;
		}
	}

	// Super System Card RAM: 192KB at hardware pages 0x68-0x7F.
	if (CD.scd_ram) {
		for (int i = 0; i < 24; i++) {
			PCE.MemoryMapR[0x68 + i] = CD.scd_ram + i * 0x2000;
			PCE.MemoryMapW[0x68 + i] = CD.scd_ram + i * 0x2000;
		}
	}

	// BRAM: 2KB at the bottom of hardware page 0xF7. The full 8KB page
	// buffer is mapped (upper 6KB is open-bus / 0xFF).
	if (CD.bram) {
		PCE.MemoryMapR[0xF7] = CD.bram;
		PCE.MemoryMapW[0xF7] = CD.bram;
	}

	// Arcade Card banks $40-$43: map to IOAREA so reads/writes are
	// intercepted by pce_readIO/pce_writeIO and routed to cd_acd_read_bank.
	if (CD.acd_ram) {
		for (int i = 0x40; i <= 0x43; i++) {
			PCE.MemoryMapR[i] = PCE.IOAREA;
			PCE.MemoryMapW[i] = PCE.IOAREA;
		}
	}
}

// ---------------------------------------------------------------------------
// ADPCM playback (Oki/Dialogic 4-bit ADPCM decoder, ported from Mesen2)
// ---------------------------------------------------------------------------

// Step-size table: 49 magnitude rows x 8 codes. Indexed (magnitude<<3)|code.
static const int16_t adpcm_step_size[392] = {
	0x0002,0x0006,0x000A,0x000E,0x0012,0x0016,0x001A,0x001E,
	0x0002,0x0006,0x000A,0x000E,0x0013,0x0017,0x001B,0x001F,
	0x0002,0x0006,0x000B,0x000F,0x0015,0x0019,0x001E,0x0022,
	0x0002,0x0007,0x000C,0x0011,0x0017,0x001C,0x0021,0x0026,
	0x0002,0x0007,0x000D,0x0012,0x0019,0x001E,0x0024,0x0029,
	0x0003,0x0009,0x000F,0x0015,0x001C,0x0022,0x0028,0x002E,
	0x0003,0x000A,0x0011,0x0018,0x001F,0x0026,0x002D,0x0034,
	0x0003,0x000A,0x0012,0x0019,0x0022,0x0029,0x0031,0x0038,
	0x0004,0x000C,0x0015,0x001D,0x0026,0x002E,0x0037,0x003F,
	0x0004,0x000D,0x0016,0x001F,0x0029,0x0032,0x003B,0x0044,
	0x0005,0x000F,0x0019,0x0023,0x002E,0x0038,0x0042,0x004C,
	0x0005,0x0010,0x001B,0x0026,0x0032,0x003D,0x0048,0x0053,
	0x0006,0x0012,0x001F,0x002B,0x0038,0x0044,0x0051,0x005D,
	0x0006,0x0013,0x0021,0x002E,0x003D,0x004A,0x0058,0x0065,
	0x0007,0x0016,0x0025,0x0034,0x0043,0x0052,0x0061,0x0070,
	0x0008,0x0018,0x0029,0x0039,0x004A,0x005A,0x006B,0x007B,
	0x0009,0x001B,0x002D,0x003F,0x0052,0x0064,0x0076,0x0088,
	0x000A,0x001E,0x0032,0x0046,0x005A,0x006E,0x0082,0x0096,
	0x000B,0x0021,0x0037,0x004D,0x0063,0x0079,0x008F,0x00A5,
	0x000C,0x0024,0x003C,0x0054,0x006D,0x0085,0x009D,0x00B5,
	0x000D,0x0027,0x0042,0x005C,0x0078,0x0092,0x00AD,0x00C7,
	0x000E,0x002B,0x0049,0x0066,0x0084,0x00A1,0x00BF,0x00DC,
	0x0010,0x0030,0x0051,0x0071,0x0092,0x00B2,0x00D3,0x00F3,
	0x0011,0x0034,0x0058,0x007B,0x00A0,0x00C3,0x00E7,0x010A,
	0x0013,0x003A,0x0061,0x0088,0x00B0,0x00D7,0x00FE,0x0125,
	0x0015,0x0040,0x006B,0x0096,0x00C2,0x00ED,0x0118,0x0143,
	0x0017,0x0046,0x0076,0x00A5,0x00D5,0x0104,0x0134,0x0163,
	0x001A,0x004E,0x0082,0x00B6,0x00EB,0x011F,0x0153,0x0187,
	0x001C,0x0055,0x008F,0x00C8,0x0102,0x013B,0x0175,0x01AE,
	0x001F,0x005E,0x009D,0x00DC,0x011C,0x015B,0x019A,0x01D9,
	0x0022,0x0067,0x00AD,0x00F2,0x0139,0x017E,0x01C4,0x0209,
	0x0026,0x0072,0x00BF,0x010B,0x0159,0x01A5,0x01F2,0x023E,
	0x002A,0x007E,0x00D2,0x0126,0x017B,0x01CF,0x0223,0x0277,
	0x002E,0x008A,0x00E7,0x0143,0x01A1,0x01FD,0x025A,0x02B6,
	0x0033,0x0099,0x00FF,0x0165,0x01CB,0x0231,0x0297,0x02FD,
	0x0038,0x00A8,0x0118,0x0188,0x01F9,0x0269,0x02D9,0x0349,
	0x003D,0x00B8,0x0134,0x01AF,0x022B,0x02A6,0x0322,0x039D,
	0x0044,0x00CC,0x0154,0x01DC,0x0264,0x02EC,0x0374,0x03FC,
	0x004A,0x00DF,0x0175,0x020A,0x02A0,0x0335,0x03CB,0x0460,
	0x0052,0x00F6,0x019B,0x023F,0x02E4,0x0388,0x042D,0x04D1,
	0x005A,0x010F,0x01C4,0x0279,0x032E,0x03E3,0x0498,0x054D,
	0x0063,0x012A,0x01F1,0x02B8,0x037F,0x0446,0x050D,0x05D4,
	0x006D,0x0148,0x0223,0x02FE,0x03D9,0x04B4,0x058F,0x066A,
	0x0078,0x0168,0x0259,0x0349,0x043B,0x052B,0x061C,0x070C,
	0x0084,0x018D,0x0296,0x039F,0x04A8,0x05B1,0x06BA,0x07C3,
	0x0091,0x01B4,0x02D8,0x03FB,0x051F,0x0642,0x0766,0x0889,
	0x00A0,0x01E0,0x0321,0x0461,0x05A2,0x06E2,0x0823,0x0963,
	0x00B0,0x0210,0x0371,0x04D1,0x0633,0x0793,0x08F4,0x0A54,
	0x00C2,0x0246,0x03CA,0x054E,0x06D2,0x0856,0x09DA,0x0B5E,
};

static const int adpcm_step_factor[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// Decode and return one ADPCM sample (mono, ~12-bit centered, scaled).
static int16_t adpcm_decode_one(void)
{
	if (CD.adpcm_play_len == 0)
		return (int16_t)(((int)CD.adpcm_cur_output - 2048) * 10);

	uint8_t byte = CD.adpcm_ram[CD.adpcm_play_addr];
	uint8_t data;
	if (CD.adpcm_nibble) {
		CD.adpcm_nibble = 0;
		data = byte & 0x0F;
		CD.adpcm_play_addr++;
		CD.adpcm_play_len--;
	} else {
		CD.adpcm_nibble = 1;
		data = (byte >> 4) & 0x0F;
	}

	uint8_t code = data & 0x07;
	int sign = (data & 0x08) ? -1 : 1;
	int adj = (int)adpcm_step_size[(CD.adpcm_magnitude << 3) | code] * sign;
	CD.adpcm_cur_output = (uint16_t)((CD.adpcm_cur_output + adj) & 0xFFF);
	int mag = CD.adpcm_magnitude + adpcm_step_factor[code];
	if (mag < 0) mag = 0; else if (mag > 48) mag = 48;
	CD.adpcm_magnitude = (uint8_t)mag;

	if (CD.adpcm_play_len == 0) {
		adpcm_set_end_reached(true);
		CD.adpcm_playing = 0;
	}
	return (int16_t)(((int)CD.adpcm_cur_output - 2048) * 10);
}

bool
cd_adpcm_generate_samples(int16_t *out, int num_samples, int host_rate)
{
	if (!CD.adpcm_playing || !CD.adpcm_ram || host_rate <= 0)
		return false;

	int denom = 16 - (CD.adpcm_rate & 0x0F);
	if (denom <= 0) denom = 1;
	uint32_t freq = 32000u / (uint32_t)denom;   // ADPCM native sample rate

	int16_t cur = (int16_t)(((int)CD.adpcm_cur_output - 2048) * 10);
	for (int i = 0; i < num_samples; i++) {
		CD.adpcm_resample_acc += freq;
		while (CD.adpcm_resample_acc >= (uint32_t)host_rate) {
			CD.adpcm_resample_acc -= (uint32_t)host_rate;
			cur = adpcm_decode_one();
		}
		int32_t l = (int32_t)out[i * 2]     + cur;
		int32_t r = (int32_t)out[i * 2 + 1] + cur;
		out[i * 2]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
	return true;
}

// ---------------------------------------------------------------------------
// CD Audio playback
// ---------------------------------------------------------------------------

static int open_bin_for_audio(int idx)
{
	cd_track_t *t = &CD.tracks[idx];
	if (audio_fil && strcmp(audio_bin_name, t->bin_name) == 0)
		return 0;
	if (audio_fil)
		f_close(audio_fil);
	else {
		audio_fil = frens_f_malloc(sizeof(FIL));
		if (!audio_fil) return -1;
	}
	static char path[256];
	snprintf(path, sizeof(path), "%s%s", CD.cue_dir, t->bin_name);
	if (f_open(audio_fil, path, FA_READ) != FR_OK) {
		audio_bin_name[0] = '\0';
		return -1;
	}
	strncpy(audio_bin_name, t->bin_name, CD_BIN_NAME_MAX - 1);
	audio_bin_name[CD_BIN_NAME_MAX - 1] = '\0';
	return 0;
}

static int find_track_by_number(int track_no)
{
	for (int i = 0; i < CD.num_tracks; i++) {
		if (CD.tracks[i].track_no == track_no)
			return i;
	}
	return -1;
}

static uint32_t parse_audio_lba(const uint8_t *cmd)
{
	switch (cmd[9] & 0xC0) {
	case 0x00:
		return ((uint32_t)cmd[3] << 16) | ((uint32_t)cmd[4] << 8) | cmd[5];
	case 0x40: {
		uint32_t lba = (uint32_t)bcd2bin(cmd[2]) * 4500
		             + (uint32_t)bcd2bin(cmd[3]) * 75
		             + (uint32_t)bcd2bin(cmd[4]);
		return (lba >= 150) ? lba - 150 : 0;
	}
	case 0x80: {
		int trk = bcd2bin(cmd[2]);
		int idx = find_track_by_number(trk);
		return (idx >= 0) ? CD.tracks[idx].lba_start : 0;
	}
	}
	return 0;
}

static uint32_t audio_next_file_off = 0xFFFFFFFF;

static void cd_audio_flush_ring(void)
{
	CD.audio_ring_write = 0;
	CD.audio_ring_read = 0;
	CD.audio_ring_count = 0;
	audio_next_file_off = 0xFFFFFFFF;
}

void
cd_audio_update(void)
{
	if (CD.audio_status != 0)  // 0 = Playing
		return;
	if (!CD.audio_ring_buf)
		return;

	int reads = 0;
	while (__atomic_load_n(&CD.audio_ring_count, __ATOMIC_ACQUIRE) < 4
	       && reads < 2) {
		uint32_t next_lba = CD.audio_cur_lba
		                  + __atomic_load_n(&CD.audio_ring_count, __ATOMIC_ACQUIRE);
		if (next_lba >= CD.audio_end_lba)
			break;

		int idx = find_track(next_lba);
		if (idx < 0 || CD.tracks[idx].type != 0)
			break;

		cd_track_t *t = &CD.tracks[idx];
		uint8_t *dst = &CD.audio_ring_buf[CD.audio_ring_write * CD_RAW_SECTOR_SIZE];
		int ok = 0;

		if (CD.is_chd) {
			// CHD-backed: pull a raw audio sector through the hunk cache.
			// cd_chd_read_raw_sector already handles the chdman big-endian
			// -> host little-endian byteswap for audio tracks.
			//
			// Use non-blocking mutex acquisition: cd_chd_read_raw_sector
			// can call libchdr's chd_read which takes ~30 ms per hunk on a
			// cache miss. If core0 already holds sd_mutex for a SCSI data
			// read (also ~30 ms for CHD data tracks), blocking here can
			// stall core1's main loop long enough that the HSTX DMA chain
			// underflows and the watchdog triggers a resync. The audio
			// ring has up to 4 sectors of slack (~52 ms), so skipping a
			// fill cycle is preferable to blocking.
			if (!mutex_try_enter(&sd_mutex, NULL))
				break;
			ok = (cd_chd_read_raw_sector(next_lba, dst) == 0);
			mutex_exit(&sd_mutex);
			if (!ok) break;
			audio_next_file_off = 0xFFFFFFFF;  // CHD path has no linear file cursor
		} else {
			uint64_t off = t->bin_offset
			             + (uint64_t)(next_lba - t->lba_start) * CD_RAW_SECTOR_SIZE;

			mutex_enter_blocking(&sd_mutex);
			if (open_bin_for_audio(idx) != 0) {
				mutex_exit(&sd_mutex);
				break;
			}
			if (off != audio_next_file_off) {
				if (f_lseek(audio_fil, off) != FR_OK) {
					mutex_exit(&sd_mutex);
					break;
				}
			}
			UINT br;
			ok = (f_read(audio_fil, dst, CD_RAW_SECTOR_SIZE, &br) == FR_OK
			      && br == CD_RAW_SECTOR_SIZE);
			mutex_exit(&sd_mutex);

			if (!ok) break;
			audio_next_file_off = off + CD_RAW_SECTOR_SIZE;
		}

		CD.audio_ring_write = (CD.audio_ring_write + 1) & 3;
		__atomic_add_fetch(&CD.audio_ring_count, 1, __ATOMIC_RELEASE);
		reads++;
	}
}

int
cd_audio_generate_samples(int16_t *out, int num_samples)
{
	if (CD.audio_status != 0 || !CD.audio_ring_buf)
		return 0;

	int generated = 0;
	while (generated < num_samples) {
		if (__atomic_load_n(&CD.audio_ring_count, __ATOMIC_ACQUIRE) == 0) {
			memset(out + generated * 2, 0, (num_samples - generated) * 4);
			break;
		}

		const uint8_t *sector = &CD.audio_ring_buf[CD.audio_ring_read * CD_RAW_SECTOR_SIZE];
		int avail = 588 - CD.audio_cur_sample;
		int want  = num_samples - generated;
		int n     = (avail < want) ? avail : want;
		memcpy(out + generated * 2, sector + CD.audio_cur_sample * 4, n * 4);
		generated += n;
		CD.audio_cur_sample += n;

		if (CD.audio_cur_sample >= 588) {
			CD.audio_cur_sample = 0;
			CD.audio_ring_read = (CD.audio_ring_read + 1) & 3;
			__atomic_sub_fetch(&CD.audio_ring_count, 1, __ATOMIC_RELEASE);
			CD.audio_cur_lba++;

			cdc_active_irqs |= 0x10;
			cdc_update_irqs();

			if (CD.audio_cur_lba >= CD.audio_end_lba) {
				switch (CD.audio_end_mode) {
				case 0: // stop
				default:
					CD.audio_status = 3;
					break;
				case 1: // loop
					CD.audio_cur_lba = CD.audio_start_lba;
					cd_audio_flush_ring();
					break;
				case 2: // IRQ
					CD.audio_status = 3;
					cdc_set_good_status();
					break;
				case 3: // stop (status already returned)
					CD.audio_status = 3;
					break;
				}
				if (CD.audio_status != 0)
					break;
			}
		}
	}
	return generated;
}

// ---------------------------------------------------------------------------
// BRAM persistence
// ---------------------------------------------------------------------------

// Check for the "HUBM" signature; if missing, write the standard
// empty-but-formatted header so the BIOS and games see valid BRAM.
static void bram_ensure_formatted(void)
{
	if (!CD.bram) return;
	if (CD.bram[0] == 'H' && CD.bram[1] == 'U' &&
	    CD.bram[2] == 'B' && CD.bram[3] == 'M')
		return;
	memset(CD.bram, 0, 0x800);
	CD.bram[0] = 0x48; // 'H'
	CD.bram[1] = 0x55; // 'U'
	CD.bram[2] = 0x42; // 'B'
	CD.bram[3] = 0x4D; // 'M'
	CD.bram[4] = 0x00;
	CD.bram[5] = 0x88;
	CD.bram[6] = 0x10;
	CD.bram[7] = 0x80;
	printf("BRAM: formatted (empty header)\n");
}

int
cd_bram_save(const char *path)
{
	static FIL fil;
	if (!CD.bram || !path) return -1;
	if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return -1;
	UINT bw;
	FRESULT r = f_write(&fil, CD.bram, 0x800, &bw);
	f_close(&fil);
	if (r != FR_OK || bw != 0x800) return -1;
	printf("BRAM: saved to %s\n", path);
	return 0;
}

int
cd_bram_load(const char *path)
{
	static FIL fil;
	if (!CD.bram || !path) return -1;
	if (f_open(&fil, path, FA_READ) != FR_OK) {
		bram_ensure_formatted();
		return -1;
	}
	UINT br;
	FRESULT r = f_read(&fil, CD.bram, 0x800, &br);
	f_close(&fil);
	if (r != FR_OK || br != 0x800) {
		bram_ensure_formatted();
		return -1;
	}
	printf("BRAM: loaded from %s\n", path);
	bram_ensure_formatted();
	return 0;
}
