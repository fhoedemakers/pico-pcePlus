#include "state.h"
#include "FrensHelpers.h"
#include "ff.h"
#include <string.h>

extern "C"
{
#include "pce-go.h"
#include "pce.h"
#include "gfx.h"
}

int Emulator_SaveState(const char *path)
{
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    FRESULT fr = f_open(fil, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        printf("Cannot open save state file: %d (%s)\n", fr, path);
        Frens::f_free(fil);
        return -1;
    }

    UINT bw;
    f_write(fil, SAVESTATE_HEADER, 8, &bw);

    for (save_var_t *var = SaveStateVars; var->ptr; var++)
    {
        void *ptr = var->desc.type == 5 ? *((void **)var->ptr) : var->ptr;
        size_t len = var->desc.len;
        f_write(fil, &var->desc, sizeof(var->desc), &bw);
        f_write(fil, ptr, len, &bw);
    }

    FRESULT frc = f_close(fil);
    Frens::f_free(fil);

    if (frc != FR_OK)
    {
        printf("Error closing save state file: %d (%s)\n", frc, path);
        return -1;
    }
    return 0;
}

int Emulator_LoadState(const char *path)
{
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    FRESULT fr = f_open(fil, path, FA_READ);
    if (fr != FR_OK)
    {
        printf("Cannot open load state file: %d (%s)\n", fr, path);
        Frens::f_free(fil);
        return -1;
    }

    char header[8];
    UINT br;
    f_read(fil, header, 8, &br);
    if (br != 8 || memcmp(header, SAVESTATE_HEADER, 8) != 0)
    {
        printf("Save state header mismatch (%s)\n", path);
        f_close(fil);
        Frens::f_free(fil);
        return -1;
    }

    block_hdr_t block;
    while (f_read(fil, &block, sizeof(block), &br) == FR_OK && br == sizeof(block))
    {
        FSIZE_t block_end = f_tell(fil) + block.len;

        for (save_var_t *var = SaveStateVars; var->ptr; var++)
        {
            if (strncmp(var->desc.key, block.key, 12) == 0)
            {
                void *ptr = var->desc.type == 5 ? *((void **)var->ptr) : var->ptr;
                size_t len = var->desc.len < (size_t)block.len ? var->desc.len : (size_t)block.len;
                f_read(fil, ptr, len, &br);
                if (len < var->desc.len)
                    memset((uint8_t *)ptr + len, 0, var->desc.len - len);
                break;
            }
        }
        f_lseek(fil, block_end);
    }

    FRESULT frc = f_close(fil);
    Frens::f_free(fil);

    if (frc != FR_OK)
    {
        printf("Error closing load state file: %d (%s)\n", frc, path);
        return -1;
    }

    for (int i = 0; i < 8; i++)
        pce_bank_set(i, PCE.MMR[i]);

    gfx_reset(true);
    PCE.VDC.mode_chg = 1;

    return 0;
}
