# CHANGELOG

Recently played list, no re-flash of a rom already in flash, SNES controller support on the GPIO port, and PicoNES PCB design v2.6.

# General Info

[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

[See the hardware section in the readme for how to install and wire up](https://github.com/fhoedemakers/pico-pcePlus#hardware-requirements)

> [!IMPORTANT]
> An **RP2350** board is required. The original RP2040 (Pico 1) is not supported.

# v0.4

## New

- **Recently played list.** The last 20 games that were started are kept, newest first. It is opened with Button3 in the rom browser, or with the **Recently played** entry at the top of the settings menu. That entry is present only when the settings menu is opened from the rom browser, which is also the route for pads without a Button3.
	- Button2 starts the selected game, SELECT removes it from the list, START shows its artwork, Button1 closes the list.
	- HuCard, SuperGrafx and CD games are all listed.
	- The list is plain text in `/recent_PCE.txt` in the root of the SD card. A game that is no longer present is reported as missing when it is started. An unreadable list is treated as empty; no other settings are affected.
	- See [Recently played games](https://github.com/fhoedemakers/pico-pcePlus#recently-played-games).
- **Roms already present in flash are no longer re-flashed** (boards without PSRAM), so restarting the last played game no longer waits for the flashing step. The image is verified before the write is skipped. That game is marked `[READY]` in the recently played list.
- **SNES controllers on the GPIO controller port**: all 12 buttons are read. In the menu A chooses, B goes back and X opens the recently played list. Previously a SNES pad was read as a NES pad and these buttons were mapped incorrectly. NES pads are unaffected.
- **PicoNES PCB design v2.6** replaces v2.1 in the release assets: through-holes for mounting a Pico 2, Pico 2 W or Pimoroni Pico Plus 2 on male headers, and corrected D3/D4 silkscreen labels on controller port 2. No firmware change is required for either revision. See [Custom PCBs](https://github.com/fhoedemakers/pico-pcePlus#custom-pcbs).

## Fixes

- Controller Test reports the detected pad type, names the buttons accordingly and shows the raw word sent by a pad on the GPIO port. Buttons of a NES pad were previously labelled in SNES order.
- Reset to defaults no longer carries over the obsolete standalone scanline flag from the loaded settings file.

# v0.3

- **Overclock on/off toggle** in the settings menu (HSTX boards only). The menu will reboot after the change is saved. Some HuCard games run better with a higher overclock.
- The in-game settings menu now scrolls when there are more entries than fit on screen.

## Fixes

- Small splash-screen fix.


# v0.2

## New

- You can now play audio CDs with the CD player built into the System Card BIOS. Put a `.cue`/`.bin` or `.chd` image of a music CD on the SD card and start it like a CD game: the track list appears and playback, track skip, pause and the level meters all work.

## Fixes

- Davis Cup Tennis no longer goes to a black screen after the match starts.
- Battle Royale no longer crashes shortly after startup.
- Cadash: in-game dialog boxes now render correctly. Some flicker on the dialog screen remains; see [#9](https://github.com/fhoedemakers/pico-pcePlus/issues/9).
- SuperGrafx graphics fixes in:
	- Ghouls 'n Ghosts
	- Aldynes
	- Darius Plus
	- Madou King Granzort
- Fixed screen tearing in CHD-based CD games.
- Reduced audio crackle in CD games on some board configurations. See [#11](https://github.com/fhoedemakers/pico-pcePlus/issues/11).
- Game speed now matches the real PC Engine refresh rate (59.826 Hz). Previously the emulator ran slightly too fast, which made some titles play noticeably quicker than on original hardware.
- The FPS overlay now reports a one-second running average instead of a noisy per-frame value.

## For developers

- Added a Linux host test harness (`hosttest/`) that runs the emulator core on a PC for faster debugging — no Pico or flashing needed. See `hosttest/README.md`.
- The host test harness now accepts arbitrary controller input via the new `PCE_PRESS_KEYS=<frame>:<hexmask>[,…]` env var, in addition to the existing `PCE_PRESS_RUN` and `PCE_HOLD_FIRE`.
- The host test harness can now load CD games (CUE/BIN and CHD), not just HuCard ROMs.
- New `PCE_QUIRK` env var on the host test harness lets you force per-game compatibility quirks on or off when reproducing issues.


# v0.1

First public release. There will be bugs. Please register an issue when you encounter one.

## Features

**HuCard / TurboGrafx-16**

- Standard PC Engine / TurboGrafx-16 cartridge ROMs (`.pce`) are loaded directly from the SD card.

**SuperGrafx (.sgx)**

- SuperGrafx titles are recognised and dispatched to the second VDC.
- Requires an RP2350 board with PSRAM.
- VPC sprite-priority modes 1 and 2 are implemented, with VDC1/VDC2 sprite layering and VDC2 scroll synchronization. Per-scanline background scroll-Y follows Mesen2's IncScrollY model so vertical parallax tracks correctly through the frame.
- Still under development: some titles exhibit residual graphical glitches (for example per-frame platform flicker in Ghouls 'n Ghosts).
- On HSTX-based boards the CPU is overclocked to 378 MHz to sustain 60 fps. At this clock the TinyUSB host stack is disabled; USB controllers continue to work through Pico-PIO-USB.
- On PicoDVI-based boards the CPU remains at 252 MHz; SuperGrafx titles fall back to frameskip and audio is does not work.

Most .sgx games still have graphical glitches. This game has severe graphical glitches:

- Madou King Granzort (Japan)

**CD-ROM² / Super CD-ROM²**

- CD-ROM² and Super CD-ROM² games are supported.
- Two disc-image formats accepted:
	- **CUE/BIN** — uncompressed (recommended).
	- **CHD** — MAME's compressed disc-image format, decompressed sector-by-sector at run time. Generally slower than CUE/BIN; useful when SD card space is limited.
- CD-DA audio playback and ADPCM streaming.
- Requires an RP2350 board with PSRAM and a user-supplied System Card BIOS at `/bios/` (or alongside the disc image). 
- BIOS files must use the `.pce` extension or be named `cd_bios.rom`. They are identified by CRC32, and when several are present the most capable variant is selected automatically. For broadest compatibility, **Super CD-ROM System v3.0 (JP)** or **Arcade Card Pro (JP)** is recommended.
- CD playback (most notably CHD) is sensitive to SD card speed. Slow or aging cards can cause intermittent horizontal black-line flicker; a faster card resolves it.

**Save states**

- Manual save and load slots through the in-game menu.
- Optional auto-save mode stores a state when the game exits and offers to resume it the next time the same ROM is launched.
- State files are stored on the SD card under `/savestates/PCE/<CRC32>/`.
- SuperGrafx state is preserved: VDC2 registers, VRAM2, SPRAM2, VPC, and extended WRAM are appended to the state file alongside the VDC1 block.

**Backup RAM (BRAM)**

- CD-ROM² games that use the System Card's BRAM (for in-game save data) have it persisted automatically alongside the save states.

**Audio**

- PSG emulation reworked against the Mesen2 reference model; addresses previously missing sound effects.
- Per-scanline audio generation.
- Optional I²S external audio output (PCM5000A / TLV320DAC3100).
- Audio recording on PSRAM boards.

**Display**

- HDMI output via HSTX (RP2350) and PicoDVI drivers.
- 8:7 and 1:1 screen modes, scanline effects, FPS overlay, VU meter.

**Controllers**

- USB controllers including Sony DualShock 4 / DualSense, 8BitDo, XInput (Xbox 360 / One), Sega Genesis Mini 1 and 2, Retro-bit 8-button Genesis-USB, PlayStation Classic, and USB keyboards.
- Directly wired NES / SNES gamepads.
- Wii Classic Controller over I²C.

**Metadata**

- Box art and short text descriptions are displayed in the file browser when a metadata pack is installed under `/metadata/PCE/`. The screensaver shows random box art.

## Use of AI

The PSG rewrite, ADPCM decoder port, CHD integration, SuperGrafx work, and general bug fixes were developed with the help of [Anthropic Claude (Opus 4.6 and 4.7)](https://www.anthropic.com/claude/opus).


<a name="downloads___"></a>
## Downloads by configuration

Binaries for each configuration are listed below. Only RP2350-based boards are supported; no RP2040 / Pico 1 binaries are provided.

For board-by-board wiring, supported display modes, and which UF2 file to flash, refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup). The set of supported boards and their pinouts is identical between the two projects.

### Standalone boards

| Board | Binary |
|:--|:--|
| Adafruit Metro RP2350 | [picopcePlus_AdafruitMetroRP2350_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitMetroRP2350_arm.uf2) |
| Adafruit Fruit Jam | [picopcePlus_AdafruitFruitJam_arm_piousb.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitFruitJam_arm_piousb.uf2) |
| Adafruit Feather RP2350 with TLV320DAC3100 | [picopcePlus_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2) |
| Waveshare RP2350-PiZero | [picopcePlus_WaveShareRP2350PiZero_arm_piousb.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_WaveShareRP2350PiZero_arm_piousb.uf2) |

### Breadboard

| Board | Binary |
|:--|:--|
| Pico 2 | [picopcePlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picopcePlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitDVISD_pico2_arm.uf2) |

### PicoNES PCB (PCB required)

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picopcePlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picopcePlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_AdafruitDVISD_pico2_arm.uf2) |

PCB: [pico_nesPCB_v2.6.zip](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/pico_nesPCB_v2.6.zip) (new in this release, replaces v2.1). [Readme](https://github.com/fhoedemakers/pico-pcePlus#picones-pcb)

3D-printed case: [thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). When the board is fitted on male headers, use the latest top cover; the older covers assume a Pico soldered flat and leave no room for the USB cable.

### PicoNES Mini PCB, Waveshare RP2350-Zero (PCB required)

| Board | Binary |
|:--|:--|
| Waveshare RP2350-Zero | [picopcePlus_WaveShareRP2350ZeroWithPCB_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_WaveShareRP2350ZeroWithPCB_arm.uf2) |

PCB: [Gerber_PicoNES_Mini_PCB_v2.0.zip](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/Gerber_PicoNES_Mini_PCB_v2.0.zip). [Readme](https://github.com/fhoedemakers/pico-pcePlus#picones-mini-pcb)

3D-printed case: [thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536)

### PicoNES Micro PCB, Waveshare RP2350-USBA (PCB required)

[Binary](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_WaveShare2350USBA_arm_piousb.uf2)

PCB: [Gerber_PicoNES_Micro_v1.2.zip](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/Gerber_PicoNES_Micro_v1.2.zip). [Readme](https://github.com/fhoedemakers/pico-pcePlus#picones-micro-pcb)

[Build guide](https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/)

### Pimoroni Pico DV

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picopcePlus_PimoroniDVI_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_PimoroniDVI_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picopcePlus_PimoroniDVI_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_PimoroniDVI_pico2_arm.uf2) |

### SpotPear HDMI

For more info about the SpotPear HDMI see https://spotpear.com/index/product/detail/id/1207.html and https://spotpear.com/index/study/detail/id/971.html.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picopcePlus_SpotpearHDMI_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picopcePlus_MurmulatorM1_pico2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picopcePlus_MurmulatorM2_arm.uf2](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/picopcePlus_MurmulatorM2_arm.uf2) |

### Other downloads

- Metadata: [PCEMetadata.zip](https://github.com/fhoedemakers/pico-pcePlus/releases/latest/download/PCEMetadata.zip)

Extract the zip file to the root folder of the SD card. Select a game in the menu and press START to show more information and box art. Works for most official released games. The screensaver shows floating random cover art.
