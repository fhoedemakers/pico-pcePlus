#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CHD-backed disc image: opens the file, parses the per-track metadata into
// CD.tracks[], allocates the hunk cache. Sets CD.is_chd = true so the rest
// of cd.c dispatches reads through cd_chd_read_sector instead of FatFs.
// Returns 0 on success, -1 on failure (caller must call cd_chd_close on
// the failure path if it wants to release any partial allocations).
int cd_chd_open(const char *path);

// Tear down: chd_close, free hunk cache, clear CD.is_chd. Idempotent.
void cd_chd_close(void);

// True if a CHD image is currently mounted (i.e. CD.is_chd && handle live).
bool cd_chd_is_active(void);

// Read one disc LBA into a 2352-byte buffer (raw sector). For data tracks
// the buffer starts with the 16-byte CD sync header; callers that want
// MODE1 user data should skip the first 16 bytes and copy 2048. Returns 0
// on success, -1 on read failure or invalid LBA.
int cd_chd_read_raw_sector(uint32_t lba, uint8_t *dest);

#ifdef __cplusplus
}
#endif
