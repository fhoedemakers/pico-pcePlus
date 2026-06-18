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
| `host_main.c` | main loop, OSD callbacks, full-frame buffer, PPM/register/VRAM dumps |
| `stubs.c` | FatFs + CD-subsystem no-op stubs, `frens_f_malloc` → plain `malloc` |
| `shim/pico.h`, `shim/pico/time.h`, `shim/ff.h` | header shims so the core compiles untouched |
| `ppm2png.py` | PPM → PNG converter, Python stdlib only (no PIL/ImageMagick needed) |

## Build

From the repo root:

```sh
gcc -O1 -g -fsanitize=address -DPCE_HOST_DEBUG -I hosttest/shim -I pce-go \
  -o hosttest/pce_host hosttest/host_main.c hosttest/stubs.c \
  pce-go/pce-go.c pce-go/pce.c pce-go/gfx.c pce-go/h6280.c pce-go/psg.c -lm
```

- AddressSanitizer is intentional: it doubles as a memory-bug detector for the
  core (the per-scanline refactor history shows how easy buffer overruns are
  to introduce). Drop `-fsanitize=address` for faster runs.
- `-DPCE_HOST_DEBUG` enables the VDC-solo hook in `mix_vpc_scanline`
  (`pce-go/gfx.c`). The firmware build never defines it, so the device is
  completely unaffected.

## Run

```sh
./hosttest/pce_host <rom> <total-frames> <dump-every-N> [outdir]

# examples
./hosttest/pce_host "Madou King Granzort (Japan).sgx" 1300 100 hosttest/out
python3 hosttest/ppm2png.py hosttest/out/frame_01200.ppm   # -> .png next to it
```

A `.sgx` extension auto-enables SuperGrafx mode (second VDC), same as the
firmware. Frames are written as `outdir/frame_NNNNN.ppm`.

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

The `.bin` dumps are little-endian uint16 arrays (0x8000 words VRAM, 256 words
SPRAM) — decode BAT/SAT tables with a few lines of `struct.unpack`.

## Caveats

- Host runs are fully deterministic: no PSRAM garbage, no input-timing
  variation. A bug that is *intermittent* on the device usually shows up here
  as its always-broken variant.
- CD games cannot load (FatFs/CD stubs fail by design).
- `crc32_le` is stubbed to 0 outside RETRO_GO, so `romFlags` CRC matching
  never hits — keep this in mind when debugging per-game flag behavior.
- Audio is stubbed entirely (`osd_psg_*` are no-ops).
