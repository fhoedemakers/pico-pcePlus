# hosttest — Linux host harness for the pce-go core

Runs the unmodified `pce-go/` emulator core headless on a Linux PC, dumping
frames as images. Useful for debugging core behavior (rendering, CPU, IRQ,
register state) with fast iteration and full instrumentation — no Pico
flashing, no serial console. Only hardware-specific issues (HSTX/DVI output,
SD card, PSRAM latency, audio sinks) still need the real device.

This harness found the SuperGrafx "sprite lists frozen when VPR MINLINE < 14"
bug (Madou King Granzort missing background/sprites, Darius Plus invisible
bullets) in a single afternoon after hardware-side debugging had stalled.

## Layout

| File | Purpose |
|---|---|
| `host_main.c` | main loop, OSD callbacks, full-frame buffer, PPM/register/VRAM dumps, CD audio/ADPCM drain |
| `stubs.c` | `frens_f_malloc/free` → plain `malloc/free` (everything else is real code) |
| `fatfs_host.c` | FatFs API on top of POSIX (`fopen`/`opendir`/...) with `$PCE_SD_ROOT`-prefix path translation |
| `cd_chd_alloc_host.c` | libchdr `chdr_libchdr_*` wrappers → libc; undef'd before `<stdlib.h>` to dodge the global force-include |
| `shim/pico.h`, `shim/pico/time.h`, `shim/pico/mutex.h`, `shim/ff.h` | header shims so the core compiles untouched |
| `build.sh` | mirrors `external/libchdr_pico.cmake` for the host so libchdr links the same way |
| `ppm2png.py` | PPM → PNG converter, Python stdlib only (no PIL/ImageMagick needed) |

## Build

From the repo root:

```sh
hosttest/build.sh   # produces hosttest/pce_host
```

The script compiles `pce-go/cd.c`, `pce-go/cd_chd.c` and the libchdr sources
from `external/libchdr/` directly into the binary, with the same defines the
firmware cmake uses (`ENABLE_CHD=1`, `WANT_RAW_DATA_SECTOR=1`, miniz no-stdio,
etc.). The defines `PCE_HOST_DEBUG` (VDC-solo hook in `gfx.c`) and
`PICO_RP2350=1` (so `LoadDisc()` allocates the CD-DA ring) are host-only.
AddressSanitizer (`-fsanitize=address`) is intentional; drop it from the
script for faster runs.

## Run

```sh
./hosttest/pce_host <rom-or-cue-or-chd> <total-frames> <dump-every-N> [outdir]

# HuCard
./hosttest/pce_host "Madou King Granzort (Japan).sgx" 1300 100 hosttest/out

# CD: lay out files under one root, then run with PCE_SD_ROOT set.
#   $ROOT/bios/Super CD-ROM System (Japan) (v3.0).pce
#   $ROOT/games/foo/foo.cue   (+ tracks alongside)
PCE_SD_ROOT=/path/to/sd PCE_PRESS_RUN=300 \
  ./hosttest/pce_host "/games/foo/foo.cue" 2000 100 hosttest/out_cd

python3 hosttest/ppm2png.py hosttest/out/frame_01200.ppm   # -> .png next to it
```

Extension dispatch: `.sgx` enables SuperGrafx (second VDC); `.cue` or `.chd`
takes the CD path through `LoadDisc()`. Everything else goes through
`LoadCard()`. Frames are written as `outdir/frame_NNNNN.ppm`.

## Environment variables

| Variable | Effect |
|---|---|
| `PCE_PRESS_RUN=<frame>` | tap RUN for 10 frames starting there (gets past title screens) |
| `PCE_HOLD_FIRE=<frame>` | autofire button II (4 frames on / 4 off) from that frame on |
| `PCE_PRESS_KEYS=<f>:<hex>[,<f>:<hex>...]` | hold an arbitrary `JOY_*` bitmask for 10 frames at each given frame (up to 16 entries). Composes with `PCE_PRESS_RUN`/`PCE_HOLD_FIRE`. Masks: I=01, II=02, SELECT=04, RUN=08, UP=10, RIGHT=20, DOWN=40, LEFT=80. Example: `1100:01,1300:40,1500:01` taps I, then DOWN, then I — useful for navigating menus past the title screen. |
| `PCE_SOLO_VDC=1` or `2` | SGX only: mixer outputs just VDC1 or VDC2 — identifies which layer holds what |
| `PCE_DUMP_REGS=1` | print VDC1/VDC2/VPC registers + VRAM nonzero counts every 100 frames |
| `PCE_DUMP_VRAM=1` | write `vram1.bin` `vram2.bin` `spram1.bin` `spram2.bin` to outdir at exit |
| `PCE_QUIRK=<hex>` | OR bits into `PCE.Quirks` after `LoadCard`. Needed because the hosttest `crc32_le` stub returns 0, so per-CRC quirks from `romFlags[]` never auto-trigger here. E.g. `PCE_QUIRK=0x1000` enables `PCE_QUIRK_HW_VDC`. On Pico the real CRC is computed and this env var is ignored. |
| `PCE_SD_ROOT=<dir>` | CD only. Root that any **absolute** FatFs path is reinterpreted relative to. The firmware lays out `/bios/<systemcard>.pce` and per-game folders at the SD root; replicate that under `$PCE_SD_ROOT` on host. Default: `.` (relative paths pass through unchanged, so HuCard runs are unaffected). |

The `.bin` dumps are little-endian uint16 arrays (0x8000 words VRAM, 256 words
SPRAM) — decode BAT/SAT tables with a few lines of `struct.unpack`.

## Caveats

- Host runs are fully deterministic: no PSRAM garbage, no input-timing
  variation, no SD/QMI contention. Bugs that are *intermittent* on the device
  usually show up here as their always-broken variant — but timing-dependent
  bugs (HSTX crackle, CHD prefetch underruns, audio-clock PLL behavior) do
  **not** reproduce here. Use the host for logic bugs (CUE parsing, SCSI
  command sequencing, ADPCM decoder, BIOS interactions); keep the device
  for sink/SD/PSRAM-latency bugs.
- BIOS CRC is computed correctly (cd.c's own `crc32_update`), so System
  Card / Arcade Card variant detection works on host. The `crc32_le` stub
  in `pce-go.c` still returns 0 though, so `romFlags`-style per-game quirks
  don't auto-fire — use `PCE_QUIRK=` for those.
- PSG (`osd_psg_*`) is still stubbed. CD-DA and ADPCM **are** generated each
  frame to keep the audio state machines moving (otherwise games hang on
  end-of-track IRQs or ADPCM-playing polling) but the samples are discarded.
- The host harness is single-threaded; `pico/mutex.h` is a no-op shim. The
  sd_mutex contention paths in `cd_audio_update` never engage here.
