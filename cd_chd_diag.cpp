// CHD feasibility-spike diagnostic.
//
// Opens every .chd file in /diag/ and times libchdr's hunk decompression
// against the SD card so we can decide whether RP2350 has the CPU + I/O
// headroom for real-time CHD playback at PCE CD rates (~176 KB/s sustained
// audio, ~150 KB/s data) before committing to full integration.
//
// Reports per-hunk decode latency in microseconds and the equivalent
// sustained MB/s. CD-DA streaming requires ~9 hunks/sec at the default
// 19584-byte hunk size, so anything under ~10 ms/hunk leaves comfortable
// headroom; >50 ms/hunk would mean borderline; >100 ms is a no-go.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ff.h"
#include "pico/time.h"

extern "C" {
#include "libchdr/chd.h"
#include "libchdr/coretypes.h"
}

namespace {

// ---- FatFs ↔ libchdr core_file_callbacks adapter -------------------------
//
// libchdr expects fread/fseek/fsize/fclose in fopen-style. We marshal those
// onto FatFs FIL through a thin static wrapper. The void* argp passed to
// libchdr is a FIL*.

uint64_t cb_fsize(void *fp)
{
    FIL *f = (FIL *)fp;
    return (uint64_t)f_size(f);
}

size_t cb_fread(void *buf, size_t sz, size_t cnt, void *fp)
{
    FIL *f = (FIL *)fp;
    UINT br = 0;
    if (sz == 0 || cnt == 0) return 0;
    // f_read takes UINT count of bytes; we read sz*cnt bytes and return
    // how many full items came back.
    UINT request = (UINT)(sz * cnt);
    if (f_read(f, buf, request, &br) != FR_OK) return 0;
    return br / sz;
}

int cb_fclose(void *fp)
{
    FIL *f = (FIL *)fp;
    f_close(f);
    return 0;
}

int cb_fseek(void *fp, int64_t off, int whence)
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

const core_file_callbacks fatfs_cb = {
    cb_fsize,
    cb_fread,
    cb_fclose,
    cb_fseek,
};

// Decode `count` hunks starting at `start` (with `stride` between them so
// we cover the file rather than re-hitting the same cached hunk). Returns
// total elapsed microseconds across all reads, or -1 on read error.
int64_t time_hunks(chd_file *chd, uint8_t *dest, uint32_t hunkbytes,
                   uint32_t total_hunks,
                   uint32_t start, uint32_t stride, uint32_t count)
{
    if (start >= total_hunks) return 0;
    int64_t total_us = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t h = start + i * stride;
        if (h >= total_hunks) break;
        absolute_time_t t0 = get_absolute_time();
        chd_error err = chd_read(chd, h, dest);
        absolute_time_t t1 = get_absolute_time();
        if (err != CHDERR_NONE) {
            printf("[chd] hunk %lu read failed: %s\n",
                   (unsigned long)h, chd_error_string(err));
            return -1;
        }
        int64_t us = absolute_time_diff_us(t0, t1);
        printf("[chd]   hunk %7lu: %6lld us\n", (unsigned long)h, (long long)us);
        total_us += us;
    }
    return total_us;
}

void diag_one_chd(const char *path)
{
    printf("[chd] === %s ===\n", path);

    // Open the underlying FatFs handle once and hand its address to libchdr.
    // libchdr will fclose() it via our callback when chd_close runs.
    static FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        printf("[chd]   f_open failed\n");
        return;
    }

    chd_file *chd = nullptr;
    absolute_time_t open_t0 = get_absolute_time();
    chd_error err = chd_open_core_file_callbacks(&fatfs_cb, &fil,
                                                 CHD_OPEN_READ,
                                                 nullptr, &chd);
    absolute_time_t open_t1 = get_absolute_time();
    if (err != CHDERR_NONE) {
        printf("[chd]   chd_open failed: %s\n", chd_error_string(err));
        // chd_open closes the file on failure path? Not guaranteed — close
        // it here to be safe in either case.
        return;
    }

    const chd_header *h = chd_get_header(chd);
    if (!h) {
        printf("[chd]   no header\n");
        chd_close(chd);
        return;
    }
    printf("[chd]   version=%lu  hunkbytes=%lu  totalhunks=%lu  logical=%llu\n",
           (unsigned long)h->version,
           (unsigned long)h->hunkbytes,
           (unsigned long)h->totalhunks,
           (unsigned long long)h->logicalbytes);
    printf("[chd]   chd_open: %lld us\n",
           (long long)absolute_time_diff_us(open_t0, open_t1));

    // Allocate scratch for one decompressed hunk. Goes through stdlib malloc
    // which lives in SRAM in the Pico SDK build — fine for the spike.
    uint8_t *dest = (uint8_t *)malloc(h->hunkbytes);
    if (!dest) {
        printf("[chd]   malloc(%lu) failed\n", (unsigned long)h->hunkbytes);
        chd_close(chd);
        return;
    }

    // Decode a handful of hunks spread across the file: front, mid, back.
    // Stride pulls fresh hunks each iteration so we measure cold decode,
    // not cache hits from the codec / SD readahead.
    const uint32_t total = h->totalhunks;
    uint32_t count = 8;
    if (count > total) count = total;
    uint32_t stride = (total > count) ? (total / count) : 1;
    if (stride == 0) stride = 1;

    int64_t total_us = time_hunks(chd, dest, h->hunkbytes, total,
                                  0, stride, count);

    if (total_us > 0) {
        uint64_t bytes = (uint64_t)count * h->hunkbytes;
        // MB/s = bytes / total_us. (1 MB = 1048576 B; 1 s = 1e6 us)
        // (bytes / 1048576) / (total_us / 1e6) = bytes*1e6 / (total_us*1048576)
        // Render as integer milli-MB/s to avoid float in printf.
        uint64_t milli_mbps = (bytes * 1000ULL * 1000000ULL)
                            / ((uint64_t)total_us * 1048576ULL);
        int64_t avg_us = total_us / count;
        printf("[chd]   avg %lld us/hunk = %lu.%03lu MB/s\n",
               (long long)avg_us,
               (unsigned long)(milli_mbps / 1000),
               (unsigned long)(milli_mbps % 1000));

        // PCE CD streams ~176 KB/s = ~9 hunks/sec at 19584 B/hunk.
        // Time budget per hunk: 1e6/9 ≈ 111000 us. Anything <10000 us has
        // ample headroom; 10000-50000 us is workable; >50000 us is tight.
        const char *verdict = "GOOD";
        if (avg_us > 100000) verdict = "TOO SLOW";
        else if (avg_us > 50000) verdict = "TIGHT";
        else if (avg_us > 10000) verdict = "OK";
        printf("[chd]   verdict: %s (budget ~111000 us/hunk for CD-DA)\n", verdict);
    }

    free(dest);
    chd_close(chd);
}

} // anonymous namespace

extern "C" void cd_chd_diag_run(void)
{
    static DIR     dir;
    static FILINFO fno;
    static char    path[FF_MAX_LFN + 8];

    if (f_opendir(&dir, "/diag") != FR_OK) {
        return;
    }
    printf("--- CHD timing diagnostic (/diag/) ---\n");
    int count = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & (AM_DIR | AM_HID)) continue;
        size_t n = strlen(fno.fname);
        if (n < 4 || strcasecmp(fno.fname + n - 4, ".chd") != 0) continue;
        snprintf(path, sizeof(path), "/diag/%s", fno.fname);
        diag_one_chd(path);
        count++;
    }
    f_closedir(&dir);
    if (count == 0) {
        printf("[chd] /diag/ has no .chd files\n");
    }
    printf("--------------------------------------\n");
}
