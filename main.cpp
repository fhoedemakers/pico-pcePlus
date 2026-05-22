/**
 * pico-pcePlus - PC Engine / TurboGrafx-16 emulator for Raspberry Pi Pico
 *
 * Based on pce-go from retro-go (https://github.com/ducalex/retro-go)
 * Using the video/audio framework from pico-infonesPlus
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/divider.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "util/work_meter.h"
#include "ff.h"
#include "tusb.h"
#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "mytypes.h"
#include "PicoPlusPsram.h"
#include "menu_settings.h"
#include "state.h"
#include "soundrecorder.h"

extern "C"
{
#include "pce-go.h"
#include "pce.h"
#include "psg.h"
}

bool isFatalError = false;
static FATFS fs;
char *romName;
bool showSettings = false;
bool loadSaveStateMenu = false;
SaveStateTypes quickSaveAction = SaveStateTypes::NONE;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;
static uint8_t framesbeforeAutoStateIsLoaded = 0;
static char fpsString[3] = "00";

#define AUDIOBUFFERSIZE 1024
#define PCE_AUDIO_RATE 44100
#define PCE_SAMPLES_PER_FRAME (PCE_AUDIO_RATE / 60)

#define EMULATOR_CLOCKFREQ_KHZ 252000
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

// PCE indexed framebuffer: pce-go renders 8-bit palette indices here.
// Renderer writes up to 16 bytes of scratch *before* and *after* the active
// region (see render_lines() in pce-go/gfx.c: framebuffer_top = screen_buffer
// - 16, plus sub-byte scroll_x shifts in draw_tiles). Pad with 16 bytes on
// each side and expose the offset pointer to keep those scratch writes from
// clobbering neighbouring BSS variables.
static uint8_t pce_framebuffer_storage[XBUF_WIDTH * XBUF_HEIGHT + 32];
static uint8_t *const pce_framebuffer = pce_framebuffer_storage + 16;

// Lookup table: 8-bit palette index -> RGB555 (HSTX) or RGB444 (DVI)
static uint16_t paletteLUT[256];

// Settings visibility for PCE
const int8_t g_settings_visibility_pce[MOPT_COUNT] = {
    0,                               // Exit Game
    0,                               // Reset Game
    0,                               // Save / Restore State
    1,                               // Screen Mode (1:1 / 8:7 x scanlines on/off)
    0,                               // Scanlines toggle (superseded by Screen Mode)
    HSTX,                            // Scanline type (HSTX only)
    1,                               // FPS Overlay
    0,                               // Audio Enable
    0,                               // Frame Skip
    (HSTX && ENABLEDVI),             // Display Mode HDMI or DVI
    (EXT_AUDIO_IS_ENABLED),          // External Audio
    1,                               // Font Color
    1,                               // Font Back Color
    ENABLE_VU_METER,                 // VU Meter
    (HW_CONFIG == 8),                // Fruit Jam Volume Control
    0,                               // DMG Palette (not applicable)
    0,                               // Border Mode (not applicable)
    0,                               // Rapid Fire on A
    0,                               // Rapid Fire on B
    0,                               // Auto insert FDS disk (not applicable)
    0,                               // Auto swap FDS disk (not applicable)
    1,                               // Enter bootsel mode
    0                                // FDS disk swap (not applicable)
};

const uint8_t g_available_screen_modes_pce[] = {
    1, // SCANLINE_8_7    (HSTX stretches the centred 256-px content; 320-px modes auto-fall-back to 1:1)
    1, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1  // NOSCANLINE_1_1
};

#define fpsfgcolor 0
#define fpsbgcolor 0xFFF

#define FPSSTART 0
#define FPSEND 8

static bool reset = false;
static bool resetGame = false;

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static uint16_t wiipad_raw_cached = 0;
#endif

// Build the palette lookup table mapping 8-bit PCE color indices to RGB
// PCE palette index encoding: GGG_RRR_BB (3-bit green, 3-bit red, 2-bit blue)
static void buildPaletteLUT()
{
    for (int i = 0; i < 256; i++)
    {
        int g3 = (i >> 5) & 7;
        int r3 = (i >> 2) & 7;
        int b2 = (i >> 0) & 3;
#if !HSTX
        // RGB444
        int r4 = (r3 << 1) | (r3 >> 2);
        int g4 = (g3 << 1) | (g3 >> 2);
        int b4 = (b2 << 2) | b2;
        paletteLUT[i] = (r4 << 8) | (g4 << 4) | b4;
#else
        // RGB555
        int r5 = (r3 << 2) | (r3 >> 1);
        int g5 = (g3 << 2) | (g3 >> 1);
        int b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        paletteLUT[i] = (r5 << 10) | (g5 << 5) | b5;
#endif
    }
}

static int16_t pce_audio_buffer[PCE_SAMPLES_PER_FRAME * 2]; // stereo interleaved

static inline void convertScanline(uint16_t *dst, int y, int y_offset, int screen_h,
                                   int x_offset, int screen_w)
{
    if (y >= y_offset && y < y_offset + screen_h)
    {
        uint8_t *src = &pce_framebuffer[(y - y_offset) * XBUF_WIDTH];
        if (x_offset > 0)
            memset(dst, 0, x_offset * sizeof(uint16_t));
        for (int x = 0; x < screen_w; x++)
            dst[x + x_offset] = paletteLUT[src[x]];
        if (x_offset + screen_w < 320)
            memset(dst + x_offset + screen_w, 0, (320 - x_offset - screen_w) * sizeof(uint16_t));
    }
    else
    {
        memset(dst, 0, 320 * sizeof(uint16_t));
    }
}

#define AUDIO_SAMPLES_PER_SCANLINE 4

static void __not_in_flash_func(convertAndDisplayFrame)()
{
    int screen_w = PCE.VDC.screen_width;
    int screen_h = PCE.VDC.screen_height;

    if (screen_w > 320) screen_w = 320;
    if (screen_h > 240) screen_h = 240;

    int x_offset = (320 - screen_w) / 2;
    int y_offset = (240 - screen_h) / 2;
    int audio_idx = 0;

#if HSTX
    // The HSTX 8:7 stretch reads a centred 252-px window and is geometrically
    // tuned for NES/PCE 256-px modes. For 320-px (or wider) PCE modes, that
    // window crops content. Auto-fall-back to 1:1 when not in a 256-px mode.
    static int last_screen_w = -1;
    if (screen_w != last_screen_w) {
        hstx_setAspectRatio87((scaleMode8_7_ && screen_w <= 256) ? 1 : 0);
        last_screen_w = screen_w;
    }
#endif

#if HSTX
#if EXT_AUDIO_IS_ENABLED
    const bool routeToExtAudio = settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected();
#endif
    for (int y = 0; y < 240; y++)
    {
        convertScanline(hstx_getlineFromFramebuffer(y), y, y_offset, screen_h, x_offset, screen_w);
        for (int a = 0; a < AUDIO_SAMPLES_PER_SCANLINE && audio_idx < PCE_SAMPLES_PER_FRAME; a++, audio_idx++)
        {
            int16_t l = pce_audio_buffer[audio_idx * 2];
            int16_t r = pce_audio_buffer[audio_idx * 2 + 1];
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
                addSampleToVUMeter(l);
#endif
#if EXT_AUDIO_IS_ENABLED
            if (routeToExtAudio)
                EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
            else
#endif
                hstx_push_audio_sample(l, r);
        }
    }
#elif !HSTX
#if FRAMEBUFFERISPOSSIBLE
    if (Frens::isFrameBufferUsed())
    {
        for (int y = 0; y < 240; y++)
        {
            convertScanline(&Frens::framebuffer[y * 320], y, y_offset, screen_h, x_offset, screen_w);
#if EXT_AUDIO_IS_ENABLED
            if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
            {
                for (int a = 0; a < AUDIO_SAMPLES_PER_SCANLINE && audio_idx < PCE_SAMPLES_PER_FRAME; a++, audio_idx++)
                    EXT_AUDIO_ENQUEUE_SAMPLE(pce_audio_buffer[audio_idx * 2], pce_audio_buffer[audio_idx * 2 + 1]);
            }
            else
#endif
            {
                auto &ring = dvi_->getAudioRingBuffer();
                int avail = std::min<int>(AUDIO_SAMPLES_PER_SCANLINE, PCE_SAMPLES_PER_FRAME - audio_idx);
                avail = std::min<int>(avail, ring.getWritableSize());
                if (avail > 0)
                {
                    auto p = ring.getWritePointer();
                    for (int a = 0; a < avail; a++, audio_idx++)
                        *p++ = {static_cast<short>(pce_audio_buffer[audio_idx * 2]),
                                static_cast<short>(pce_audio_buffer[audio_idx * 2 + 1])};
                    ring.advanceWritePointer(avail);
                }
            }
        }
    }
    else
    {
#endif
        for (int y = 0; y < 240; y++)
        {
            auto *b = dvi_->getLineBuffer();
            uint16_t *dst = b->data();
            convertScanline(dst, y, y_offset, screen_h, x_offset, screen_w);
            dvi_->setLineBuffer(y, b);
#if EXT_AUDIO_IS_ENABLED
            if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
            {
                for (int a = 0; a < AUDIO_SAMPLES_PER_SCANLINE && audio_idx < PCE_SAMPLES_PER_FRAME; a++, audio_idx++)
                    EXT_AUDIO_ENQUEUE_SAMPLE(pce_audio_buffer[audio_idx * 2], pce_audio_buffer[audio_idx * 2 + 1]);
            }
            else
#endif
            {
                auto &ring = dvi_->getAudioRingBuffer();
                int avail = std::min<int>(AUDIO_SAMPLES_PER_SCANLINE, PCE_SAMPLES_PER_FRAME - audio_idx);
                avail = std::min<int>(avail, ring.getWritableSize());
                if (avail > 0)
                {
                    auto p = ring.getWritePointer();
                    for (int a = 0; a < avail; a++, audio_idx++)
                        *p++ = {static_cast<short>(pce_audio_buffer[audio_idx * 2]),
                                static_cast<short>(pce_audio_buffer[audio_idx * 2 + 1])};
                    ring.advanceWritePointer(avail);
                }
            }
        }
#if FRAMEBUFFERISPOSSIBLE
    }
#endif
#endif

    // Display frame rate overlay
    if (settings.flags.displayFrameRate)
    {
        for (int line = FPSSTART; line < FPSEND; line++)
        {
            WORD *fpsBuffer = nullptr;
#if HSTX
            fpsBuffer = hstx_getlineFromFramebuffer(line) + 4;
#elif FRAMEBUFFERISPOSSIBLE
            if (Frens::isFrameBufferUsed())
                fpsBuffer = &Frens::framebuffer[line * 320] + 4;
#endif
            if (fpsBuffer)
            {
                int rowInChar = line % 8;
                for (int i = 0; i < 2; i++)
                {
                    char fontSlice = getcharslicefrom8x8font(fpsString[i], rowInChar);
                    for (int bit = 0; bit < 8; bit++)
                    {
                        if (fontSlice & 1)
                            *fpsBuffer++ = fpsfgcolor;
                        else
                            *fpsBuffer++ = fpsbgcolor;
                        fontSlice >>= 1;
                    }
                }
            }
        }
    }
}

// OSD callbacks required by pce-go
extern "C" uint8_t *osd_gfx_framebuffer(int width, int height)
{
    return pce_framebuffer;
}

extern "C" void osd_vsync(void)
{
    // Conversion is done explicitly in the main loop via convertAndDisplayFrame()
}

extern "C" void osd_input_read(uint8_t joypads[8])
{
    // Input is read explicitly in the main loop
}

static inline int ProcessAfterFrameIsRendered()
{
    Frens::pollHeadPhoneJack();
    Frens::PaceFrames60fps(false);
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    auto onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
    Frens::blinkLed(onOff);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();
    if (settings.flags.displayFrameRate)
    {
        uint32_t tick_us = Frens::time_us() - start_tick_us;
        fps = (1000000 - 1) / tick_us + 1;
        start_tick_us = Frens::time_us();
        fpsString[0] = '0' + (fps / 10);
        fpsString[1] = '0' + (fps % 10);
    }
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_raw_cached = wiipad_read();
#endif
#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        turnOffAllLeds();
    }
#endif
    if (showSettings)
    {
        showSettings = false;
        int rval = showSettingsMenu(true);
        if (rval == 3)
        {
            reset = true;
            if (isAutoSaveStateConfigured())
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::SAVE_AND_EXIT;
            }
        }
        if (rval == 4)
        {
            loadSaveStateMenu = true;
            quickSaveAction = SaveStateTypes::NONE;
        }
        if (rval == 5)
        {
            reset = resetGame = true;
        }
    }
    if (loadSaveStateMenu)
    {
        if (quickSaveAction == SaveStateTypes::LOAD_AND_START)
        {
            if (framesbeforeAutoStateIsLoaded > 0)
            {
                --framesbeforeAutoStateIsLoaded;
            }
        }
        else
        {
            framesbeforeAutoStateIsLoaded = 0;
        }
        if (framesbeforeAutoStateIsLoaded == 0)
        {
            char msg[24];
            snprintf(msg, sizeof(msg), "CRC %08X", Frens::getCrcOfLoadedRom());
            if (showSaveStateMenu(Emulator_SaveState, Emulator_LoadState, msg, quickSaveAction) == false)
            {
                reset = true;
            };
            loadSaveStateMenu = false;
        }
    }
    return count;
}

#define NESPAD_SELECT (0x04)
#define NESPAD_START (0x08)
#define NESPAD_UP (0x10)
#define NESPAD_DOWN (0x20)
#define NESPAD_LEFT (0x40)
#define NESPAD_RIGHT (0x80)
#define NESPAD_A (0x01)
#define NESPAD_B (0x02)

static DWORD prevButtons[2]{};
static DWORD prevButtonssystem[2]{};
static DWORD prevOtherButtons[2]{};

#define OTHER_BUTTON1 (0b1)
#define OTHER_BUTTON2 (0b10)

// PCE button definitions (from pce-go.h):
// JOY_A=0x01, JOY_B=0x02, JOY_SELECT=0x04, JOY_RUN=0x08
// JOY_UP=0x10, JOY_RIGHT=0x20, JOY_DOWN=0x40, JOY_LEFT=0x80

void readInputAndMapToPCE(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem, bool ignorepushed, char *gamepadType)
{
    *pdwPad1 = *pdwPad2 = *pdwSystem = 0;

    int pcesystem[2]{};
    unsigned long pushed, pushedsystem, pushedother;
    bool usbConnected = false;

    for (int i = 0; i < 2; i++)
    {
        int nespadbuttons = 0;
        auto &dst = (i == 0) ? *pdwPad1 : *pdwPad2;
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0)
        {
            usbConnected = gp.isConnected();
            if (gamepadType)
                strcpy(gamepadType, gp.GamePadName);
        }

        int pcebuttons = (gp.buttons & io::GamePadState::Button::LEFT ? JOY_LEFT : 0) |
                         (gp.buttons & io::GamePadState::Button::RIGHT ? JOY_RIGHT : 0) |
                         (gp.buttons & io::GamePadState::Button::UP ? JOY_UP : 0) |
                         (gp.buttons & io::GamePadState::Button::DOWN ? JOY_DOWN : 0) |
                         (gp.buttons & io::GamePadState::Button::A ? JOY_A : 0) |
                         (gp.buttons & io::GamePadState::Button::B ? JOY_B : 0) | 0;

        int otherButtons = (gp.buttons & io::GamePadState::Button::X ? OTHER_BUTTON1 : 0) |
                           (gp.buttons & io::GamePadState::Button::Y ? OTHER_BUTTON2 : 0) | 0;

        pcesystem[i] =
            (gp.buttons & io::GamePadState::Button::SELECT ? JOY_SELECT : 0) |
            (gp.buttons & io::GamePadState::Button::START ? JOY_RUN : 0) | 0;

#if NES_PIN_CLK != -1
        if (usbConnected)
        {
            if (i == 1)
                nespadbuttons = nespadbuttons | nespad_states[1] | nespad_states[0];
        }
        else
        {
            nespadbuttons |= nespad_states[i];
        }
#endif
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected)
        {
            if (i == 1)
                nespadbuttons |= wiipad_raw_cached;
        }
        else
        {
            if (i == 0)
                nespadbuttons |= wiipad_raw_cached;
        }
#endif
        if (nespadbuttons > 0)
        {
            pcebuttons |= ((nespadbuttons & NESPAD_UP ? JOY_UP : 0) |
                           (nespadbuttons & NESPAD_DOWN ? JOY_DOWN : 0) |
                           (nespadbuttons & NESPAD_LEFT ? JOY_LEFT : 0) |
                           (nespadbuttons & NESPAD_RIGHT ? JOY_RIGHT : 0) |
                           (nespadbuttons & NESPAD_A ? JOY_A : 0) |
                           (nespadbuttons & NESPAD_B ? JOY_B : 0) | 0);
            pcesystem[i] |= ((nespadbuttons & NESPAD_SELECT ? JOY_SELECT : 0) |
                             (nespadbuttons & NESPAD_START ? JOY_RUN : 0) | 0);
        }

        // Write combined buttons to PCE joypad registers
        PCE.Joypad.regs[i] = pcebuttons | pcesystem[i];

        auto p1 = pcesystem[i];
        if (ignorepushed == false)
        {
            pushed = pcebuttons & ~prevButtons[i];
            pushedsystem = pcesystem[i] & ~prevButtonssystem[i];
            pushedother = otherButtons & ~prevOtherButtons[i];
        }
        else
        {
            pushed = pcebuttons;
            pushedsystem = pcesystem[i];
            pushedother = otherButtons;
        }

        // SELECT + RUN -> settings menu
        if (p1 & JOY_SELECT)
        {
            if (pushedsystem & JOY_RUN)
            {
                FrensSettings::savesettings();
                showSettings = true;
            }
            else if (pushed & JOY_LEFT)
            {
#if EXT_AUDIO_IS_ENABLED && !HSTX
                settings.flags.useExtAudio = !settings.flags.useExtAudio;
#else
                settings.flags.useExtAudio = 0;
#endif
            }
            else if (pushed & JOY_UP)
            {
#if !HSTX
                scaleMode8_7_ = Frens::screenMode(-1);
#else
                Frens::toggleScanLines();
#endif
            }
            else if (pushed & JOY_DOWN)
            {
#if !HSTX
                scaleMode8_7_ = Frens::screenMode(+1);
#else
                Frens::toggleScanLines();
#endif
            }
#if ENABLE_VU_METER
            else if (pushed & JOY_RIGHT)
            {
                settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                turnOffAllLeds();
            }
#endif
        }
        // RUN + button combos
        if (p1 & JOY_RUN)
        {
            if (pushed & JOY_A)
            {
                settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
            }
            else if (pushed & JOY_B)
            {
#if PICO_RP2350
                if (Frens::isPsramEnabled() && !SoundRecorder::isRecording())
                    SoundRecorder::startRecording();
#endif
            }
            else if (pushed & JOY_UP)
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::LOAD;
            }
            else if (pushed & JOY_DOWN)
            {
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::SAVE;
            }
            else if (pushed & JOY_LEFT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::max(-63, settings.fruitjamVolumeLevel - 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
            else if (pushed & JOY_RIGHT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::min(23, settings.fruitjamVolumeLevel + 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
        }

        prevButtons[i] = pcebuttons;
        prevButtonssystem[i] = pcesystem[i];
        prevOtherButtons[i] = otherButtons;
        if (pushed)
            dst = pcebuttons;
        if (pushedother)
        {
            if (pushedother & OTHER_BUTTON1)
                printf("Other 1\n");
            if (pushedother & OTHER_BUTTON2)
                printf("Other 2\n");
        }
    }
    *pdwSystem = pcesystem[0] | pcesystem[1];
    if (reset)
    {
        // No SRAM to save for PCE (battery-backed RAM handled differently)
    }
}

void __not_in_flash_func(process)(void)
{
    DWORD pdwPad1, pdwPad2, pdwSystem;
    while (reset == false)
    {
        readInputAndMapToPCE(&pdwPad1, &pdwPad2, &pdwSystem, false, nullptr);
        pce_run();
        psg_update(pce_audio_buffer, PCE_SAMPLES_PER_FRAME, 0x3F);
        convertAndDisplayFrame();
        ProcessAfterFrameIsRendered();
    }
}

static char selectedRom[FF_MAX_LFN];

int main()
{
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    int fileSize = 0;

    Frens::setClocksAndStartStdio(CPUFreqKHz, VREG_VOLTAGE_1_20);

    printf("==========================================================================================\n");
    printf("Pico-PCE+ %s\n", SWVERSION);
    printf("Build date: %s\n", __DATE__);
    printf("Build time: %s\n", __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");
    printf("Starting up...\n");

    FrensSettings::initSettings(FrensSettings::emulators::PCE);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);

    buildPaletteLUT();

    bool showSplash = true;
    g_settings_visibility = g_settings_visibility_pce;
    g_available_screen_modes = g_available_screen_modes_pce;

    while (true)
    {
        if (strlen(selectedRom) == 0 || reset == true)
        {
            menu("Pico-PCE+", ErrorMessage, isFatalError, showSplash, ".pce", selectedRom);
            printf("Selected rom from menu: %s\n", selectedRom);
        }
        reset = false;
        fileSize = 0;

        if (Frens::isPsramEnabled())
        {
#if PICO_RP2350 && PSRAM_CS_PIN
            PicoPlusPsram &psram_ = PicoPlusPsram::getInstance();
            fileSize = psram_.GetSize((void *)ROM_FILE_ADDR);
            if (fileSize == 0)
            {
                printf("No rom loaded, continuing to menu\n");
                selectedRom[0] = 0;
                continue;
            }
#endif
        }
        else
        {
            FRESULT fr;
            FIL file;
            fr = f_open(&file, selectedRom, FA_READ);
            if (fr != FR_OK)
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open rom %d", fr);
                printf("%s\n", ErrorMessage);
                selectedRom[0] = 0;
                continue;
            }
            fileSize = f_size(&file);
            f_close(&file);
        }

        printf("Now playing: %s (%d bytes)\n", selectedRom, fileSize);

        if (isAutoSaveStateConfigured())
        {
            char tmpPath[40];
            getAutoSaveStatePath(tmpPath, sizeof(tmpPath));
            if (Frens::fileExists(tmpPath))
            {
                printf("Auto-save state found: %s\n", tmpPath);
                loadSaveStateMenu = true;
                quickSaveAction = SaveStateTypes::LOAD_AND_START;
                framesbeforeAutoStateIsLoaded = 120;
            }
        }

        do
        {
            reset = resetGame = false;
            InitPCE(PCE_AUDIO_RATE, true);
            if (LoadCard((uint8_t *)ROM_FILE_ADDR, fileSize) != 0)
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM load failed");
                printf("%s\n", ErrorMessage);
                break;
            }
            printf("Starting game\n");
            Frens::PaceFrames60fps(true);
            process();
            PCE.ROM = NULL; // prevent ShutdownPCE from freeing flash/PSRAM
            ShutdownPCE();
        } while (resetGame);

        selectedRom[0] = 0;
        showSplash = false;
#if ENABLE_VU_METER
        turnOffAllLeds();
#endif
    }

    return 0;
}
