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
#include "ff.h"

#include "pce-go.h"
#include "pce.h"
#include "cd.h"

// Persistent BIN file handle, allocated from PSRAM while a disc is open.
static FIL *bin_fil = NULL;

// C-side allocator wrappers from pico_shared/FrensHelpers.cpp. They route to
// PSRAM via lwmem when available and fall back to plain malloc otherwise.
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

// Global CD state (zero-initialized).
cd_state_t CD;

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
	CD.audio_status = 0;
	CD.irq_status = 0;
	CD.sense_key = 0;
	CD.sense_asc = 0;
	CD.sense_ascq = 0;

	CD.adpcm_ctrl = 0;
	CD.adpcm_dma_ctrl = 0;
	CD.adpcm_status = 0;
	CD.adpcm_rate = 0;
	CD.adpcm_fade = 0;

	memset(CD.acd_port, 0, sizeof(CD.acd_port));
	CD.acd_shift = 0;
	CD.acd_rotate = 0;
}

void
cd_term(void)
{
	bool was_attached = CD.cd_attached;

	cd_close();

	if (CD.cd_ram)    { frens_f_free(CD.cd_ram);    CD.cd_ram = NULL; }
	if (CD.scd_ram)   { frens_f_free(CD.scd_ram);   CD.scd_ram = NULL; }
	if (CD.adpcm_ram) { frens_f_free(CD.adpcm_ram); CD.adpcm_ram = NULL; }
	if (CD.acd_ram)   { frens_f_free(CD.acd_ram);   CD.acd_ram = NULL; }
	if (CD.bram)      { frens_f_free(CD.bram);      CD.bram = NULL; }

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
	return strcasecmp(name + n - 4, ".pce") == 0;
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

int
cd_find_bios(char *out_path, size_t size, bios_variant_t *out_variant)
{
	if (!out_path || size < 32)
		return -1;

	// All FatFS-heavy locals are static: a DIR is ~570 B and a FILINFO is
	// ~278 B on this FatFS configuration (FF_MAX_LFN=255), and several
	// FF_MAX_LFN-sized path buffers compound the problem. With
	// PICO_STACK_SIZE=3072 the deep call chain from main would otherwise
	// overflow into core1's SCRATCH region and cause a delayed hardfault.
	// cd_find_bios is single-threaded and only runs at boot / on disc load.
	static DIR     dir;
	static FILINFO fno;
	static char    best_path[FF_MAX_LFN + 8];
	static char    fallback_path[FF_MAX_LFN + 8];
	static char    full[FF_MAX_LFN + 8];

	best_path[0] = fallback_path[0] = '\0';

	if (f_opendir(&dir, "/bios") != FR_OK) {
		printf("cd_find_bios: /bios directory not found\n");
		return -1;
	}

	int best_priority = -1;
	uint32_t best_crc = 0;
	const bios_entry_t *best_entry = NULL;
	uint32_t fallback_crc = 0;

	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
		if (fno.fattrib & (AM_DIR | AM_HID))
			continue;
		if (!has_pce_extension(fno.fname))
			continue;

		snprintf(full, sizeof(full), "/bios/%s", fno.fname);

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
		strncpy(out_path, best_path, size - 1);
		out_path[size - 1] = '\0';
		if (out_variant) *out_variant = best_entry->variant;
		CD.bios_variant = best_entry->variant;
		CD.bios_is_us = best_entry->is_us;
		printf("cd_find_bios: selected %s (CRC=0x%08lX, %s, region=%s)\n",
		       best_path, (unsigned long)best_crc, best_entry->name,
		       best_entry->is_us ? "US" : "JP");
		return 0;
	}

	if (fallback_path[0]) {
		strncpy(out_path, fallback_path, size - 1);
		out_path[size - 1] = '\0';
		if (out_variant) *out_variant = BIOS_UNKNOWN;
		CD.bios_variant = BIOS_UNKNOWN;
		CD.bios_is_us = false;
		printf("cd_find_bios: no known BIOS, falling back to %s "
		       "(CRC=0x%08lX, assuming System Card 3.0 / JP)\n",
		       fallback_path, (unsigned long)fallback_crc);
		return 0;
	}

	printf("cd_find_bios: no .pce files found in /bios/\n");
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
static int stat_bin_size(const char *bin_name, FSIZE_t *out_size)
{
	static FIL  s_fil;
	static char s_path[256];
	if (snprintf(s_path, sizeof(s_path), "%s%s", CD.cue_dir, bin_name)
	        >= (int)sizeof(s_path))
		return -1;
	if (f_open(&s_fil, s_path, FA_READ) != FR_OK)
		return -1;
	*out_size = f_size(&s_fil);
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
	//   - lba_start: running cumulative disc LBA
	//   - bin_offset: byte offset of track within its OWN BIN file
	//   - lba_end:
	//       * if next track shares this BIN: lba_end = lba_start + delta(file_lba)
	//       * otherwise (last track in its BIN): derive length from BIN size
	uint32_t running_lba = 0;
	for (int i = 0; i < CD.num_tracks; i++) {
		cd_track_t *t = &CD.tracks[i];
		uint32_t raw = t->sector_size ? CD_RAW_SECTOR_SIZE : CD_SECTOR_SIZE;

		t->lba_start  = running_lba;
		t->bin_offset = (uint64_t)t->file_lba * raw;

		uint32_t length_lbas = 0;
		const bool same_bin_next =
			(i + 1 < CD.num_tracks) &&
			(strcmp(t->bin_name, CD.tracks[i + 1].bin_name) == 0);

		if (same_bin_next) {
			uint32_t next_fl = CD.tracks[i + 1].file_lba;
			if (next_fl >= t->file_lba)
				length_lbas = next_fl - t->file_lba;
		} else {
			FSIZE_t bin_size = 0;
			if (stat_bin_size(t->bin_name, &bin_size) == 0 &&
			    bin_size > t->bin_offset) {
				length_lbas = (uint32_t)((bin_size - t->bin_offset) / raw);
			} else {
				printf("cd_load_cue: cannot stat BIN %s%s\n",
				       CD.cue_dir, t->bin_name);
			}
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
		printf("  T%02u %s lba=[%6lu..%6lu) flba=%6lu off=%9llu  %s\n",
		       t->track_no,
		       t->type ? "DATA " : "AUDIO",
		       (unsigned long)t->lba_start,
		       (unsigned long)t->lba_end,
		       (unsigned long)t->file_lba,
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
	memset(CD.tracks, 0, sizeof(CD.tracks));
	CD.num_tracks = 0;
	CD.first_track = 0;
	CD.last_track = 0;
	CD.total_lba = 0;
}

// ---------------------------------------------------------------------------
// Register read/write dispatch ($1800-$1AFF on hardware page $FF).
//
// Step 4 implements:
//   - Super System Card identification at $18C1/$18C2/$18C3/$18C5/$18C6
//     (the BIOS needs this to acknowledge the 192KB SCD RAM at pages $68-$7F)
//   - Arcade Card identification at $1AFE/$1AFF (games probe this; harmless
//     to report present even without full ACD register handling yet).
//
// Everything else still returns 0xFF — the SCSI state machine, ADPCM port
// registers, BRAM lock/unlock and the Arcade Card port windows are filled
// in by Steps 5-9.
// ---------------------------------------------------------------------------

uint8_t
cd_read(uint16_t addr)
{
	// Only the low 12 bits matter; readIO/writeIO already filtered the high
	// page (0xFF) and the $1Ann group is dispatched here too.
	switch (addr) {

	// --- Super System Card identification ($18C0-$18C6) ---
	// The SC3 BIOS reads these to decide whether to enable extended RAM
	// (pages $68-$7F). See pctech.txt §8.1: $18C5=$55 && $18C6=$AA selects
	// the v3 path; $18C1=$AA && $18C2=$55 enables the v2 path as a fallback.
	case 0x18C1: return 0xAA;
	case 0x18C2: return 0x55;
	case 0x18C3: return 0x00;   // version / status byte (BIOS doesn't use)
	case 0x18C5: return 0x55;
	case 0x18C6: return 0xAA;

	// --- Arcade Card identification ($1AFE/$1AFF) ---
	// $1AFE: bit 4 set => Arcade Card present. $1AFF: hardware version.
	// We report 0x10 / 0x01 unconditionally on CD games so ACD-aware games
	// can map their 2MB window. Full port-window handling is Step 9.
	case 0x1AFE: return 0x10;
	case 0x1AFF: return 0x01;
	}

	return 0xFF;
}

void
cd_write(uint16_t addr, uint8_t val)
{
	(void)addr;
	(void)val;
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

	// Arcade Card RAM is *not* memory-mapped; it is accessed only through
	// the $1A00-$1AFF I/O port window (Step 9).
}

int
cd_bram_save(const char *path)
{
	(void)path;
	return -1;
}

int
cd_bram_load(const char *path)
{
	(void)path;
	return -1;
}
