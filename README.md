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
- **SuperGrafx (SGX)** — SuperGrafx titles are recognised and dispatched to the second VDC. SuperGrafx support requires PSRAM, and the board must be overclocked to 378 MHz to reach the correct framerate and audio. Overclocking to 378 MHz is only possible on configurations that use HSTX for video, such as the Adafruit Fruit Jam. PicoDVI configurations like the Waveshare RP2350 PiZero need an exact system clock of 252 MHz to produce 60 fps, which leaves no headroom to overclock for SuperGrafx. On those boards SuperGrafx games run at a reduced framerate and without audio.
- **CD-ROM²** — CD-ROM² and Super CD-ROM² games are supported, including CD-DA audio playback and ADPCM streaming. CD-ROM² playback requires PSRAM and a System Card BIOS supplied by the user.
- **Audio CDs** — Regular music CDs can be played with the CD player built into the System Card BIOS. Put a CUE/BIN or CHD image of a music CD on the SD card and start it like a CD game: the track list appears and playback, track skip, pause and the level meters all work.
- **Save states** — Manual save and load slots are available through the in-game menu. An optional auto-save mode stores a state when the game exits and offers to resume it the next time the same ROM is launched. State files are stored on the SD card under `/savestates/PCE/<CRC32>/`.
- **Backup RAM (BRAM)** — CD-ROM² games that use the System Card's BRAM (for in-game save data) have it persisted automatically alongside the save states.

| | |
|---|---|
| <img width="400" height="225" alt="Screenshot 2026-07-02 18-17-33" src="https://github.com/user-attachments/assets/3aaf8609-8a22-4ae2-a30c-a1245db6fa3a" /> | <img width="400" height="225" alt="Screenshot 2026-07-02 18-17-44" src="https://github.com/user-attachments/assets/bbe11c91-2d1c-44f4-a0e9-f58433475f68" /> |
| <img width="400" height="225" alt="Screenshot 2026-07-02 18-18-06" src="https://github.com/user-attachments/assets/2f5dfe78-1ea8-436e-95e6-ad5b10b90735" /> | <img width="400" height="225" alt="Screenshot 2026-07-02 18-19-17" src="https://github.com/user-attachments/assets/96554ffc-37d1-4015-b2ee-121e8e68d36f" /> |
| <img width="400" height="225" alt="Screenshot 2026-07-02 18-22-44" src="https://github.com/user-attachments/assets/e6fcf7a5-3775-474e-b86a-a0625ae8d9a3" /> | <img width="400" height="225" alt="Screenshot 2026-07-02 18-32-00" src="https://github.com/user-attachments/assets/240948c6-d0eb-48b1-a7b8-efcc7d5ab4f7" /> |



***

## CD-ROM² disc formats

Two disc-image formats are accepted:

