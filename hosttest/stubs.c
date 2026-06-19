// Host-build stubs: the FatFs API is implemented in fatfs_host.c. The real
// CD subsystem (pce-go/cd.c + pce-go/cd_chd.c) is linked directly, so the
// only stub left is the PSRAM allocator (host has no PSRAM — plain malloc).
//
// frens_f_realloc isn't referenced by the host build today (only the libchdr
// alloc-wrap calls it on Pico), so we don't define it here. Add it if a
// future caller pulls it in.
#include <stdlib.h>

void *frens_f_malloc(size_t size) { return malloc(size); }
void  frens_f_free(void *ptr) { free(ptr); }
