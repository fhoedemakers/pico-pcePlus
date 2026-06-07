// CHD-backed CD-ROM image support.
//
// Mounts a MAME .chd disc image, parses its per-track metadata into the
// existing CD.tracks[] array (so the rest of the SCSI command path stays
// untouched), and serves sector reads via a small LRU cache of decompressed
// hunks. Audio reads (cd_audio_update in cd.c) and data reads (read_sector)
// both route through cd_chd_read_raw_sector when CD.is_chd is set.
//
// Trade-offs:
//   * Hunk cache lives in PSRAM (allocated via frens_f_malloc). CD CHDs use
//     hunkbytes = frames_per_hunk * 2448. The Pico CHDs we've tested use
//     4 frames/hunk = 9792 B; the libchdr default is 8 frames = 19584 B.
//     We sized the cache for either: 4 entries x 19584 B max = ~77 KB worst
//     case, well under the PSRAM headroom.
//   * Audio byteswap: real CDs store CD-DA samples little-endian, but
//     chdman packs them big-endian (MAME convention). We swap on the way
//     out of the hunk cache for audio tracks only.
//   * CHD frame layout: each disc frame is stored as a 2352-byte sector
//     followed by 96 bytes of subcode. We extract the 2352 and discard
//     the subcode. For MODE1 data tracks the SCSI path expects 2048 user
//     bytes starting after the 16-byte sync header; we still return the
//     raw 2352 and let cd.c strip as it does for MODE1/2352 CUE+BIN.

#include <stdio.h>
#include <string.h>
#include "cd.h"
#include "cd_chd.h"

#if !ENABLE_CHD

// CHD compiled out — provide trivial stubs so cd.c's CD.is_chd dispatches
// resolve at link time. CD.is_chd stays false because cd_chd_open always
// fails, so the CUE+BIN path is always taken.

bool cd_chd_is_active(void)      { return false; }
int  cd_chd_open(const char *p)  { (void)p; return -1; }
void cd_chd_close(void)          {}
int  cd_chd_read_raw_sector(uint32_t lba, uint8_t *dest)
{
	(void)lba; (void)dest; return -1;
}

#else  // ENABLE_CHD

#include "libchdr/chd.h"
#include "libchdr/cdrom.h"
#include "libchdr/coretypes.h"
#include "ff.h"

// PSRAM-backed alloc/free C wrappers from pico_shared/FrensHelpers.cpp.
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

// --- FatFs <-> libchdr core_file_callbacks adapter ------------------------
// Mirrors the spike's adapter (cd_chd_diag.cpp). The void* argp passed to
// libchdr is a static FIL *. We keep one FIL handle for the lifetime of the
// mount in chd_fil so libchdr's f* calls don't churn opens.

static FIL  chd_fil;
static bool chd_fil_open = false;

static uint64_t cb_fsize(void *fp)  { return (uint64_t)f_size((FIL *)fp); }
static int      cb_fclose(void *fp) { (void)fp; return 0; /* we own the FIL */ }

static size_t cb_fread(void *buf, size_t sz, size_t cnt, void *fp)
{
	UINT br = 0;
	if (sz == 0 || cnt == 0) return 0;
	if (f_read((FIL *)fp, buf, (UINT)(sz * cnt), &br) != FR_OK) return 0;
	return br / sz;
}

static int cb_fseek(void *fp, int64_t off, int whence)
{
	FIL *f = (FIL *)fp;
	FSIZE_t target = 0;
	switch (whence) {
		case SEEK_SET: target = (FSIZE_t)off; break;
		case SEEK_CUR: target = f_tell(f) + (FSIZE_t)off; break;
		case SEEK_END: target = f_size(f) + (FSIZE_t)off; break;
		default: return -1;
	}
	return (f_lseek(f, target) == FR_OK) ? 0 : -1;
}

static const core_file_callbacks fatfs_cb = {
	cb_fsize, cb_fread, cb_fclose, cb_fseek,
};

// --- Hunk cache ----------------------------------------------------------
// Small LRU keyed by hunk index. Sized to hold one full per-frame-group in
// each entry; 4 entries cover ~16-32 sequential sectors which is enough for
// CD-DA streaming and bursty data reads without thrash.

#define CHD_CACHE_ENTRIES 4

