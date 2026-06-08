# pico-pcePlus

## Introduction

**pico-pcePlus** is a PC Engine / TurboGrafx-16 emulator for RP2350-based microcontrollers. It is built on the [pce-go](https://github.com/ducalex/retro-go) emulation core from retro-go by ducalex, integrated with the video, audio, menu, and SD card framework from [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

> [!IMPORTANT]
> An **RP2350** board is required. The original RP2040 (Pico 1) is not supported.

This project is part of a family of Raspberry Pi Pico emulator projects:

- NES: [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus)
- Sega Master System / Game Gear: [pico-smsplus](https://github.com/fhoedemakers/pico-smsplus)
- Game Boy / Game Boy Color: [pico-peanutGB](https://github.com/fhoedemakers/pico-peanutGB)
- Sega Mega Drive / Genesis: [pico-genesisPlus](https://github.com/fhoedemakers/pico-genesisPlus)

***

## What it emulates

- **HuCard ROMs** — Standard PC Engine / TurboGrafx-16 cartridge dumps (`.pce`) are loaded directly from the SD card.
- **SuperGrafx (SGX)** — SuperGrafx titles are recognised and dispatched to the second VDC. SuperGrafx support requires PSRAM. SGX emulation is **still under development**: a number of titles exhibit graphical glitches (for example background corruption in certain games) and not all SuperGrafx games are fully playable.
- **CD-ROM²** — CD-ROM² and Super CD-ROM² games are supported, including CD-DA audio playback and ADPCM streaming. CD-ROM² playback requires PSRAM and a System Card BIOS supplied by the user.
- **Save states** — Manual save and load slots are available through the in-game menu. An optional auto-save mode stores a state when the game exits and offers to resume it the next time the same ROM is launched. State files are stored on the SD card under `/savestates/PCE/<CRC32>/`.
- **Backup RAM (BRAM)** — CD-ROM² games that use the System Card's BRAM (for in-game save data) have it persisted automatically alongside the save states.

***

## CD-ROM² disc formats

Two disc-image formats are accepted:

- **CUE/BIN** — Uncompressed CD images. This is the recommended format.
- **CHD** — Compressed disc images (MAME's CHD format). CHD images are decompressed sector-by-sector at run time and are therefore generally slower than CUE/BIN. They are useful when SD card space is limited; otherwise CUE/BIN is preferred.

Each disc image (and its associated tracks for CUE/BIN) should be placed in its own folder on the SD card.

***

## Hardware requirements

The emulator runs on RP2350-based boards in two configurations:

- **RP2350 without PSRAM** — Supports HuCard ROMs, save states, and metadata display.
- **RP2350 with PSRAM** — In addition to the above, enables SuperGrafx titles, CD-ROM² playback (CUE/BIN or CHD), Backup RAM persistence, and audio recording.

For board-by-board wiring, supported display modes, and which UF2 file to flash, refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup). The set of supported boards and their pinouts are identical between the two projects.

***

## CD-ROM² BIOS

CD-ROM² games require a System Card BIOS to boot. The BIOS file must be supplied by the user; it is not distributed with the emulator.

### Placement

- Create a `/bios/` folder in the root of the SD card and place one or more BIOS files there (with a `.pce` extension, or named `cd_bios.rom`).
- Alternatively, place a BIOS file alongside a disc image; this takes precedence over `/bios/` and allows a per-game override.

The emulator identifies BIOS files by CRC32 and, when several are available, selects the most capable variant automatically. For maximum compatibility the **Super CD-ROM System v3.0 (JP)** or the **Arcade Card Pro (JP)** BIOS is recommended.

> [!NOTE]
> Without a recognised BIOS file in `/bios/` (or alongside the disc image), CD-ROM² games will fail to load.

***

## Metadata

The emulator can display box art and a short text description for each ROM when a metadata pack is present on the SD card. With the pack installed, pressing **START** on a ROM in the file browser displays its metadata; the screensaver also shows random box art.

A metadata pack can be downloaded from the [releases page](https://github.com/fhoedemakers/pico-pcePlus/releases) and extracted to the root of the SD card. It is installed under:

```
/metadata/PCE/
├── images/   (box art, named by ROM CRC32)
└── descr/    (text descriptions, named by ROM CRC32)
```

***

## Controls

The emulator presents the PC Engine controller mapping on any connected USB controller, NES/SNES controller (directly wired), or Wii Classic Controller (via I²C). Supported USB devices include Sony DualShock 4 / DualSense, Sega Genesis Mini 1 and 2, Retro-bit 8-button Genesis-USB, PlayStation Classic, USB keyboards, and XInput-compatible controllers (Xbox 360, Xbox One, 8BitDo, and similar).

The PC Engine controller has a D-pad, two action buttons (**I** and **II**), and two system buttons (**SELECT** and **RUN**).

During gameplay, **SELECT + RUN** opens the in-game settings menu. All other in-game adjustments — screen mode, scanlines, FPS overlay, VU meter, external (I²S) audio output, volume, save and restore state, rapid-fire, audio recording, and so on — are reached from this menu.

***

## Building from source

### Prerequisites

- The [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk), with the `PICO_SDK_PATH` environment variable set.
- CMake 3.13 or later.
- The ARM GCC toolchain (and, for RISC-V builds, the RISC-V toolchain in `$PICO_SDK_PATH/toolchain/RISCV_RPI_2_0_0_2/bin`).
- `picotool` on the `PATH`.

### Clone and build

```bash
git clone --recurse-submodules https://github.com/fhoedemakers/pico-pcePlus.git
cd pico-pcePlus
```

Run `./bld.sh -h` to see the available build options for a single configuration, or `./buildAll.sh` to build every supported configuration and collect the resulting UF2 files in the `releases` folder. For the mapping between configuration numbers and specific boards, consult the [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) documentation.

***

## Flashing

1. Hold the **BOOTSEL** button on the RP2350 board while connecting it to a computer via USB.
2. The board mounts as a USB mass-storage device.
3. Copy the appropriate `.uf2` file to the device.
4. The board reboots automatically and starts the emulator.

***

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](LICENSE) file for details.

***

## Credits

### Emulation cores and frameworks

- [pce-go](https://github.com/ducalex/retro-go) — PC Engine / TurboGrafx-16 emulation core, by ducalex (part of retro-go).
- [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) — shared video, audio, menu, and SD card infrastructure, by Frank Hoedemakers.
- [Mesen2](https://github.com/SourMesen/Mesen2) — used as a reference for the PC Engine CD ADPCM decoder port.

### Video and display

- [PicoDVI](https://github.com/Wren6991/PicoDVI) — DVI output library by Wren6991.
- [pico_lib](https://github.com/shuichitakano/pico_lib) — PicoDVI audio support by [Shuichi Takano](https://github.com/shuichitakano).
- [pico_hdmi](https://github.com/fliperama86/pico_hdmi) — HSTX HDMI/DVI driver with audio by [fliperama86](https://github.com/fliperama86).

### Disc images, storage, and memory

- [libchdr](https://github.com/rtissera/libchdr) — CHD disc-image decompression, with its bundled miniz, LZMA SDK, and zstd dependencies.
- [pico_fatfs](https://github.com/elehobica/pico_fatfs) — FatFs SD card driver by [elehobica](https://github.com/elehobica).
- [PicoPlusPsram](https://github.com/AndrewCapon/PicoPlusPsram) — PSRAM driver by [AndrewCapon](https://github.com/AndrewCapon).
- [lwmem](https://github.com/MaJerle/lwmem) — lightweight memory allocator by [MaJerle](https://github.com/MaJerle).

### USB and controllers

- [TinyUSB](https://github.com/hathach/tinyusb) — USB host stack.
- [tusb_XInput](https://github.com/Ryzee119/tusb_XInput) — XInput controller driver by [Ryzee119](https://github.com/Ryzee119).
- NES gamepad and Wii Classic Controller support contributed by [PaintYourDragon](https://github.com/PaintYourDragon) and [Adafruit](https://github.com/adafruit).

### Hardware and assets

- PCB design by [John Edgar Park](https://twitter.com/johnedgarpark).
- Additional PCB design and 3D-printable cases (for both PCBs and the WaveShare RP2040/RP2350-PiZero) by [Gavin Knight](https://github.com/DynaMight1124).
- Metadata files provided by [Gavin Knight](https://github.com/DynaMight1124), based on [Ducalex's retro-go-covers](https://github.com/ducalex/retro-go-covers).

### Contributions and assistance

- [Anthropic Claude (Opus 4.6 and 4.7)](https://www.anthropic.com/claude/opus) assisted with the PSG sample-accurate rewrite, the ADPCM decoder port, CHD integration, SuperGrafx work, and general bug fixes.
