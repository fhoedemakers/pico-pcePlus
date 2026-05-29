#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CD_MAX_TRACKS    100
#define CD_SECTOR_SIZE   2048
#define CD_RAW_SECTOR_SIZE 2352
#define CD_BIN_NAME_MAX  96    // per-track BIN filename (relative to CUE dir)

// CUE sheet track descriptor. Tracks are stored sequentially as they appear
// in the CUE. lba_start / lba_end are *disc-level* LBAs (concatenated across
// all BIN files); bin_offset is the byte offset *within* this track's own
// BIN file where lba_start lives. file_lba is the per-BIN LBA from INDEX 01,
// which differs from lba_start whenever the disc spans multiple BIN files.
typedef struct {
	char     bin_name[CD_BIN_NAME_MAX]; // BIN filename (relative to CUE dir)
	uint32_t lba_start;     // Disc-level LBA where this track begins
	uint32_t lba_end;       // Disc-level LBA where this track ends (exclusive)
	uint64_t bin_offset;    // Byte offset within bin_name where lba_start sits
	uint32_t file_lba;      // LBA within this track's BIN file (INDEX 01)
	uint8_t  type;          // 0 = audio, 1 = data
	uint8_t  sector_size;   // 0 = 2048, 1 = 2352
	uint8_t  track_no;      // 1-based track number
} cd_track_t;

// SCSI command state machine phases
typedef enum {
	CDC_PHASE_IDLE = 0,
	CDC_PHASE_COMMAND,
	CDC_PHASE_DATA_IN,
	CDC_PHASE_DATA_OUT,
	CDC_PHASE_STATUS,
	CDC_PHASE_MESSAGE,
	CDC_PHASE_BUSY,
} cdc_phase_t;

// BIOS variants identified by CRC
typedef enum {
	BIOS_UNKNOWN = 0,
	BIOS_SC1,           // System Card 1.0 (JP)
	BIOS_SC2,           // System Card 2.0 (JP)
	BIOS_SC3,           // System Card 3.0 / Super System Card (JP)
	BIOS_ACD_DUO,       // Arcade Card Duo (JP)
	BIOS_ACD_PRO,       // Arcade Card Pro (JP)
	BIOS_GEX,           // Games Express CD Card (JP)
	BIOS_TGCD_US,       // US TurboGrafx-CD System Card
} bios_variant_t;

