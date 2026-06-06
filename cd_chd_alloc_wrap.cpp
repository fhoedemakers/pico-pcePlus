// PSRAM-backed allocator for libchdr only.
//
// libchdr's chd_open allocates ~hundreds of KB (per-hunk map ~450 KB plus
// zlib/LZMA/zstd codec state) which would blow out the 264 KB SRAM heap.
// Earlier we tried linker --wrap on malloc/calloc/realloc/free, but that
// redirected EVERY allocation in the binary — including the emulator's
// own runtime mallocs for PCE.VRAM (64 KB), PCE.RAM, MemoryMap, etc.,
// which need to stay in the fast SRAM heap. Pushing them into PSRAM
// (~10-20× slower) starved the HSTX video DMA timing.
//
// Current design: the libchdr_pico build adds a `-include` that turns
// every malloc/free/calloc/realloc CALL inside libchdr's translation
// units into a call to one of these chdr_libchdr_* functions, which
// route through Frens::f_malloc (PSRAM). No other code in the binary
// is affected — emulator allocations continue to land in SRAM via the
// regular C library.

#include <stddef.h>
#include <string.h>

// C wrappers from pico_shared/FrensHelpers.cpp; declared locally to avoid
// pulling FrensHelpers.h (which transitively includes ff.h) into this
// translation unit and onto libchdr's include path.
extern "C" void *frens_f_malloc(size_t size);
extern "C" void  frens_f_free(void *ptr);
extern "C" void *frens_f_realloc(void *ptr, size_t newSize);

extern "C" void *chdr_libchdr_malloc(size_t sz)
{
    if (sz == 0) return nullptr;
    return frens_f_malloc(sz);
}

extern "C" void chdr_libchdr_free(void *p)
{
    if (!p) return;
    frens_f_free(p);
}

extern "C" void *chdr_libchdr_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (total == 0) return nullptr;
    void *p = frens_f_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

extern "C" void *chdr_libchdr_realloc(void *p, size_t sz)
{
    if (!p)      return chdr_libchdr_malloc(sz);
    if (sz == 0) { chdr_libchdr_free(p); return nullptr; }
    return frens_f_realloc(p, sz);
}