- **CUE/BIN** — Uncompressed CD images. This is the recommended format.
- **CHD** — Compressed disc images (MAME's CHD format). CHD images are decompressed sector-by-sector at run time and are therefore generally slower than CUE/BIN. They are useful when SD card space is limited; otherwise CUE/BIN is preferred.

Each disc image (and its associated tracks for CUE/BIN) should be placed in its own folder on the SD card.

> **Note on SD card speed:** CD playback is sensitive to SD card speed, especially when using CHD images that must be decompressed in real time. v0.2 reduced the impact considerably, but a slow or aging card can still cause occasional stutter or audio glitches; a faster card resolves it.

### Setup Overview

1. Prepare an SD card formatted as FAT32 or exFAT
2. Transfer ROM files to the card, preferably in /roms/PCE and CD games in /roms/PCE/CD (subdirectory organization is supported).
3. Optionally include [metadata files](#using-metadata) for game information
4. Insert the SD card into the device
5. Use the menu to browse, select, and play games. Save data is automatically persisted to the SD card. The last 20 games that were started are kept in a [recently played list](#recently-played-games), one button press away in the menu.

***

## Hardware requirements

The emulator runs on RP2350-based boards in two configurations:

- **RP2350 without PSRAM** — Supports HuCard ROMs, save states, and metadata display.
- **RP2350 with PSRAM** — In addition to the above, enables SuperGrafx titles, CD-ROM² playback (CUE/BIN or CHD), Backup RAM persistence, and audio recording.

For board-by-board wiring, supported display modes, and which UF2 file to flash, refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup). The set of supported boards and their pinouts is identical between the two projects.

A board and its breakouts can also be built into a finished little console on one of the three [custom PCBs](#custom-pcbs).

***

## Custom PCBs

Three community PCB designs turn a supported board and its breakouts into a finished little console, each with an optional 3D-printed case. They are simply a neater way to build hardware this emulator already supports, so nothing changes in the firmware: flash the binary for that configuration and you are done.

| Design | Board it carries | Build | Gerber archive | Designed by |
| --- | --- | --- | --- | --- |
| [PicoNES](#picones-pcb) | Pico 2, Pico 2 W or Pimoroni Pico Plus 2 | `-c2` | `pico_nesPCB_v2.6.zip` | John Edgar Park |
| [PicoNES Mini](#picones-mini-pcb) | Waveshare RP2350-Zero | `-c6` | `Gerber_PicoNES_Mini_PCB_v2.0.zip` | Gavin Knight |
| [PicoNES Micro](#picones-micro-pcb) | Waveshare RP2350-USB-A | `-c9` | `Gerber_PicoNES_Micro_v1.2.zip` | Gavin Knight |

All three archives are attached to every [release](https://github.com/fhoedemakers/pico-pcePlus/releases/latest) of this project and also live in [pico_shared/PCB](https://github.com/fhoedemakers/pico_shared/tree/main/PCB). Upload the zip as-is to a PCB manufacturer of your choice; [PCBWay](https://www.pcbway.com/) and JLCPCB are both good options.

The designs come from [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) and kept their NES-flavoured names, but there is nothing NES-specific about them — they are DVI, microSD and controller wiring, and this emulator runs on them just as well. The PicoNES and PicoNES Mini both have two controller ports, so the second player of a two-player PC Engine game has a pad; the PicoNES Micro is a single-controller design.

> [!IMPORTANT]
> The PicoNES also fits an original RP2040 Pico, and the PicoNES Mini exists in an RP2040-Zero flavour. Neither is of use here — this emulator requires an RP2350.

Of the three boards, only the Pimoroni Pico Plus 2 on the PicoNES has PSRAM. That is what CD-ROM² games, SuperGrafx titles and audio recording need; on a Pico 2, a Waveshare RP2350-Zero or a Waveshare RP2350-USB-A the emulator plays HuCard ROMs.

> [!NOTE]
> Sellers on AliExpress have copied the PicoNES design and sell ready-made boards. For questions about those, contact the seller.

### PicoNES PCB

The original design, by [@johnedgarpark](https://twitter.com/johnedgarpark). It carries the Pico, the DVI and microSD breakouts and up to two NES controller ports. It is also the only one of the three that takes an interchangeable Pico-format board, which is what makes a Pimoroni Pico Plus 2 — and with it PSRAM, CD-ROM² and SuperGrafx — an option. The current design is **v2.6**.

<img width="480" alt="Populated PCB with a Pico plugged into the through-holes" src="https://github.com/user-attachments/assets/2bbc846d-56b1-4528-9899-01bc9b32ce11" />

#### Mounting the board

Design v2.6 added through-holes, so there are now two ways to fit the board:

| Mounting | Boards | Design version |
| --- | --- | --- |
| Soldered flat onto the PCB, no headers | Pico 2, Pico 2 W | any |
| Male headers plugged into the through-holes | Pico 2, Pico 2 W, Pimoroni Pico Plus 2 | v2.6 or later |

> [!IMPORTANT]
> A [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) needs v2.6 **and** male headers. On v2.1 and older designs the board has to lie flat against the PCB, which the SP/CE connector on the back of the Pimoroni Pico Plus 2 prevents.

> [!NOTE]
> Soldering skills are required. Solder every connection from the Pico to the PCB, including the ones on the short right-hand side of the board — those are ground.

#### What you need

- One of the following, mounted as described above:
  * Raspberry Pi Pico 2 or Pico 2 W **without headers**, soldered flat.
  * Raspberry Pi Pico 2, Pico 2 W or [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) **with male headers** soldered on ([these](https://a.co/d/dSNPuyo) fit), plugged into the through-holes.
- [Adafruit DVI Breakout Board — For HDMI Source Devices](https://www.adafruit.com/product/4984)
- [Adafruit Micro SD SPI or SDIO Card Breakout Board — 3V ONLY!](https://www.adafruit.com/product/4682)
- For controllers on the GPIO ports:
  * [one or two NES controller ports](https://www.zedlabz.com/products/controller-connector-port-for-nintendo-nes-console-7-pin-90-degree-replacement-2-pack-black-zedlabz) — populate both to give player 2 a pad
  * NES or SNES controllers
- [Micro USB to OTG Y-cable](https://a.co/d/b9t11rl) if you want to use a USB game controller — it powers the board and connects the controller at the same time.
- Micro USB power supply.
- Optional: an on/off switch, such as [this one](https://www.kiwi-electronics.com/en/spdt-slide-switch-410?search=KW-2467).

> [!NOTE]
> A plain NES controller has no Button3, so it cannot open the [recently played list](#recently-played-games) directly — use the settings menu entry instead. The sockets speak the SNES protocol as well, so an SNES pad with a [SNES-to-NES adapter cable](https://nl.aliexpress.com/item/1005007923169070.html) — [or one you make yourself](http://www.neshq.com/hardmods/snes_to_nes_controller.txt) — is the better choice.

#### Which binary to flash

`picopcePlus_AdafruitDVISD_pico2_arm.uf2`, for the Pico 2, the Pico 2 W and the Pimoroni Pico Plus 2 alike. The Pimoroni Pico Plus 2 needs no separate build: the emulator reads the real flash size from the chip at boot and detects PSRAM at runtime, so the same image adapts to whichever board is plugged in.

#### 3D printed case

Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)) designed an NES-like enclosure for this PCB: [thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). The v2.0 design has a base, a power-switch part and a choice of two top covers — one with a button that reaches the BOOTSEL button so firmware can be updated without opening the case, one without. Print the files that match the PCB version you own; Gavin's Thingiverse page has the details.

> [!IMPORTANT]
> If the board is mounted with male headers, download the **latest** top cover. Headers raise the board, and only the newest cover leaves room for the USB cable — the older ones assume a Pico soldered flat onto the PCB.

<img width="480" alt="Top cover with a button for BOOTSEL" src="https://github.com/user-attachments/assets/3c8f8990-51b9-4873-9054-64bb2cd6c300" />

For the full photo gallery and assembly detail, see the [PCB section of the pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2).

### PicoNES Mini PCB

A smaller take on the same idea by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)), built around a Waveshare RP2350-Zero and two NES controller ports. It uses cheaper but considerably harder to solder parts, so it is a more advanced project than the PicoNES — if you are unsure of your soldering, start with that one instead. The current design is **v2.0** (`Gerber_PicoNES_Mini_PCB_v2.0.zip`), which improved the SD slot and the components around the HDMI port.

Flash `picopcePlus_WaveShareRP2350ZeroWithPCB_arm.uf2`. The RP2350-Zero has no PSRAM, so this build plays HuCard ROMs.

> [!NOTE]
> Good soldering skills are required, especially around the HDMI portion: plenty of flux, a fine tip and solder wick. The recommended order is the resistor arrays first, then the HDMI port, then the Pico or the microSD adaptor, and the NES ports last — they can be hard to push into the PCB.

The build guide and the full component list are on Instructables: <https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/>

<img width="480" alt="Soldered PicoNES Mini PCB" src="https://github.com/user-attachments/assets/13933b1d-af00-402e-a0a0-8456de4a82da" />

#### 3D printed case for the Mini

Also by Gavin Knight: [thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536). The same page still carries the older v1.0 PCB design files, gerber and BOM. Without a printer of your own, a local printing service or a professional one such as PCBWay or JLCPCB will produce it — the professional finishes are excellent.

<img width="480" alt="PicoNES Mini in its 3D-printed case" src="https://github.com/user-attachments/assets/732384bd-062d-43ca-97cb-a16a39607c41" />

### PicoNES Micro PCB

The smallest of the three, again by Gavin Knight: a Waveshare RP2350-USB-A board on a PCB barely larger than the USB port itself, with a single player controlling the console over USB. The current design is **v1.2** (`Gerber_PicoNES_Micro_v1.2.zip`).

Flash `picopcePlus_WaveShare2350USBA_arm_piousb.uf2`. The game controller plugs into the USB-A port; the USB-C port is for power and for flashing the firmware. The RP2350-USB-A has no PSRAM, so this build plays HuCard ROMs, and the driver recognises only one USB controller, so two-player games cannot be played on it.

> [!NOTE]
> Because of the size, micro-soldering skills are required — the design uses 0603 SMD components. This is the most demanding of the three builds.

The build guide is on Instructables: <https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/>

<img width="480" alt="PicoNES Micro populated PCB, NES controller shown for scale" src="https://github.com/user-attachments/assets/59c8a31b-dc3e-47b0-8ffb-89e1eab2a75b" />

<img width="480" alt="PicoNES Micro in its 3D-printed case" src="https://github.com/user-attachments/assets/1d6051f2-1393-40e1-aad0-e39ffb7717a0" />

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
<img width="400" height="225" alt="Screenshot 2026-07-02 18-25-51" src="https://github.com/user-attachments/assets/685485d8-d882-4ec9-aaff-76de7e95920a" />

***

## Controls

The emulator presents the PC Engine controller mapping on any connected USB controller, NES/SNES controller (directly wired), or Wii Classic Controller (via I²C). Supported USB devices include Sony DualShock 4 / DualSense, Sega Genesis Mini 1 and 2, Retro-bit 8-button Genesis-USB, PlayStation Classic, USB keyboards, and XInput-compatible controllers (Xbox 360, Xbox One, 8BitDo, and similar).

The PC Engine controller has a D-pad, two action buttons (**I** and **II**), and two system buttons (**SELECT** and **RUN**).

During gameplay, **SELECT + RUN** opens the in-game settings menu. All other in-game adjustments — screen mode, scanlines, FPS overlay, VU meter, external (I²S) audio output, volume, save and restore state, rapid-fire, audio recording, and so on — are reached from this menu.

In the file browser, Button2 opens a folder or starts a game, Button1 goes back to the parent folder, START shows metadata and box art, SELECT opens the settings menu, and Button3 opens the [recently played list](#recently-played-games). Button3 is X on a SNES or Wii Classic pad, Y on XInput, Triangle on PlayStation and C on Genesis; a plain NES controller has no Button3.

Two-player games are supported. Without a USB controller, the two GPIO controller ports are player 1 and player 2. With a USB controller connected, that pad becomes player 1 and either GPIO port (or a Wii Classic Controller) drives player 2.

***

## Recently played games

The menu keeps a list of the **last 20 games that were started**, most recent first. Open it with **Button3** in the file browser, or with the **Recently played** entry at the top of the settings menu. That entry is only present when the settings menu is opened from the file browser, not from inside a running game — which is also the route for pads without a Button3.

In the list:

| Button | Action |
| ------ | ------ |
| UP/DOWN | Select a game. |
| Button2 | Start the highlighted game. |
| Button1 | Close the list and return to the file browser. |
| SELECT | Remove the highlighted game from the list. Asks for confirmation first. This only removes the entry; the file on the SD card is left alone. |
| START | Show [metadata](#metadata) and box art (when available). |

Games are added automatically when they are started, so nothing has to be enabled. HuCard, SuperGrafx and CD games all appear in the list; starting a game that is already in it moves it back to the top. The list closes by itself after a minute without input.

The list is kept in **`/recent_PCE.txt`** in the root of the SD card, as plain text with one game per line. It survives a reboot and can be read, edited or deleted on a PC. Deleting the file simply empties the list, and a damaged file is treated as an empty list — unlike the settings file, nothing else is reset. Each emulator running under [pico-bootLoader](https://github.com/fhoedemakers/pico-bootLoader) keeps its own list.

If a game was moved, renamed or deleted on the SD card in the meantime, the list says so instead of starting it. Use SELECT to remove such an entry.

On boards **without** PSRAM, one entry can be tagged **[READY]**. That is the game whose ROM is currently written to flash, which is the one that starts without waiting for the flashing step. As of v0.4 a ROM that is already in flash is not written again, so restarting that game is immediate.

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
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) — software USB host implementation on PIO, by [sekigon-gonnoc](https://github.com/sekigon-gonnoc), used to expose a second USB port for controllers.
- [tusb_XInput](https://github.com/Ryzee119/tusb_XInput) — XInput controller driver by [Ryzee119](https://github.com/Ryzee119).
- NES gamepad and Wii Classic Controller support contributed by [PaintYourDragon](https://github.com/PaintYourDragon) and [Adafruit](https://github.com/adafruit).

### Hardware and assets

- PCB design by [John Edgar Park](https://twitter.com/johnedgarpark).
- Additional PCB design and 3D-printable cases (for both PCBs and the WaveShare RP2040/RP2350-PiZero) by [Gavin Knight](https://github.com/DynaMight1124).
- Metadata files provided by [Gavin Knight](https://github.com/DynaMight1124), based on [Ducalex's retro-go-covers](https://github.com/ducalex/retro-go-covers).

### Contributions and assistance

- [Anthropic Claude (Opus 4.6 and 4.7)](https://www.anthropic.com/claude/opus) assisted with the PSG rewrite, the ADPCM decoder port, CHD integration, SuperGrafx work, and general bug fixes.