// Complete CD-ROM hardware state
typedef struct {
	// CDC state machine
	uint8_t  scsi_command[16];
	uint8_t  scsi_cmd_len;
	uint8_t  scsi_cmd_idx;
	cdc_phase_t phase;

	// Data transfer
	uint8_t  sector_buf[CD_SECTOR_SIZE];
	uint32_t data_index;
	uint32_t data_length;
	uint32_t read_lba;
	uint32_t read_remaining;

	// ADPCM (registers only in Phase 1, no playback)
	uint8_t  *adpcm_ram;        // 64KB in PSRAM
	uint16_t adpcm_addr_port;   // address port ($1808/$1809)
	uint16_t adpcm_read_addr;
	uint16_t adpcm_write_addr;
	uint16_t adpcm_length;
	uint8_t  adpcm_read_buf;
	uint8_t  adpcm_ctrl;
	uint8_t  adpcm_dma_ctrl;
	uint8_t  adpcm_status;
	uint8_t  adpcm_rate;
	uint8_t  adpcm_fade;

	// BRAM (battery-backed save RAM): 8KB page allocated in PSRAM.
	// Only the first 0x800 bytes are real BRAM; the remaining 6KB mimics
	// open-bus behaviour (filled with 0xFF). Save/load operates on the
	// first 0x800 bytes only.
	uint8_t  *bram;
	bool     bram_locked;

	// RAM (allocated from PSRAM)
	uint8_t  *cd_ram;           // 64KB CD-ROM RAM (pages 0x80-0x87)
	uint8_t  *scd_ram;          // 192KB Super System Card RAM (pages 0x68-0x7F)

	// Arcade Card (2MB RAM + 4 port-mapped bank windows)
	uint8_t  *acd_ram;          // 2MB Arcade Card RAM (PSRAM)
	struct {
		uint32_t base;          // 24-bit base address
		uint16_t offset;        // 16-bit offset
		uint16_t increment;     // 16-bit auto-increment value
		uint8_t  control;       // raw control byte
		bool     auto_inc;      // bit 0: auto-increment on access
		bool     add_offset;    // bit 1: add offset to base for address
		bool     sign_offset;   // bit 3: treat offset as signed (add 0xFF0000)
		bool     inc_to_base;   // bit 4: add increment to base (else to offset)
		uint8_t  off_trigger;   // bits 5-6: 0=none 1=low 2=high 3=reg0A
	} acd_port[4];
	uint32_t acd_value;         // 32-bit shift/rotate value register
	uint8_t  acd_shift;
	uint8_t  acd_rotate;

	// Disc info (parsed from CUE)
	cd_track_t tracks[CD_MAX_TRACKS];
	char     cue_dir[256];   // directory containing the CUE (with trailing /)
	uint8_t  num_tracks;
	uint8_t  first_track;
	uint8_t  last_track;
	uint32_t total_lba;

	// Sense data
	uint8_t  sense_key;
	uint8_t  sense_asc;
	uint8_t  sense_ascq;

	// CD Audio playback
	uint8_t  audio_status;      // 0=playing, 1=inactive, 2=paused, 3=stopped
	uint32_t audio_start_lba;
	uint32_t audio_end_lba;
	uint32_t audio_cur_lba;
	uint16_t audio_cur_sample;  // 0-587 within current sector
	uint8_t  audio_end_mode;    // 0=stop, 1=loop, 2=IRQ, 3=stop+status

	// Audio sector ring buffer (4 * CD_RAW_SECTOR_SIZE)
	uint8_t  *audio_ring_buf;
	uint8_t  audio_ring_write;
	uint8_t  audio_ring_read;
	uint8_t  audio_ring_count;  // filled slots (0-4)

	// IRQ state
	uint8_t  irq_mask;
	uint8_t  irq_status;

	// BIOS identification
	bios_variant_t bios_variant;
	bool     bios_is_us;

	// CD attached flag
	bool     cd_attached;
} cd_state_t;

extern cd_state_t CD;

// Lifecycle
int  cd_init(void);
void cd_reset(void);
void cd_term(void);

// I/O register access (called from pce_readIO / pce_writeIO)
uint8_t cd_read(uint16_t addr);
void    cd_write(uint16_t addr, uint8_t val);

// Arcade Card bank $40-$43 memory-mapped access
uint8_t cd_acd_read_bank(uint8_t port);
void    cd_acd_write_bank(uint8_t port, uint8_t val);

// Memory map setup (called after RAM is allocated and BIOS is mapped)
void cd_setup_memory_map(void);

// Disc image management
int  cd_load_cue(const char *cue_path);
void cd_close(void);

// CD Audio (called from main loop each frame)
void cd_audio_update(void);
int  cd_audio_generate_samples(int16_t *out, int num_samples);

// ADPCM playback timing (no decode — just length countdown at correct rate)
void cd_adpcm_update(void);

// BRAM persistence
int  cd_bram_save(const char *path);
int  cd_bram_load(const char *path);

// BIOS discovery: scans /bios/ for .pce files, CRCs each, picks best match.
// On success: writes path into out_path (size bytes), sets *out_variant and
// CD.bios_is_us, returns 0. On failure returns -1.
int  cd_find_bios(char *out_path, size_t size, bios_variant_t *out_variant);

// True if the currently loaded BIOS is the US TG-CD variant (affects joypad
// region bit). Returns false when no CD game is loaded.
bool cd_bios_is_us(void);

#ifdef __cplusplus
}
#endif