typedef struct {
	uint32_t hunk_index;
	uint32_t age;            // higher = more recently used
	uint8_t *buf;            // hunkbytes long (allocated in PSRAM at mount)
	bool     valid;
} chd_cache_entry_t;

static chd_cache_entry_t cache[CHD_CACHE_ENTRIES];
static uint32_t          cache_age_tick = 1;
static uint32_t          chd_hunkbytes = 0;
static uint32_t          chd_frames_per_hunk = 0;

static void cache_init(uint32_t hunkbytes)
{
	chd_hunkbytes = hunkbytes;
	chd_frames_per_hunk = hunkbytes / CD_FRAME_SIZE;
	cache_age_tick = 1;
	for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
		cache[i].valid = false;
		cache[i].age   = 0;
		cache[i].hunk_index = 0xFFFFFFFFu;
		if (!cache[i].buf) {
			cache[i].buf = (uint8_t *)frens_f_malloc(hunkbytes);
		}
	}
}

static void cache_free(void)
{
	for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
		if (cache[i].buf) {
			frens_f_free(cache[i].buf);
			cache[i].buf = NULL;
		}
		cache[i].valid = false;
	}
}

// Look up or fetch a hunk. Returns the buffer pointer (hunkbytes long), or
// NULL on read/decompress failure.
static uint8_t *cache_get(uint32_t hunk_index)
{
	// Hit?
	for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
		if (cache[i].valid && cache[i].hunk_index == hunk_index) {
			cache[i].age = ++cache_age_tick;
			return cache[i].buf;
		}
	}

	// Miss — evict the LRU entry. Empty slot is treated as age 0, so it
	// wins automatically.
	int victim = 0;
	uint32_t victim_age = cache[0].valid ? cache[0].age : 0;
	for (int i = 1; i < CHD_CACHE_ENTRIES; i++) {
		uint32_t age = cache[i].valid ? cache[i].age : 0;
		if (age < victim_age) { victim = i; victim_age = age; }
	}

	chd_file *chd = (chd_file *)CD.chd_handle;
	if (!chd || !cache[victim].buf) return NULL;
	chd_error err = chd_read(chd, hunk_index, cache[victim].buf);
	if (err != CHDERR_NONE) {
		printf("cd_chd: chd_read hunk %lu failed: %s\n",
		       (unsigned long)hunk_index, chd_error_string(err));
		cache[victim].valid = false;
		return NULL;
	}
	cache[victim].valid = true;
	cache[victim].hunk_index = hunk_index;
	cache[victim].age = ++cache_age_tick;
	return cache[victim].buf;
}

// --- Track metadata parsing ----------------------------------------------
//
// Two formats in use:
//   CHTR (older, no pregap):      "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d"
//   CHT2 (current, with pregap):  "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d
//                                  PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
// Tracks are stored in the CHD's data area sequentially, padded to multiples
// of CD_TRACK_PADDING (4) frames per track.

static int parse_track_type(const char *s, uint8_t *out_type, uint8_t *out_sector_size)
{
	// type: 0 = audio, 1 = data. sector_size: 0 = 2048-byte semantics
	// (MODE1 data), 1 = 2352-byte semantics (MODE1_RAW, MODE2_RAW, AUDIO).
	if (strcmp(s, "AUDIO") == 0)       { *out_type = 0; *out_sector_size = 1; return 0; }
	if (strcmp(s, "MODE1") == 0)       { *out_type = 1; *out_sector_size = 0; return 0; }
	if (strcmp(s, "MODE1_RAW") == 0)   { *out_type = 1; *out_sector_size = 1; return 0; }
	if (strcmp(s, "MODE2") == 0)       { *out_type = 1; *out_sector_size = 1; return 0; }
	if (strcmp(s, "MODE2_FORM1") == 0) { *out_type = 1; *out_sector_size = 0; return 0; }
	if (strcmp(s, "MODE2_FORM2") == 0) { *out_type = 1; *out_sector_size = 1; return 0; }
	if (strcmp(s, "MODE2_RAW") == 0)   { *out_type = 1; *out_sector_size = 1; return 0; }
	if (strcmp(s, "MODE2_FORM_MIX") == 0) { *out_type = 1; *out_sector_size = 1; return 0; }
	return -1;
}

