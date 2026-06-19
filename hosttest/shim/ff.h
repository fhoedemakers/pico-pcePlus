// Host-build shim for FatFs, backed by POSIX (fopen/opendir/...) in
// hosttest/fatfs_host.c. Path translation: any absolute path is
// reinterpreted relative to $PCE_SD_ROOT (default "."), so the firmware
// layout — /bios/, /games/<title>/<title>.chd, BRAM next to the game —
// can be reproduced on disk and the unchanged core code finds it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FF_MAX_LFN 255

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_CREATE_ALWAYS 0x08

#define AM_DIR 0x10
#define AM_HID 0x02

typedef unsigned int UINT;
typedef uint64_t     FSIZE_t;

typedef enum {
	FR_OK = 0,
	FR_DISK_ERR,
	FR_NO_FILE,
	FR_NO_PATH,
	FR_DENIED,
} FRESULT;

// FIL: void *fp holds a host FILE *; size cached at open for f_size().
typedef struct { FSIZE_t size; void *fp; } FIL;

// FILINFO: minimal — only fname / fattrib are read by the core.
typedef struct {
	char     fname[FF_MAX_LFN + 1];
	uint8_t  fattrib;
} FILINFO;

// FF_DIR wraps a POSIX DIR *. The name `DIR` clashes with <dirent.h>, which
// the POSIX backend has to include — so we expose it as FF_DIR and #define
// DIR to FF_DIR for the core. cd.c uses `DIR dir;` verbatim.
typedef struct { void *dp; char path[FF_MAX_LFN + 8]; } FF_DIR;
#ifndef DIR
#define DIR FF_DIR
#endif

FRESULT f_open    (FIL *fp, const char *path, uint8_t mode);
FRESULT f_close   (FIL *fp);
FRESULT f_read    (FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write   (FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek   (FIL *fp, FSIZE_t ofs);
FRESULT f_opendir (FF_DIR *dp, const char *path);
FRESULT f_readdir (FF_DIR *dp, FILINFO *fno);
FRESULT f_closedir(FF_DIR *dp);
char   *f_gets    (char *buff, int len, FIL *fp);

static inline FSIZE_t f_size(FIL *fp) { return fp->size; }
FSIZE_t f_tell(FIL *fp);
