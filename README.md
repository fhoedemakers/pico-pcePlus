# pico-pcePlus

## Introduction

**pico-pcePlus** is a PC Engine / TurboGrafx-16 emulator for Raspberry Pi Pico, Pico 2, and other RP2040/RP2350-based microcontrollers. It provides PC Engine emulation with SD card support, an integrated menu system, and HDMI video output, enabling users to build compact, affordable retro gaming systems.

This project is based on [pce-go](https://github.com/ducalex/retro-go) from retro-go by ducalex, using the video/audio framework from [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

### Features

- **PC Engine / TurboGrafx-16 Emulation** – Execute HuCard ROM files (`.pce`) directly from an SD card
- **CD-ROM² Support** – Play CD-ROM² games using CUE/BIN disc images with CD-DA audio and ADPCM playback
- **SD Card Menu System** – Browse and launch games from an on-screen menu interface
- **Dual Controller Support** – Two simultaneous controllers for multiplayer gameplay
- **Save State Management** – Save and restore game states
- **Audio** – PSG sound with per-scanline generation, CD-DA audio mixing, and ADPCM decoding
- **Multiple Display Modes** – DVI (PicoDVI) and HSTX output with configurable screen modes
- **Flexible Hardware** – Compatible with standard DVI/HDMI breakout boards and various RP2040/RP2350 boards

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
3. For CD-ROM² games, place CUE/BIN disc image files on the SD card
4. Insert the SD card into the device
5. Use the menu to browse, select, and play games

***

## Possible Configurations

You can use it with these RP2040/RP2350 boards and configurations:

- **Raspberry Pi Pico / Pico 2** with a breadboard setup:
  - [Adafruit DVI Breakout For HDMI Source Devices](https://www.adafruit.com/product/4984)
  - [Adafruit Micro-SD breakout board+](https://www.adafruit.com/product/254)

- **[Adafruit Feather RP2040 with DVI](https://www.adafruit.com/product/5710)** (HDMI Output Port)

- **[Waveshare RP2040-PiZero Development Board](https://www.waveshare.com/rp2040-pizero.htm)**

- **[Waveshare RP2350-PiZero Development Board](https://www.waveshare.com/rp2350-pizero.htm)** – Supports optional PSRAM

- **[Adafruit Metro RP2350](https://www.adafruit.com/product/6003)** – Supports optional PSRAM

- **[Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107)** – Built-in PSRAM

- **[Adafruit Fruit Jam](https://www.adafruit.com/product/6200)** – No additional hardware required apart from a USB gamepad

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
# Build for a specific hardware configuration (e.g., config 1 = breadboard with Pico)
./bld.sh -c 1

# Build for Pico 2 (ARM)
./bld.sh -c 1 -2

# Build for Pico 2 (RISC-V)
./bld.sh -c 1 -r -t $PICO_SDK_PATH/toolchain/RISCV_RPI_2_0_0_2/bin

# Build for Pico W
./bld.sh -c 2 -w

# Build with PIO USB support (RP2350 only)
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
| 1 | Breadboard with Pico / Pico 2 |
| 2 | Breadboard with Pico / Pico 2 (active-low active SD CS) |
| 3 | Adafruit Feather RP2040 DVI |
| 4 | Waveshare RP2040-PiZero |
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
- [PicoDVI](https://github.com/Wren6991/PicoDVI) – DVI output library
- [TinyUSB](https://github.com/hathach/tinyusb) – USB host support