static int load_tracks_from_metadata(chd_file *chd)
{
	memset(CD.tracks, 0, sizeof(CD.tracks));
	CD.num_tracks  = 0;
	CD.first_track = 0;
	CD.last_track  = 0;
	CD.total_lba   = 0;

	uint32_t running_lba = 0;
	uint32_t chd_frame_cursor = 0;

	for (uint32_t i = 0; i < CD_MAX_TRACKS; i++) {
		char meta[256];
		uint32_t resultlen = 0;
		uint32_t resulttag = 0;
		uint8_t  resultflags = 0;

		// Try CHT2 first (richer), fall back to CHTR.
		chd_error err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG,
		                                 i, meta, sizeof(meta),
		                                 &resultlen, &resulttag, &resultflags);
		bool is_cht2 = (err == CHDERR_NONE);
		if (!is_cht2) {
			err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG,
			                       i, meta, sizeof(meta),
			                       &resultlen, &resulttag, &resultflags);
			if (err != CHDERR_NONE) break;  // no more tracks
		}
		meta[sizeof(meta) - 1] = '\0';

		int trk = 0, frames = 0, pregap = 0, postgap = 0;
		char type[32] = {0}, subtype[32] = {0};
		char pgtype[32] = {0}, pgsub[32] = {0};

		if (is_cht2) {
			if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT,
			           &trk, type, subtype, &frames,
			           &pregap, pgtype, pgsub, &postgap) < 4) {
				printf("cd_chd: bad CHT2 metadata: %s\n", meta);
				continue;
			}
		} else {
			if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT,
			           &trk, type, subtype, &frames) < 4) {
				printf("cd_chd: bad CHTR metadata: %s\n", meta);
				continue;
			}
			pregap = 0;
		}

		uint8_t t_type = 1, t_ssz = 1;
		if (parse_track_type(type, &t_type, &t_ssz) != 0) {
			printf("cd_chd: unknown TYPE '%s' for track %d, defaulting to MODE1_RAW\n",
			       type, trk);
			t_type = 1; t_ssz = 1;
		}

		cd_track_t *t = &CD.tracks[CD.num_tracks++];
		t->track_no       = (uint8_t)trk;
		t->type           = t_type;
		t->sector_size    = t_ssz;
		t->pregap_lbas    = (uint32_t)pregap;
		t->file_lba       = 0;
		t->bin_offset     = 0;
		t->bin_name[0]    = '\0';

		// CHT2's `FRAMES` field is the TOTAL number of frames the track
		// occupies in the CHD storage AND on the disc — pregap is part of
		// that count, not on top of it. So:
		//   * the track contributes `frames` LBAs to the disc layout
		//   * the data portion (after the pregap) is (frames - pregap) LBAs
		//   * the on-disc INDEX 01 lba_start is running_lba + pregap
		//   * the CHD storage base for this track is chd_frame_cursor; the
		//     INDEX 01 frame lives `pregap` frames in
		// Tracks are then padded to CD_TRACK_PADDING (4) frames in storage.
		uint32_t data_lbas = (uint32_t)frames - (uint32_t)pregap;
		t->chd_frame_start = chd_frame_cursor + (uint32_t)pregap;
		t->lba_start      = running_lba + (uint32_t)pregap;
		t->lba_end        = t->lba_start + data_lbas;

		running_lba       = t->lba_end;
		uint32_t padded   = ((uint32_t)frames + CD_TRACK_PADDING - 1) & ~(CD_TRACK_PADDING - 1);
		chd_frame_cursor += padded;

		if (CD.first_track == 0 || trk < CD.first_track) CD.first_track = (uint8_t)trk;
		if (trk > CD.last_track) CD.last_track = (uint8_t)trk;
	}

	CD.total_lba = running_lba;

	if (CD.num_tracks == 0) {
		printf("cd_chd: no tracks found in CHD metadata\n");
		return -1;
	}

	printf("cd_chd: %u tracks, total_lba=%lu (~%lu MB raw)\n",
	       CD.num_tracks,
	       (unsigned long)CD.total_lba,
	       (unsigned long)((uint64_t)CD.total_lba * CD_RAW_SECTOR_SIZE / (1024 * 1024)));
	for (int i = 0; i < CD.num_tracks; i++) {
		const cd_track_t *t = &CD.tracks[i];
		printf("  T%02u %s lba=[%6lu..%6lu) pre=%4lu chd_frame=%6lu\n",
		       t->track_no,
		       t->type ? "DATA " : "AUDIO",
		       (unsigned long)t->lba_start,
		       (unsigned long)t->lba_end,
		       (unsigned long)t->pregap_lbas,
		       (unsigned long)t->chd_frame_start);
	}
	return 0;
}

