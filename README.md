# pico-pcePlus

## Introduction

**pico-pcePlus** is a PC Engine / TurboGrafx-16 emulator for Raspberry Pi Pico 2 and other RP2350-based microcontrollers. It provides PC Engine emulation with SD card support, an integrated menu system, and HDMI video output, enabling users to build compact, affordable retro gaming systems.

> [!IMPORTANT]
> This emulator requires an **RP2350**-based board. RP2040 boards (original Pico) are not supported.
> CD-ROM² games additionally require **PSRAM** and a valid **CD-ROM BIOS** file on the SD card.

This project is based on [pce-go](https://github.com/ducalex/retro-go) from retro-go by ducalex, using the video/audio framework from [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

### Features

- **PC Engine / TurboGrafx-16 Emulation** – Execute HuCard ROM files (`.pce`) directly from an SD card
- **CD-ROM² Support** – Play CD-ROM² games using CUE/BIN disc images with CD-DA audio and ADPCM playback (requires PSRAM)
- **SD Card Menu System** – Browse and launch games from an on-screen menu interface
- **Dual Controller Support** – Two simultaneous controllers for multiplayer gameplay
- **Save State Management** – Save and restore game states
- **Audio** – PSG sound with per-scanline generation, CD-DA audio mixing, and ADPCM decoding
- **Multiple Display Modes** – DVI (PicoDVI) and HSTX output with configurable screen modes
- **Flexible Hardware** – Compatible with standard DVI/HDMI breakout boards and various RP2350 boards

### Project Information

This is part of a family of Raspberry Pi Pico emulator projects:

- NES: [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus)
- Sega Master System / Game Gear: [pico-smsplus](https://github.com/fhoedemakers/pico-smsplus)
- Nintendo Game Boy / Game Boy Color: [pico-peanutGB](https://github.com/fhoedemakers/pico-peanutGB)
- Sega Mega Drive / Genesis: [pico-genesisPlus](https://github.com/fhoedemakers/pico-genesisPlus)

***

## Setup Overview

1. Prepare an SD card formatted as FAT32 or exFAT
2. Transfer PC Engine ROM files (`.pce`) to the card (subdirectory organization is supported)
3. For CD-ROM² games:
   - Place CUE/BIN disc image files on the SD card
   - Install a compatible BIOS file (see [CD-ROM BIOS Setup](#cd-rom-bios-setup) below)
   - A board with PSRAM is required
4. Insert the SD card into the device
5. Optionally include [metadata files](#using-metadata) for box art and game information
6. Use the menu to browse, select, and play games

***

## Using Metadata

> [!NOTE]
> Metadata support will be available in an upcoming release.

The emulator can display box art and game information in the menu when metadata files are present on the SD card. When a ROM is selected in the menu, press **START** to view its metadata. The screensaver will also display random box art when metadata is available.

### Installation

Download the PCE metadata pack from the [releases page](https://github.com/fhoedemakers/pico-pcePlus/releases) and extract its contents to the root of your SD card. The metadata should be placed in the `/metadata/PCE/` folder with the following structure:

```
SD Card Root
└── metadata/
    └── PCE/
        ├── images/
        │   └── 160/
        │       ├── 0/
        │       │   ├── <CRC32>.444    (DVI boards)
        │       │   └── <CRC32>.555    (HSTX boards)
        │       ├── 1/
        │       ...
        └── descr/
            ├── 0/
            │   └── <CRC32>.txt
            ├── 1/
            ...
```

- **Images** are box art files named by the CRC32 of the ROM, stored as `.444` (for DVI/PicoDVI boards) or `.555` (for HSTX boards).
- **Descriptions** are text files named by the CRC32 of the ROM, containing game information.
- Files are organized into subdirectories by the first hex digit of the CRC32 hash (`0`–`f`).

The CRC32 of a loaded ROM is shown in the emulator's on-screen display and can be used to verify which metadata file corresponds to a game.

***

## CD-ROM BIOS Setup

CD-ROM² games require a System Card BIOS to boot. You must provide your own BIOS file — it is not included with the emulator.

### Installation

1. Create a `/bios/` folder in the root of your SD card
2. Place one or more BIOS files (with a `.pce` extension, or named `cd_bios.rom`) in the `/bios/` folder
3. The emulator will automatically scan this folder, identify the BIOS by its CRC32 checksum, and select the best available one

### Per-game BIOS override

The emulator also looks for a BIOS in the **same folder as the CUE/BIN or CHD file** of the game you are launching. If a `.pce` file (or a file named `cd_bios.rom`) is found next to the disc image, it is loaded **instead of** the BIOS in `/bios/`. The `/bios/` folder is only used as a fallback when the disc folder contains no usable BIOS.

This is useful for:

- Running a specific region's BIOS for a single game without changing your default
- Shipping a game patch that depends on a particular System Card version
- Keeping unmodified game folders self-contained on the SD card

CRC32 detection still applies in the per-game folder, so the variant (System Card 1.x/2.x/3.0, Arcade Card Duo/Pro, Games Express, US/JP region) is selected automatically.

### Supported BIOS Files

The emulator recognizes the following BIOS files by CRC32:

| CRC32 | Description | Priority |
|--------|-------------|----------|
| `0x1F240E6E` | Arcade Card Pro (JP) | Highest |
| `0x8C4588E2` | Arcade Card Duo (JP) | High |
| `0x6D9A73EF` | Super CD-ROM System (JP, v3.0) | Medium-High |
| `0x2B5B75FE` | TG-CD Super System Card (US) | Medium-High |
| `0x51A12D90` | Games Express CD Card (JP) | Medium |
| `0x9D1E81B8` | Games Express CD Card (JP, Alt) | Medium |
| `0x283B74E0` | CD-ROM System (JP, v2.1) | Medium-Low |
| `0x52520BC6` | CD-ROM System (JP, v2.0) | Medium-Low |
| `0xFF2A5EC3` | TurboGrafx CD System (US, v2.0) | Medium-Low |
| `0x3F9F95A4` | CD-ROM System (JP, v1.0) | Low |

> [!NOTE]
> - The **Super CD-ROM System v3.0** or **Arcade Card Pro** BIOS is recommended for maximum game compatibility.
> - If multiple BIOS files are found, the one with the highest priority is automatically selected.
> - US-region BIOS files will run the emulator in US/TurboGrafx mode; JP BIOS files run in PC Engine mode.
> - If the emulator does not recognize your BIOS CRC, it will still attempt to use the file as a fallback, but compatibility may vary.
> - Without a BIOS file in `/bios/`, CD-ROM² games will fail to load with the message "No BIOS in /bios/".

***

## Possible Configurations

You can use it with these RP2350 boards and configurations:

- **Raspberry Pi Pico 2** with a breadboard setup:
  - [Adafruit DVI Breakout For HDMI Source Devices](https://www.adafruit.com/product/4984)
  - [Adafruit Micro-SD breakout board+](https://www.adafruit.com/product/254)

- **[Waveshare RP2350-PiZero Development Board](https://www.waveshare.com/rp2350-pizero.htm)** – Supports optional PSRAM. When installed, CD-ROM² games are supported.

- **[Adafruit Metro RP2350](https://www.adafruit.com/product/6003)** – Supports optional PSRAM. When installed, CD-ROM² games are supported.

- **[Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107)** – Built-in PSRAM, full CD-ROM² support.

- **[Adafruit Fruit Jam](https://www.adafruit.com/product/6200)** – Built-in PSRAM, no additional hardware required apart from a USB gamepad. Full CD-ROM² support.

For detailed setup instructions, refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup) as the hardware configurations are identical.

***

## Gamecontroller Support

Depending on the hardware configuration, the emulator supports these game controllers:

### USB Game Controllers

- Sony DualShock 4
- Sony DualSense
- Genesis Mini 1 and 2
- [Retro-bit 8 button Genesis-USB](https://www.retro-bit.com/controllers/genesis/#usb)
- PlayStation Classic
- Keyboard
- XInput controllers (Xbox 360, Xbox One, 8bitDo, and other XInput-compatible controllers)

### Legacy Controllers

- NES / SNES controllers (directly wired)
- Wii Classic Controller (via I2C)

***

## In-Game Hotkeys

The emulator reacts to button combinations on controller 1 during gameplay. The combinations use the PC Engine controller's **SELECT** and **RUN** buttons together with the directional pad or action buttons.

### SELECT + …

| Combo | Action |
|---|---|
| SELECT + RUN | Open the in-game settings menu |
| SELECT + UP / DOWN | Cycle screen mode (1:1 ↔ 8:7, scanlines on/off) on DVI boards; toggle scanlines on HSTX boards |
| SELECT + LEFT | Toggle external (I²S) audio output (DVI boards only) |
| SELECT + RIGHT | Toggle the VU meter LEDs (boards that support it) |

### RUN + …

| Combo | Action |
|---|---|
| RUN + A | Toggle the on-screen FPS overlay |
| RUN + B | Start recording in-game audio to a WAV file on the SD card (requires PSRAM) |
| RUN + UP | Quick-load a save state |
| RUN + DOWN | Quick-save a save state |
| RUN + LEFT / RIGHT | Adjust output volume (Adafruit Fruit Jam only) |

***

## Save States and Backup RAM

- **Save states** – Manual save/load slots are available for any game through the in-game menu or via the **RUN + UP/DOWN** hotkeys. Save state files are stored on the SD card under `/savestates/PCE/<CRC32>/`.
- **Auto-save state** – When enabled in the settings menu, the emulator automatically saves a state when you exit a game and offers to resume it the next time the same ROM is launched.
- **CD-ROM² Backup RAM (BRAM)** – CD-ROM² games that use the System Card's BRAM (e.g. to save progress) get their BRAM persisted automatically. The file is written to `/savestates/PCE/<CRC32>/bram.sav` when the game exits, and reloaded the next time the same disc image is launched.

***

## Audio Recording

On boards with PSRAM, the emulator can record the live audio output to a `.wav` file on the SD card. Press **RUN + B** during gameplay to start a recording; the file is finalised when the recording is stopped or the game exits. This is useful for capturing soundtracks, BGM, or evidence of an emulation issue.

***

## Performance and CD-ROM² Frame Pacing

- The emulator targets a steady **60 fps** for HuCard games.
- For CD-ROM² games, an **adaptive frame-skip** kicks in when the SoC cannot finish a frame in time (typically during heavy CD streaming). The skip is limited so that video never drops below **30 fps**, and the emulator never skips two frames in a row, keeping motion smooth even on busy scenes.
- HSTX boards run the video output in a streamed mode that frees more time for CD audio and ADPCM mixing — this is one of the reasons HSTX is preferred for CD-ROM² and SuperGrafx titles.

***

## Building from Source

### Prerequisites

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (with `PICO_SDK_PATH` environment variable set)
- CMake (3.13 or later)
- ARM GCC toolchain
- For RISC-V builds: RISC-V toolchain installed in `$PICO_SDK_PATH/toolchain/RISCV_RPI_2_0_0_2/bin`
- `picotool` installed and available in PATH

### Clone the Repository

```bash
git clone --recurse-submodules https://github.com/fhoedemakers/pico-pcePlus.git
cd pico-pcePlus
git checkout pce
```

### Building

Use the build script to compile the firmware:

```bash
# Build for Pico 2 (ARM) with a specific hardware configuration
./bld.sh -c 1 -2

# Build for Pico 2 (RISC-V)
./bld.sh -c 1 -r -t $PICO_SDK_PATH/toolchain/RISCV_RPI_2_0_0_2/bin

# Build for Pico 2 W
./bld.sh -c 2 -2 -w

# Build with PIO USB support (second USB port for controllers)
./bld.sh -c 2 -2 -u
```

### Build All Configurations

To build all supported configurations and place UF2 binaries in the `releases` folder:

```bash
./buildAll.sh
```

### Hardware Configurations

| Config | Board |
|--------|-------|
| 1 | Breadboard with Pico 2 |
| 2 | Breadboard with Pico 2 (active-low SD CS) |
| 5 | Adafruit Metro RP2350 |
| 6 | Waveshare RP2350-PiZero |
| 7 | Pimoroni Pico Plus 2 (breadboard) |
| 8 | Pimoroni Pico DV Demo Base |
| 9 | Pimoroni Pico Plus 2 (Pico DV Demo Base) |
| 10 | SpotPear HDMI |
| 12 | Adafruit Fruit Jam |
| 13 | Murmulator |

***

## Flashing the Firmware

1. Hold the BOOTSEL button on your Pico board while connecting it to your computer via USB
2. The board will appear as a USB mass storage device
3. Copy the appropriate `.uf2` file to the device
4. The board will automatically reboot and start the emulator

***

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](LICENSE) file for details.

## Credits

- [pce-go](https://github.com/ducalex/retro-go) – PC Engine emulation core by ducalex
- [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) – Video/audio framework and shared infrastructure
- [pico_lib](https://github.com/shuichitakano/pico_lib) – Audio support for PicoDVI by [Shuichi Takano](https://github.com/shuichitakano)
- [pico_hdmi](https://github.com/fliperama86/pico_hdmi) – HDMI library by [fliperama86](https://github.com/fliperama86)
- [PicoDVI](https://github.com/Wren6991/PicoDVI) – DVI output library
- [TinyUSB](https://github.com/hathach/tinyusb) – USB host support
