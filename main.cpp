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
#include "cd.h"
#include "cd_chd.h"
#include "gfx.h"
}

#if HSTX
#include "drivers/pico_hdmi/hstx.h"
#endif

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

#define AUDIOBUFFERSIZE 4096
#define PCE_AUDIO_RATE 44100
// Native PCE NTSC frame rate is 59.826 Hz (263 lines * 1365 master clocks at
// 21.47727 MHz), not 60. At 44100 Hz audio that's 44100/59.826 ≈ 737.14
// samples per frame; we emit 737 most frames and 738 occasionally via an
// exact rational accumulator so audio production locks to 44100 Hz long-term.
// Buffer sizes use the ceiling.
#define PCE_SAMPLES_PER_FRAME_MAX 738
// PCE NTSC frame rate scaled by 10000 (avoids floats in the hot path).
#define PCE_FPS_X10000 598261u

#define EMULATOR_CLOCKFREQ_KHZ 252000
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

#ifndef CHECK_BIOS_AT_BOOT
#define CHECK_BIOS_AT_BOOT 1
#endif


// Per-scanline indexed line buffer: pce-go renders one scanline at a time.
// 16-byte scratch padding on each side (see render_lines() in gfx.c).
static uint8_t pce_line_buffer_storage[XBUF_WIDTH + 64];
static uint8_t *const pce_line_buffer = pce_line_buffer_storage + 16;
int osd_gfx_render_line = 0;

// Lookup table: 8-bit palette index -> RGB555 (HSTX) or RGB444 (DVI)
static uint16_t paletteLUT[256];

// Screen geometry — updated by osd_gfx_framebuffer() each time render_lines fires
static int current_screen_w = 256;
static int current_screen_h = 240;
static int current_x_offset = 32;
static int current_y_offset = 0;

