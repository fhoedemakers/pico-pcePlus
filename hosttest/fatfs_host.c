// FatFs API implemented on top of POSIX for the hosttest harness. Translates
// firmware-style absolute paths ("/bios/SYSCARD3.pce", "/games/rondo/rondo.chd")
// into host paths under $PCE_SD_ROOT (default ".") so the unchanged pce-go
// CD code in cd.c / cd_chd.c finds the files. Relative paths pass through
// unchanged so existing HuCard runs (which fopen via plain argv[1]) keep
// working.
//
// Caveats:
//   - Single-threaded. No locking. cd.c's sd_mutex no-ops on host.
//   - FILINFO only carries fname + fattrib (the only fields the core reads).
//     fsize / mtime are not populated.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ff.h"
// ff.h defines DIR as a macro for FF_DIR (so the core can write `DIR dir;`),
// but in this file we need POSIX's DIR typedef from <dirent.h>. Restore it.
#undef DIR

// Map a firmware path to a host path. Absolute paths get $PCE_SD_ROOT
// prepended (default ".") so /bios/... resolves to <root>/bios/...; relative
// paths pass through. The returned pointer is to one of two rotating static
// buffers — safe for the call patterns in cd.c (one map per FS op).
static const char *map_path(const char *p)
{
	static char bufs[2][FF_MAX_LFN + 64];
	static int  cur = 0;
	if (!p) return p;
	if (p[0] != '/') return p;

	const char *root = getenv("PCE_SD_ROOT");
	if (!root || !*root) root = ".";

	char *out = bufs[cur ^= 1];
	snprintf(out, sizeof(bufs[0]), "%s%s", root, p);
	return out;
}

static FRESULT errno_to_fr(void)
{
	// Coarse: callers only branch on FR_OK vs not-OK.
	return FR_DISK_ERR;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{
	if (!fp || !path) return FR_DISK_ERR;
	const char *m = "rb";
	if (mode & FA_WRITE) m = (mode & FA_CREATE_ALWAYS) ? "wb" : "r+b";
	FILE *f = fopen(map_path(path), m);
	if (!f) return errno_to_fr();

	fseek(f, 0, SEEK_END);
	fp->size = (FSIZE_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	fp->fp = f;
	return FR_OK;
}

FRESULT f_close(FIL *fp)
{
	if (!fp || !fp->fp) return FR_OK;
	fclose((FILE *)fp->fp);
	fp->fp = NULL;
	return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
	if (!fp || !fp->fp || !buff || !br) return FR_DISK_ERR;
	size_t n = fread(buff, 1, btr, (FILE *)fp->fp);
	*br = (UINT)n;
	return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
	if (!fp || !fp->fp || !buff || !bw) return FR_DISK_ERR;
	size_t n = fwrite(buff, 1, btw, (FILE *)fp->fp);
	*bw = (UINT)n;
	return (n == btw) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
	if (!fp || !fp->fp) return FR_DISK_ERR;
	if (fseek((FILE *)fp->fp, (long)ofs, SEEK_SET) != 0) return FR_DISK_ERR;
	return FR_OK;
}

FSIZE_t f_tell(FIL *fp)
{
	if (!fp || !fp->fp) return 0;
	long t = ftell((FILE *)fp->fp);
	return t < 0 ? 0 : (FSIZE_t)t;
}

FRESULT f_opendir(FF_DIR *dp, const char *path)
{
	if (!dp || !path) return FR_DISK_ERR;
	const char *mapped = map_path(path);
	DIR *d = opendir(mapped);
	if (!d) return FR_NO_PATH;
	dp->dp = d;
	strncpy(dp->path, mapped, sizeof(dp->path) - 1);
	dp->path[sizeof(dp->path) - 1] = '\0';
	return FR_OK;
}

FRESULT f_readdir(FF_DIR *dp, FILINFO *fno)
{
	if (!dp || !dp->dp || !fno) return FR_DISK_ERR;
	fno->fname[0] = '\0';
	fno->fattrib  = 0;

	struct dirent *e;
	// Skip "." and ".." but otherwise expose every entry once.
	for (;;) {
		e = readdir((DIR *)dp->dp);
		if (!e) return FR_OK; // signals end of dir (fname[0] == 0)
		if (e->d_name[0] == '.' &&
		    (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
			continue;
		break;
	}
	strncpy(fno->fname, e->d_name, sizeof(fno->fname) - 1);
	fno->fname[sizeof(fno->fname) - 1] = '\0';

	// Stat to derive fattrib (AM_DIR for directories; hidden bit not used
	// on POSIX — leave AM_HID off).
	char full[FF_MAX_LFN * 2 + 64];
	snprintf(full, sizeof(full), "%s/%s", dp->path, e->d_name);
	struct stat st;
	if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
		fno->fattrib |= AM_DIR;
	return FR_OK;
}

char *f_gets(char *buff, int len, FIL *fp)
{
	if (!buff || len <= 0 || !fp || !fp->fp) return NULL;
	return fgets(buff, len, (FILE *)fp->fp);
}

FRESULT f_closedir(FF_DIR *dp)
{
	if (!dp || !dp->dp) return FR_OK;
	closedir((DIR *)dp->dp);
	dp->dp = NULL;
	return FR_OK;
}
