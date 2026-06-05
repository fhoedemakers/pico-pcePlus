// Linker --wrap implementations for malloc/free/calloc/realloc.
//
// Routes all stdlib allocator calls in the picopcePlus binary to PSRAM via
// PicoPlusPsram (lwmem-backed) when PSRAM is up, falling back to newlib's
// real allocator for the early-boot window before PSRAM init. The wrap is
// only enabled when ENABLE_CHD is set, since the sole driver of large
// allocations is libchdr's codec state + hunk map.
//
// Why we need this: libchdr's chd_open allocates ~hundreds of KB (per-hunk
// map for a 700 MB CD ≈ 35000 × 12 B = 420 KB plus zlib/LZMA/zstd codec
// state). The 264 KB SRAM heap can't satisfy that; PSRAM has ~5 MB free.
//
// Address-based dispatch on free(): PSRAM lives in the XIP-mapped range
// 0x11000000+ on Pico Plus 2 / Fruit Jam, separate from SRAM at 0x20000000+.
// We check the pointer's range to decide which allocator owns it. This lets
// a newlib-allocated buffer (allocated before PSRAM was up) still be freed
// correctly via __real_free, while CHD-allocated PSRAM buffers go via Frens.

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "FrensHelpers.h"
#include "PicoPlusPsram.h"

// This file is only compiled when ENABLE_CHD is on (gated in CMakeLists.txt
// via the pico_malloc pre-empt block), so no #if guard needed here.

extern "C" void *__real_malloc(size_t sz);
extern "C" void  __real_free(void *p);
extern "C" void *__real_calloc(size_t n, size_t sz);
extern "C" void *__real_realloc(void *p, size_t sz);

namespace {

// RP2350 SRAM lives at 0x20000000 - 0x20080000 (512 KB on RP2350 / RP2354).
// PSRAM is XIP-mapped at 0x11000000 - 0x11800000 (8 MB on Pico Plus 2 / Fruit
// Jam — region size varies; we cover the maximum any board exposes here).
inline bool addr_is_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x11000000u && a < 0x12000000u;
}

inline bool psram_available() {
#if PICO_RP2350 && PSRAM_CS_PIN
    return Frens::isPsramEnabled();
#else
    return false;
#endif
}

} // namespace

extern "C" void *__wrap_malloc(size_t sz)
{
    if (sz == 0) return nullptr;
    if (psram_available()) {
        return PicoPlusPsram::getInstance().Malloc(sz);
    }
    return __real_malloc(sz);
}

extern "C" void __wrap_free(void *p)
{
    if (!p) return;
    if (addr_is_psram(p)) {
        PicoPlusPsram::getInstance().Free(p);
        return;
    }
    __real_free(p);
}

extern "C" void *__wrap_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (total == 0) return nullptr;
    if (psram_available()) {
        void *p = PicoPlusPsram::getInstance().Malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    return __real_calloc(n, sz);
}

extern "C" void *__wrap_realloc(void *p, size_t sz)
{
    if (!p)      return __wrap_malloc(sz);
    if (sz == 0) { __wrap_free(p); return nullptr; }
    if (addr_is_psram(p)) {
        return PicoPlusPsram::getInstance().Realloc(p, sz);
    }
    return __real_realloc(p, sz);
}