// Settings visibility for PCE
const int8_t g_settings_visibility_pce[MOPT_COUNT] = {
    0,                               // Exit Game, or back to menu. Always visible when in-game.
    0,                               // Reset Game
    BOOTLOADER_BUILD,                // Return to emuLoader picker (only when built for the loader)
    0,                               // Save / Restore State
    1,                               // Screen Mode
    0,                               // Scanlines toggle (superseded by Screen Mode)
    HSTX,                            // Scanline Type (HSTX only)
    1,                               // FPS Overlay
    0,                               // Audio Enable
    0,                               // Frame Skip
    HSTX && ENABLEDVI,                            // Display Mode (HDMI or DVI, only when HSTX is enabled, because non-HSTX builds always use HDMI)
    (EXT_AUDIO_IS_ENABLED ), // External Audio
    1,                               // Font Color
    1,                               // Font Back Color
    ENABLE_VU_METER,                 // VU Meter
    //(HW_CONFIG == 8),                // Fruit Jam Internal Speaker
    (HW_CONFIG == 8),                // Fruit Jam Volume Control
    0,                               // DMG Palette (NES emulator does not use GameBoy palettes)
    0,                               // Border Mode (Super Gameboy style borders not applicable for NES)
    0,                               // Rapid Fire on A
    0,                               // Rapid Fire on B
    0,                               // Auto Insert Disk A, enabled at runtime on RP2350
    0,                               // Auto Swap FDS, enabled at runtime on RP2350
    0,                               // FDS Disk Swap (toggled on after fdsParse succeeds)
    0,                               // Overclock (CPU high clock toggle)
    1,                               // Enter bootsel mode
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

static int16_t pce_audio_buffer[PCE_SAMPLES_PER_FRAME_MAX * 2]; // stereo interleaved
static int audio_pos = 0;   // PSG samples synthesised so far this frame
static int pce_samples_this_frame = 737;

// Returns the number of audio samples this frame should produce. Exact rational
// accumulator: target rate = PCE_AUDIO_RATE / 59.8261 Hz. Drift-free long-term.
static int __not_in_flash_func(advance_samples_this_frame)(void)
{
    static uint32_t accum = 0;
    accum += (uint32_t)PCE_AUDIO_RATE * 10000u;
    int n = (int)(accum / PCE_FPS_X10000);
    accum -= (uint32_t)n * PCE_FPS_X10000;
    return n;  // ~737 or 738 per call; averages 737.143
}

#define AUDIO_SAMPLES_PER_SCANLINE 4

// pce-go internal cycles per frame: 263 scanlines * cycles_per_line (113).
#define PCE_CYCLES_PER_FRAME (263 * 113)

// Sample-accurate PSG: generate audio up to the frame-relative CPU cycle
// position of the register write that triggered this (called from pce_writeIO
// before the write is applied). Each register state is thus rendered for
// exactly the cycles it was active — short SFX / DDA are no longer lost to
// coarse per-batch sampling. The frame always completes to pce_samples_this_frame
// samples via the frame-end flush in osd_psg_scanline.
extern "C" void __not_in_flash_func(osd_psg_sync)(int frame_cycle)
{
    int target = (int)((uint64_t)frame_cycle * pce_samples_this_frame / PCE_CYCLES_PER_FRAME);
    if (target > pce_samples_this_frame)
        target = pce_samples_this_frame;
    int n = target - audio_pos;
    if (n > 0) {
        psg_update(pce_audio_buffer + audio_pos * 2, n, 0x3F);
        audio_pos += n;
    }
}

extern "C" void __not_in_flash_func(osd_psg_scanline)(void)
{
    // Frame-end flush: synthesise the remainder of this frame's samples even
    // if the game wrote no PSG registers late in the frame.
    if (PCE.Scanline >= 262)
        osd_psg_sync(PCE_CYCLES_PER_FRAME);
}

extern "C" void __not_in_flash_func(osd_gfx_lines_rendered)(int first_line, int last_line)
{
    (void)last_line;
    int display_y = first_line + current_y_offset;
    if (display_y < 0 || display_y >= 240) return;

    uint16_t *dst;
#if HSTX
    dst = hstx_getlineFromFramebuffer(display_y);
#elif FRAMEBUFFERISPOSSIBLE
    auto *b = Frens::isFrameBufferUsed() ? nullptr : dvi_->getLineBuffer();
    dst = b ? b->data() : &Frens::framebuffer[display_y * 320];
#else
    auto *b = dvi_->getLineBuffer();
    dst = b->data();
#endif

    if (current_x_offset > 0)
        memset(dst, 0, current_x_offset * sizeof(uint16_t));

    // 4 indexed pixels per iteration: one uint32_t load, four LUT lookups,
    // two uint32_t stores. pce_line_buffer and dst+offset are both 4-byte
    // aligned (PCE widths are multiples of 8, so x_offset is a multiple of 4).
    const uint32_t *src32 = (const uint32_t *)pce_line_buffer;
    uint32_t *dst32 = (uint32_t *)(dst + current_x_offset);
    int chunks = current_screen_w >> 2;
    for (int i = 0; i < chunks; i++)
    {
        uint32_t s = src32[i];
        uint32_t p0 = paletteLUT[s & 0xFF];
        uint32_t p1 = paletteLUT[(s >> 8) & 0xFF];
        uint32_t p2 = paletteLUT[(s >> 16) & 0xFF];
        uint32_t p3 = paletteLUT[(s >> 24)];
        dst32[i * 2]     = p0 | (p1 << 16);
        dst32[i * 2 + 1] = p2 | (p3 << 16);
    }
    // Defensive: handle a non-multiple-of-4 width (shouldn't happen on PCE).
    for (int x = chunks << 2; x < current_screen_w; x++)
        dst[x + current_x_offset] = paletteLUT[pce_line_buffer[x]];

    int right = current_x_offset + current_screen_w;
    if (right < 320)
        memset(dst + right, 0, (320 - right) * sizeof(uint16_t));

    if (settings.flags.displayFrameRate &&
        display_y >= current_y_offset + FPSSTART &&
        display_y <  current_y_offset + FPSEND)
    {
        WORD *fpsBuffer = dst + current_x_offset + 4;
        int rowInChar = display_y - current_y_offset - FPSSTART;
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

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
    if (b) dvi_->setLineBuffer(display_y, b);
#else
    dvi_->setLineBuffer(display_y, b);
#endif
#endif
}

#if PICO_RP2350
// Heap-allocated for the lifetime of a CD-game session so the menu doesn't
// hold ~3 KB it can't use.
static int16_t *cd_audio_buffer = nullptr;
#endif

// Audio-pipeline diagnostics: once per second during CD-DA playback, print
// the minimum levels reached by each buffer in the chain (CD-DA prefetch
// ring, HDMI data-island queue, I2S ring) plus the underrun delta. The
// buffer whose minimum approaches zero around an audible crackle is the
// one to fix. Compile with -DCD_AUDIO_DIAG=1; default off.
#ifndef CD_AUDIO_DIAG
#define CD_AUDIO_DIAG 0
#endif
#if CD_AUDIO_DIAG && PICO_RP2350
#if USE_I2S_AUDIO
extern "C" int audio_i2s_get_fill_permille();
#endif
static void __not_in_flash_func(cdAudioDiagTick)()
{
    static uint32_t min_cdring = UINT32_MAX, min_di = UINT32_MAX;
    static int min_i2s = INT32_MAX;
    static uint32_t last_underruns = 0;
    static int frames = 0;
    static bool played = false;

    if (!CD.cd_attached)
        return;
    if (CD.audio_status == 0)
        played = true;
    uint32_t rc = CD.audio_ring_count;
    if (rc < min_cdring) min_cdring = rc;
#if HSTX
    uint32_t di = hstx_di_queue_get_level();
    if (di < min_di) min_di = di;
#endif
#if USE_I2S_AUDIO
    int fp = audio_i2s_get_fill_permille();
    if (fp < min_i2s) min_i2s = fp;
#endif
    if (++frames < 60)
        return;
    if (played) {
        printf("[adiag] cdring_min=%lu/%u di_min=%lu i2s_min=%d underruns+%lu\n",
               (unsigned long)min_cdring, CD_AUDIO_RING_SECTORS,
               min_di == UINT32_MAX ? 0UL : (unsigned long)min_di,
               min_i2s == INT32_MAX ? -1 : min_i2s,
               (unsigned long)(cd_audio_underruns - last_underruns));
        last_underruns = cd_audio_underruns;
    }
    frames = 0;
    played = false;
    min_cdring = UINT32_MAX;
    min_di = UINT32_MAX;
    min_i2s = INT32_MAX;
}
#endif

#if !HSTX && PICO_RP2350
#if USE_I2S_AUDIO
extern "C" int audio_i2s_get_fill_permille();
#endif
// Fill level (permille, 0..1000) of whichever audio output is currently
// active — the DVI/HDMI audio ring or the I2S ring. Drives the audio-clock
// pace and frame-skip so both behave identically regardless of audio route.
static int __not_in_flash_func(cdAudioFillPermille)()
{
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
    {
#if USE_I2S_AUDIO
        return audio_i2s_get_fill_permille();
#else
        return 500; // other ext-audio drivers expose no fill level
#endif
    }
#endif
    auto &r = dvi_->getAudioRingBuffer();
    uint32_t sz = r.getBufferSize();
    return sz ? (int)((uint64_t)r.getFullReadableSize() * 1000u / sz) : 0;
}
#endif

// Per-frame: push audio, clear display borders, render FPS overlay.
static void __not_in_flash_func(pushAudioAndOverlay)()
{
#if PICO_RP2350
    if (CD.cd_attached) {
        // CD audio SD prefetch (cd_audio_update):
        //  - HSTX: core1 background task.
        //  - framebuffer DVI: during the PaceFrames60fps slack-wait (the
        //    vsyncWaitTask), overlapping the wait instead of adding frame time.
        //  - non-framebuffer DVI: no slack-wait, so prefetch here on core0.
#if !HSTX
        if (CD.audio_status == 0 && !Frens::isFrameBufferUsed())
            cd_audio_update();
#endif
        // CD-DA: generate into a scratch buffer and mix into the PSG output.
        if (CD.audio_status == 0 && cd_audio_buffer) {
            int n = cd_audio_generate_samples(cd_audio_buffer, pce_samples_this_frame);
            for (int i = 0; i < n * 2; i++) {
                int32_t v = (int32_t)pce_audio_buffer[i] + (int32_t)cd_audio_buffer[i];
                pce_audio_buffer[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        // ADPCM: decode + resample, summed directly into the PSG output.
        cd_adpcm_generate_samples(pce_audio_buffer, pce_samples_this_frame, PCE_AUDIO_RATE);
    }
#endif

    int audio_idx = 0;

#if HSTX
#if EXT_AUDIO_IS_ENABLED
    const bool routeToExtAudio = settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected();
#endif
    for (int y = 0; y < 240; y++)
    {
        for (int a = 0; a < AUDIO_SAMPLES_PER_SCANLINE && audio_idx < pce_samples_this_frame; a++, audio_idx++)
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
    // Clear top/bottom border lines in HSTX framebuffer
    for (int y = 0; y < current_y_offset; y++)
        memset(hstx_getlineFromFramebuffer(y), 0, 320 * sizeof(uint16_t));
    for (int y = current_y_offset + current_screen_h; y < 240; y++)
        memset(hstx_getlineFromFramebuffer(y), 0, 320 * sizeof(uint16_t));

#elif !HSTX
    // DVI audio (framebuffer and line-streaming paths)
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
    {
        for (int a = 0; a < audio_pos; a++)
            EXT_AUDIO_ENQUEUE_SAMPLE(pce_audio_buffer[a * 2], pce_audio_buffer[a * 2 + 1]);
    } else {
#endif
        // Batch push to the DVI audio ring buffer.
        int remaining = audio_pos;
        int pos = 0;
        while (remaining > 0)
        {
            auto &ring = dvi_->getAudioRingBuffer();
            auto n = std::min<int>(remaining, ring.getWritableSize());
            if (!n)
                break;
            auto p = ring.getWritePointer();
            for (int a = 0; a < n; a++)
                *p++ = {pce_audio_buffer[(pos + a) * 2],
                        pce_audio_buffer[(pos + a) * 2 + 1]};
            ring.advanceWritePointer(n);
            remaining -= n;
            pos += n;
        }
#if EXT_AUDIO_IS_ENABLED
    }
#endif
#if FRAMEBUFFERISPOSSIBLE
    if (Frens::isFrameBufferUsed())
    {
        // Clear top/bottom border lines in DVI framebuffer
        for (int y = 0; y < current_y_offset; y++)
            memset(&Frens::framebuffer[y * 320], 0, 320 * sizeof(uint16_t));
        for (int y = current_y_offset + current_screen_h; y < 240; y++)
            memset(&Frens::framebuffer[y * 320], 0, 320 * sizeof(uint16_t));
    }
#endif
#endif

#if CD_AUDIO_DIAG && PICO_RP2350
    cdAudioDiagTick();
#endif
    // FPS overlay is rendered inline in osd_gfx_lines_rendered()
}

// OSD callbacks required by pce-go
extern "C" uint8_t *__not_in_flash_func(osd_gfx_framebuffer)(int width, int height)
{
    current_screen_w = (width > 320) ? 320 : width;
    current_screen_h = (height > 240) ? 240 : height;
    current_x_offset = (320 - current_screen_w) / 2;
    current_y_offset = (240 - current_screen_h) / 2;

#if HSTX
    static int last_w = -1;
    if (current_screen_w != last_w)
    {
        hstx_setAspectRatio87((scaleMode8_7_ && current_screen_w <= 256) ? 1 : 0);
        last_w = current_screen_w;
    }
#endif

    return pce_line_buffer - osd_gfx_render_line * XBUF_WIDTH;
}

extern "C" void osd_vsync(void)
{
}

extern "C" void osd_input_read(uint8_t joypads[8])
{
    // Input is read explicitly in the main loop
}

static inline int ProcessAfterFrameIsRendered()
{
    Frens::pollHeadPhoneJack();
    // Pulse-vsync wait for non-CD games (HuCards) on PicoDVI — locks to the
    // DVI's 60Hz pulse, same as before. CD games override this via the
    // audio-clock pace registered with setAudioPaceQuery.
    Frens::PaceFrames60fps(false, !CD.cd_attached);
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
        // 1-second windowed average. The previous per-frame ceil(1e6/interval)
        // misread slack-paced frames: in heavy scenes a no-wait frame (~15.5ms)
        // alternates with a longer waiting frame, flashing 63-65 on screen even
        // though the average is locked at ~59.83fps.
        static uint32_t fpsFrames = 0;
        fpsFrames++;
        uint32_t elapsed_us = Frens::time_us() - start_tick_us;
        if (elapsed_us >= 1000000)
        {
            fps = (fpsFrames * 1000000ull + elapsed_us / 2) / elapsed_us;
            fpsFrames = 0;
            start_tick_us = Frens::time_us();
            fpsString[0] = '0' + (fps / 10);
            fpsString[1] = '0' + (fps % 10);
        }
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
    bool skipRender = false;
    uint32_t frameWorkStartUs = time_us_32();
    while (reset == false)
    {
        readInputAndMapToPCE(&pdwPad1, &pdwPad2, &pdwSystem, false, nullptr);
        audio_pos = 0;
        pce_samples_this_frame = advance_samples_this_frame();
        gfx_set_skip_render(skipRender);
        pce_run();
        pushAudioAndOverlay();
#if !HSTX && PICO_RP2350
        // Audio-clock pacing for CD games on the framebuffer DVI path, for both
        // HDMI and I2S (the I2S ring is enlarged to 4096 so it has the same
        // >=2-frame depth the audio-clock lock needs). cdAudioFillPermille()
        // reports whichever output is active. Disabled only for ext-audio
        // drivers that expose no fill level.
        bool cdFb = CD.cd_attached && Frens::isFrameBufferUsed();
#if EXT_AUDIO_IS_ENABLED && !USE_I2S_AUDIO
        if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
            cdFb = false; // SPI/other ext audio: no fill query → timer pace
#endif
        Frens::setAudioPaceQuery(cdFb ? cdAudioFillPermille : nullptr);
#endif
        // Emulation+render time for this frame, EXCLUDING the pace wait below.
        // The skip trigger must compare work done against the frame budget;
        // measuring across the pace wait makes the interval equal the paced
        // frame period (~16.7ms), which exceeds any sub-frame threshold and
        // made skipRender alternate every frame regardless of load.
        uint32_t frameWorkUs = time_us_32() - frameWorkStartUs;
        ProcessAfterFrameIsRendered();
        frameWorkStartUs = time_us_32();
#if PICO_RP2350
        // Frame-skip when the emulator can't hold the frame rate. Originally
        // for CD games (audio underrun → HDMI drops / I2S crackles); now also
        // applied to HuCard / SGX scenes with heavy sprite work where the
        // PSG audio crackles for the same reason. Skipping rendering keeps
        // CPU+audio running at full speed while only the picture stalls.
        // Never skip twice in a row → ≥30fps video.
        //
        // PicoDVI CD-FB path keeps the precise audio-ring "behind" signal
        // (only available there). Everything else falls back to the work-time
        // overrun: PCE_FRAME_SKIP_OVERRUN_US is tunable from CMakeLists.txt —
        // proactive (just under one 59.826Hz frame, 16715 µs) so we skip
        // before the audio queue actually underruns.
#ifndef PCE_FRAME_SKIP_OVERRUN_US
#define PCE_FRAME_SKIP_OVERRUN_US 16250u
#endif
#if !HSTX
        if (CD.cd_attached)
        {
            // PicoDVI CD: audio-ring fill is the authoritative signal.
            bool behind = cdFb && cdAudioFillPermille() < 250;
            skipRender = behind && !skipRender;
        }
        else
        {
            // PicoDVI HuCard / SGX: no audio-ring query → work-time overrun.
            bool behind = frameWorkUs > PCE_FRAME_SKIP_OVERRUN_US;
            skipRender = behind && !skipRender;
        }
#else
        // HSTX (CD or HuCard / SGX): work-time overrun. CD has no audio-ring
        // query on core0 either, so both use the same trigger.
        bool behind = frameWorkUs > PCE_FRAME_SKIP_OVERRUN_US;
        skipRender = behind && !skipRender;
#endif

        // A/B experiment switch: build with -DPCE_DISABLE_FRAMESKIP=1 to never
        // skip rendering. Use it to decide whether the Sapphire "freeze" during
        // intensive action is the frameskip itself (freeze disappears, possibly
        // replaced by audio crackle / sub-60fps slowdown) or something downstream
        // in the display path (freeze persists). See project_sapphire_freeze_phase_a.
#if PCE_DISABLE_FRAMESKIP
        skipRender = false;
#endif

        // 1 Hz UART diagnostic of frameskip behaviour. Build with
        // -DPCE_FRAMESKIP_DIAG=1. Reports per second: total frames, how many
        // were render-skipped, the worst single-frame emulation+render time,
        // and (CD on PicoDVI) the minimum CD-DA ring fill permille — i.e. the
        // exact signals that trigger a skip. Capture this while reproducing the
        // Sapphire freeze to see if skips spike with the visible stall.
#if PCE_FRAMESKIP_DIAG
        {
            static uint32_t fs_frames = 0, fs_skips = 0, fs_maxwork = 0, fs_t0 = 0;
            static uint32_t fs_minfill = 1000;
            fs_frames++;
            if (skipRender) fs_skips++;
            if (frameWorkUs > fs_maxwork) fs_maxwork = frameWorkUs;
#if !HSTX
            if (CD.cd_attached && cdFb)
            {
                uint32_t fill = cdAudioFillPermille();
                if (fill < fs_minfill) fs_minfill = fill;
            }
#endif
            uint32_t fs_now = time_us_32();
            if (fs_t0 == 0) fs_t0 = fs_now;
            if (fs_now - fs_t0 >= 1000000u)
            {
                printf("[fsdiag] frames=%lu skips=%lu maxwork_us=%lu minfill=%lu\n",
                       (unsigned long)fs_frames, (unsigned long)fs_skips,
                       (unsigned long)fs_maxwork, (unsigned long)fs_minfill);
                fs_frames = fs_skips = fs_maxwork = 0;
                fs_minfill = 1000;
                fs_t0 = fs_now;
            }
        }
#endif
#endif
    }
    gfx_set_skip_render(false);
}

static char selectedRom[FF_MAX_LFN];

int main()
{
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    int fileSize = 0;
    vreg_voltage voltage = VREG_VOLTAGE_1_20;
#if SGX
    Frens::FlashParams *flashParams;
    // assign flashParams to point to flash location

    bool freqOverruled = false;
    flashParams = (Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if ( Frens::validateFlashParams(*flashParams) ) {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
        freqOverruled = true;
    }
#endif
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);

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

    // CD BIOS self-test: scan /bios/ once at boot and log what we find.
    // This is purely diagnostic — actual BIOS loading happens in LoadDisc()
    // when the user selects a .cue file. Requires PSRAM and a mounted SD.
    //
    // All FatFS-heavy locals are static. The 3 KB main stack would overflow
    // into core1's SCRATCH region with deep call chains here (DIR + FILINFO
    // + multiple FF_MAX_LFN buffers + cd_find_bios's own static-now buffers
    // is fine, but stack-allocating here on top is not).
#if CHECK_BIOS_AT_BOOT
    if (!isFatalError && Frens::isPsramEnabled())
    {
        printf("--- CD BIOS self-test ---\n");
        static char     biosPath[FF_MAX_LFN + 8];
        bios_variant_t  variant = BIOS_UNKNOWN;
        if (cd_find_bios(biosPath, sizeof(biosPath), NULL, &variant) == 0) {
            printf("Found CD BIOS: %s (region=%s)\n",
                   biosPath, cd_bios_is_us() ? "US" : "JP");
        } else {
            printf("No CD BIOS in /bios/ — CD games will fail to load until one is added.\n");
        }
        printf("-------------------------\n");

        // CUE parser diagnostic: parse every .cue file in /diag/ (if any).
        // Drop CUE+BIN pairs into /diag/ to validate parsing without booting.
        static DIR     diagDir;
        static FILINFO diagFno;
        static char    diagCuePath[FF_MAX_LFN + 8];
        if (f_opendir(&diagDir, "/diag") == FR_OK) {
            printf("--- CUE parser diagnostic (/diag/) ---\n");
            int count = 0;
            while (f_readdir(&diagDir, &diagFno) == FR_OK && diagFno.fname[0]) {
                if (diagFno.fattrib & (AM_DIR | AM_HID)) continue;
                size_t n = strlen(diagFno.fname);
                if (n < 4 || strcasecmp(diagFno.fname + n - 4, ".cue") != 0) continue;

                snprintf(diagCuePath, sizeof(diagCuePath), "/diag/%s", diagFno.fname);
                printf("[diag] parsing %s\n", diagCuePath);
                if (cd_load_cue(diagCuePath) == 0) {
                    printf("[diag] OK\n");
                } else {
                    printf("[diag] FAILED\n");
                }
                cd_close();
                count++;
            }
            f_closedir(&diagDir);
            if (count == 0) {
                printf("[diag] /diag/ is empty (no .cue files)\n");
            }
            printf("--------------------------------------\n");
        }
    }
#endif

    buildPaletteLUT();

    bool showSplash = true;
    g_settings_visibility = g_settings_visibility_pce;
    g_available_screen_modes = g_available_screen_modes_pce;

    while (true)
    {
        if (strlen(selectedRom) == 0 || reset == true)
        {
            // CD-ROM games (.cue) require PSRAM for the additional ~2.6MB of
            // CD/SCD/ADPCM/Arcade Card buffers.
            // SuperGrafx (.sgx) needs an extra 64 KB VRAM2 that doesn't fit in
            // the SRAM heap alongside the 150 KB framebuffer; we route it to
            // PSRAM and therefore require PSRAM for .sgx as well. Most SGX
            // carts are 1 MB anyway, which already needs PSRAM to hold the ROM.
            const char *menuExts;
#if PICO_RP2350
#if ENABLE_CHD
            menuExts = Frens::isPsramEnabled() ? ".pce .cue .chd .sgx" : ".pce";
#else
            menuExts = Frens::isPsramEnabled() ? ".pce .cue .sgx" : ".pce";
#endif
#else
            menuExts = ".pce";
#endif
            menu("Pico-PCE+", ErrorMessage, isFatalError, showSplash, menuExts, selectedRom);
            printf("Selected rom from menu: %s\n", selectedRom);
        }
        reset = false;
        fileSize = 0;

        // Detect CD-ROM / SuperGrafx / HuCard by file extension.
        bool isCDGame = false;
        bool isSgxGame = false;
        if (strlen(selectedRom) > 0) {
            char ext[8];
            Frens::getextensionfromfilename(selectedRom, ext, sizeof(ext));
            isCDGame  = (strcasecmp(ext, ".cue") == 0
#if ENABLE_CHD
                      || strcasecmp(ext, ".chd") == 0
#endif
                        );
            isSgxGame = (strcasecmp(ext, ".sgx") == 0);
        }

        if (isCDGame)
        {
            // The menu's PSRAM loader copied the (small) .cue text into PSRAM
            // at ROM_FILE_ADDR. We don't need it — LoadDisc() re-parses the
            // CUE directly from SD and loads the BIOS into PSRAM instead.
#if PICO_RP2350 && PSRAM_CS_PIN
            if (Frens::isPsramEnabled() && ROM_FILE_ADDR) {
                Frens::f_free((void *)ROM_FILE_ADDR);
                ROM_FILE_ADDR = 0;
            }
#endif
            printf("Now playing (CD): %s\n", selectedRom);
        }
        else if (Frens::isPsramEnabled())
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

        if (!isCDGame)
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
            SetSgxModePCE(isSgxGame);
            InitPCE(PCE_AUDIO_RATE, true);
            int loadResult;
            if (isCDGame) {
                loadResult = LoadDisc(selectedRom);
                if (loadResult == -1) {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "No BIOS in /bios/");
                } else if (loadResult != 0) {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "CD load failed");
                }
            } else {
                loadResult = LoadCard((uint8_t *)ROM_FILE_ADDR, fileSize);
                if (loadResult != 0) {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM load failed");
                }
            }
            if (loadResult != 0)
            {
                printf("%s\n", ErrorMessage);
                break;
            }

            if (isCDGame) {
                char bramPath[50];
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X/bram.sav",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                cd_bram_load(bramPath);
            }

            printf("Starting game\n");
            // CHD on HSTX needs a larger core1 stack (libchdr's chd_read uses
            // ~6-10 KB; default is 4 KB and overflowing zeroes the hunk-cache
            // pointer array → silent CD audio). Growing the static stack
            // permanently starves SGX's SRAM heap, so swap core1's stack only
            // for the duration of this CHD game by tearing HSTX down,
            // resetting core1, and relaunching it with a fresh 8 KB heap
            // buffer. The pointer is remembered here so the exit branch can
            // restore the default stack and free the heap buffer.
#if HSTX && ENABLE_CHD
            uint32_t *chd_core1_stack = nullptr;
            if (isCDGame && cd_chd_is_active()) {
                chd_core1_stack = (uint32_t *)malloc(8192);
                if (chd_core1_stack) {
                    printf("[hstx] swapping core1 stack: default 4 KB -> 8 KB malloc for CHD\n");
                    hstx_restart_core1(chd_core1_stack, 8192);
                } else {
                    printf("[hstx] WARN: malloc(8192) for CHD core1 stack failed; CD audio will be silent\n");
                }
            }
#endif
            if (isCDGame) {
                cd_audio_buffer = (int16_t *)malloc(PCE_SAMPLES_PER_FRAME_MAX * 2 * sizeof(int16_t));
                if (!cd_audio_buffer)
                    printf("WARN: malloc cd_audio_buffer failed; CD audio will be silent\n");
#if HSTX
                extern void video_output_set_background_task(void (*)(void));
                video_output_set_background_task(cd_audio_update);
#else
                // Framebuffer DVI: prefetch CD audio during the pace slack-wait.
                if (Frens::isFrameBufferUsed())
                    Frens::setVSyncWaitTask(cd_audio_update);
#endif
            }
            Frens::PaceFrames60fps(true, !isCDGame);
            process();
            if (isCDGame) {
#if HSTX
                extern void video_output_set_background_task(void (*)(void));
                video_output_set_background_task(nullptr);
#else
                Frens::setVSyncWaitTask(nullptr);
#endif
                char bramPath[50];
                snprintf(bramPath, sizeof(bramPath), SAVESTATEDIR);
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s",
                         FrensSettings::getEmulatorTypeString());
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                f_mkdir(bramPath);
                snprintf(bramPath, sizeof(bramPath),
                         SAVESTATEDIR "/%s/%08X/bram.sav",
                         FrensSettings::getEmulatorTypeString(),
                         Frens::getCrcOfLoadedRom());
                cd_bram_save(bramPath);
                cd_close();
                free(cd_audio_buffer);
                cd_audio_buffer = nullptr;
            } else {
                PCE.ROM = NULL; // prevent ShutdownPCE from freeing flash/PSRAM
            }
#if HSTX && ENABLE_CHD
            if (chd_core1_stack) {
                size_t default_bytes = 0;
                void *default_stack = hstx_default_core1_stack(&default_bytes);
                printf("[hstx] restoring core1 stack to default 4 KB\n");
                hstx_restart_core1((uint32_t *)default_stack, default_bytes);
                free(chd_core1_stack);
                chd_core1_stack = nullptr;
            }
#endif
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
