// Host-build stubs: FatFs + CD subsystem + PSRAM allocator. The harness only
// runs HuCard/SGX ROMs, so the CD paths just need to link and no-op.
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "cd.h"
#include "cd_chd.h"

cd_state_t CD;

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) { (void)fp; (void)path; (void)mode; return FR_DISK_ERR; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) { (void)fp; (void)buff; (void)btr; *br = 0; return FR_DISK_ERR; }

int  cd_init(void) { return 0; }
void cd_reset(void) {}
void cd_term(void) {}
uint8_t cd_read(uint16_t addr) { (void)addr; return 0xFF; }
void    cd_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
uint8_t cd_acd_read_bank(uint8_t port) { (void)port; return 0xFF; }
void    cd_acd_write_bank(uint8_t port, uint8_t val) { (void)port; (void)val; }
void cd_setup_memory_map(void) {}
int  cd_load_cue(const char *cue_path) { (void)cue_path; return -1; }
int  cd_find_bios(char *out_path, size_t size, const char *primary_dir, bios_variant_t *variant)
{ (void)out_path; (void)size; (void)primary_dir; (void)variant; return -1; }
bool cd_bios_is_us(void) { return false; }

int  cd_chd_open(const char *path) { (void)path; return -1; }
void cd_chd_close(void) {}
bool cd_chd_is_active(void) { return false; }
int  cd_chd_read_raw_sector(uint32_t lba, uint8_t *dest) { (void)lba; (void)dest; return -1; }

void *frens_f_malloc(size_t size) { return malloc(size); }
void  frens_f_free(void *ptr) { free(ptr); }