// --- Public API ----------------------------------------------------------

bool cd_chd_is_active(void)
{
	return CD.is_chd && CD.chd_handle != NULL;
}

int cd_chd_open(const char *path)
{
	cd_chd_close();   // start clean

	if (f_open(&chd_fil, path, FA_READ) != FR_OK) {
		printf("cd_chd: cannot open %s\n", path);
		return -1;
	}
	chd_fil_open = true;

	chd_file *chd = NULL;
	chd_error err = chd_open_core_file_callbacks(&fatfs_cb, &chd_fil,
	                                             CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE) {
		printf("cd_chd: chd_open failed: %s\n", chd_error_string(err));
		f_close(&chd_fil);
		chd_fil_open = false;
		return -1;
	}
	CD.chd_handle = chd;

	const chd_header *h = chd_get_header(chd);
	if (!h) {
		printf("cd_chd: chd_get_header returned NULL\n");
		cd_chd_close();
		return -1;
	}
	printf("cd_chd: opened %s  v%lu  hunkbytes=%lu  totalhunks=%lu\n",
	       path,
	       (unsigned long)h->version,
	       (unsigned long)h->hunkbytes,
	       (unsigned long)h->totalhunks);

	if (h->hunkbytes % CD_FRAME_SIZE != 0) {
		printf("cd_chd: hunkbytes %lu not a multiple of frame size %d\n",
		       (unsigned long)h->hunkbytes, CD_FRAME_SIZE);
		cd_chd_close();
		return -1;
	}
	cache_init(h->hunkbytes);

	if (load_tracks_from_metadata(chd) != 0) {
		cd_chd_close();
		return -1;
	}

	CD.is_chd = true;
	return 0;
}

void cd_chd_close(void)
{
	cache_free();
	if (CD.chd_handle) {
		chd_close((chd_file *)CD.chd_handle);
		CD.chd_handle = NULL;
	}
	if (chd_fil_open) {
		f_close(&chd_fil);
		chd_fil_open = false;
	}
	CD.is_chd = false;
	chd_hunkbytes = 0;
	chd_frames_per_hunk = 0;
}

// Find the track containing this disc-LBA. Same logic as cd.c's find_track
// but localised here so cd_chd.c can be built independently.
static int chd_find_track(uint32_t lba)
{
	for (int i = 0; i < CD.num_tracks; i++) {
		if (lba >= CD.tracks[i].lba_start && lba < CD.tracks[i].lba_end)
			return i;
	}
	return -1;
}

int cd_chd_read_raw_sector(uint32_t lba, uint8_t *dest)
{
	if (!cd_chd_is_active() || chd_frames_per_hunk == 0) return -1;

	int idx = chd_find_track(lba);
	if (idx < 0) {
		// Pregap region between tracks — return silence so the BIOS doesn't
		// get garbage. Real CDs would play 2 sec of digital silence here.
		memset(dest, 0, CD_RAW_SECTOR_SIZE);
		return 0;
	}
	const cd_track_t *t = &CD.tracks[idx];
	uint32_t track_off = lba - t->lba_start;
	uint32_t chd_frame = t->chd_frame_start + track_off;

	uint32_t hunk_index = chd_frame / chd_frames_per_hunk;
	uint32_t frame_in_hunk = chd_frame % chd_frames_per_hunk;

	uint8_t *hunk = cache_get(hunk_index);
	if (!hunk) return -1;

	const uint8_t *src = hunk + (size_t)frame_in_hunk * CD_FRAME_SIZE;
	memcpy(dest, src, CD_RAW_SECTOR_SIZE);

	// chdman stores CD-DA samples as big-endian (MAME convention); real
	// CDs and our downstream audio path are little-endian 16-bit stereo.
	// Swap on the way out for audio tracks only — data tracks must stay
	// byte-for-byte raw so the BIOS reads the right ECC/sync.
	if (t->type == 0) {
		for (int i = 0; i < CD_RAW_SECTOR_SIZE; i += 2) {
			uint8_t a = dest[i];
			dest[i]   = dest[i + 1];
			dest[i + 1] = a;
		}
	}
	return 0;
}

#endif // ENABLE_CHD
