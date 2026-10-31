/*
 * picoDragonsLair - Dragon's Lair (Cinematronics, 1983) laserdisc arcade
 * emulator for RP2350 boards.
 *
 * Boots the pico_shared framework, loads the ROM set and opens the laserdisc
 * file from the SD card, and runs the emulated board (dlair/) one video field
 * per display frame. The core is plain C shared with the host harness in
 * hosttest/; this file wires it to the display, audio and controller drivers
 * and to the settings menu.
 *
 * As in the other arcade games of this family there is no ROM browser: the
 * board boots straight into the game, and only the in-game settings menu
 * (SELECT + START) is used. Two things are read from ROMDIR:
 *   - MAME's dlair.zip (revision F2) as is, or its files unzipped: the Z80
 *     program;
 *   - lair.pld: the laserdisc, converted once on a PC from MAME's dlair.chd by
 *     the ld2pico program (tools/ld2pico). All pictures and sound of the game
 *     are on it.
 * When either is missing, an error screen says what to do.
 *
 * Everything runs on core0; core1 belongs to the display driver. Each display
 * frame the loop shows the laserdisc picture that is due, runs one field of
 * the Z80 and the laserdisc player, and mixes the sound. The rest of the frame
 * is spent waiting for the next one, and that wait is filled with reading the
 * laserdisc's pictures and sound and decoding the next picture
 * (ldstream_step), one small step at a time, until shortly before the next
 * frame begins.
 */

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

#include "ff.h"
#include "tusb.h"
#if DL_SD_FAST_HZ
#include "hardware/spi.h"
#endif

#include "FrensHelpers.h"
#include "FrensFonts.h"
#include "gamepad.h"
#include "menu.h"
#include "menu_settings.h"
#include "nespad.h"
#include "settings.h"
#include "vumeter.h"
#include "wiipad.h"

#include "dlair.h"
#include "ldstream.h"
#include "romload.h"
#include "scoreboard.h"

// The emulated board is light (a Z80 at 4 MHz); the work is in reading the
// laserdisc pictures from the SD card. The clock stays at 252 MHz, which is
// what PicoDVI needs for 640x480 at 60 Hz.
#define DLAIR_CLOCKFREQ_KHZ 252000

// Must be a power of two (util::RingBuffer asserts it). It holds the audio
// cushion (see pushAudio()) plus a whole frame of 735 samples. The I2S ring of
// pico_shared is given the same size in CMakeLists.txt.
#define AUDIOBUFFERSIZE 2048

#define SAMPLERATE 44100
#define SAMPLES_PER_FIELD (SAMPLERATE / 60) // 735

#define ROMDIR "/roms/arcade/DLAIR"
#define DISCFILE ROMDIR "/lair.pld"

// The laserdisc decoder's buffers: the picture records it reads ahead, and the
// sound track (8 KB, 0.37 s). With PSRAM the read-ahead buffer is 1 MB there,
// several seconds of pictures, so that light scenes fill it ahead of heavy
// ones. Without PSRAM it is in SRAM; HSTX boards have the HSTX framebuffer and
// audio queue in SRAM as well, so they get the smaller one.
#define DISC_BUFFER_PSRAM (1024 * 1024)
#if HSTX
#define DISC_BUFFER (32 * 1024)
#else
#define DISC_BUFFER (48 * 1024)
#endif
#define DISC_AUDIO_BUFFER (8 * 1024)
#define DISC_READ_CHUNK 8192

// Decoding stops this long before the next display frame is due. Each read is
// sized to the time left, at the speed the card has shown so far.
#define FRAME_US 16667
#define STREAM_MARGIN_US 1500

// Output gains in Q8 (256 = unity). The converter brings the laserdisc sound
// to full level, so HDMI plays it as it is; the I2S DAC drives the amplifier
// or headphones directly, with no volume stage after it, and gets half.
#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 256
#endif
#ifndef EXT_AUDIO_GAIN_Q8
#define EXT_AUDIO_GAIN_Q8 128
#endif

// Mix, in the proportion of MAME's driver (the disc at its own level, the AY at
// 0.33). The AY makes the coin sound and the joystick feedback: a short 87 Hz
// buzz on every press, which the player has to hear over the disc's sound.
// The converter brings the disc's sound to full level (about 4.5 times the
// disc's own), so it is played lower here and the AY higher.
#define LD_AUDIO_GAIN_Q8 160
#define AY_LEVEL 15000

// Optional: raise the SD card's SPI clock after mounting (hardware SPI only).
// 0 keeps pico_shared's clock. The SD driver does not check CRCs, so a too
// fast clock would show up as checksum errors in the laserdisc records; after
// a few of them the original clock is restored.
#ifndef DL_SD_FAST_HZ
#define DL_SD_FAST_HZ 0
#endif

static uint32_t CPUFreqKHz = DLAIR_CLOCKFREQ_KHZ;

static dl_roms_t roms;
static dl_machine_t machine;
static ldstream_t disc;
static FIL discFile;
static FIL discAudio; // a second handle on the same file, for the sound track
static uint8_t __attribute__((aligned(4))) picture[PLD_WIDTH * PLD_HEIGHT];
static int16_t audioBuf[SAMPLES_PER_FIELD];

static bool haveRoms = false;
static bool haveDisc = false;
static int discError = 0; // ldstream_open() error, or -1 when the file is missing
static uint32_t romsFound = 0;
static bool showSettings = false;
static bool scoreboardOn = true;
static uint32_t fps = 0;          // laserdisc pictures shown per second
static uint32_t picturesShown = 0;
static uint64_t fpsStartUs = 0;
static uint32_t lastVsyncUs = 0;

// ---------------------------------------------------------------------------
// Settings menu wiring.
//
// Positional, indexed by MenuSettingsIndex - append only, never reorder.
// 1 = shown, 0 = hidden, -1 = never shown. 0 and -1 differ only for Exit, Save/
// Restore state and Reset, which the in-game menu shows unless they are -1.
// C++ designated initializers may not skip members, so every entry up to the
// last one used is listed.
// ---------------------------------------------------------------------------
int8_t g_settings_visibility_dlair[MOPT_COUNT] = {
    [MOPT_EXIT_GAME] = -1,                         // nowhere to exit to: there is no ROM browser
    [MOPT_RESET_GAME] = 1,
    [MOPT_REBOOT_TO_LOADER] = BOOTLOADER_BUILD,    // return to the bootloader's picker
    [MOPT_SAVE_RESTORE_STATE] = -1,                // no save states
    [MOPT_SCREENMODE] = 1,
    [MOPT_SCANLINES] = 0,                          // covered by the screen modes
    [MOPT_SCANLINE_TYPE] = HSTX,
    [MOPT_FPS_OVERLAY] = 1,
    [MOPT_AUDIO_ENABLE] = 1,
    [MOPT_FRAMESKIP] = 0,
    [MOPT_DISPLAY_MODE] = HSTX && ENABLEDVI,
    [MOPT_EXTERNAL_AUDIO] = EXT_AUDIO_IS_ENABLED,
    [MOPT_FONT_COLOR] = 0,
    [MOPT_FONT_BACK_COLOR] = 0,
    [MOPT_FRUITJAM_VUMETER] = ENABLE_VU_METER,
    [MOPT_FRUITJAM_VOLUME_CONTROL] = (HW_CONFIG == 8),
    [MOPT_DMG_PALETTE] = 0,                        // Game Boy
    [MOPT_BORDER_MODE] = 0,                        // Game Boy
    [MOPT_RAPID_FIRE_ON_A] = 0,                    // the sword is not a fire button
    [MOPT_RAPID_FIRE_ON_B] = 0,
    [MOPT_AUTO_INSERT_FDS_DISK_A] = 0,             // Famicom Disk System
    [MOPT_AUTO_SWAP_FDS_DISK] = 0,                 // Famicom Disk System
    [MOPT_FDS_DISK_SWAP] = 0,                      // Famicom Disk System
    [MOPT_OVERCLOCK] = 0,                          // ROM browser only; PicoDVI needs 252 MHz
    [MOPT_FM_AUDIO] = 0,                           // Master System YM2413
    [MOPT_ENTER_BOOTSEL_MODE] = 1,
    [MOPT_CONTROLLER_TEST] = 1,
    [MOPT_RECENT_GAMES] = 0,                       // ROM browser only
    [MOPT_USB_DRIVE_MODE] = 0,                     // shown regardless: FRENS_FORCE_USB_MSC_IN_SETTINGS
    [MOPT_CASSETTE] = 0,                           // TI-99/4A
    [MOPT_DISK] = 0,                               // TI-99/4A
    [MOPT_SERIAL_KEYBOARD] = 0,                    // TI-99/4A
    [MOPT_SPRITE_LIMIT] = 0,                       // NES
    [MOPT_MENU_OVERSCAN] = 0,                      // shown regardless: listed below the menu colors
    [MOPT_GENESIS_PAD] = 0,                        // Genesis
    [MOPT_NES_PALETTE] = 0,                        // NES
    [MOPT_BUTTON_LAYOUT] = 0,                      // NES
    [MOPT_TATE_MODE] = 0,                          // a horizontal game
};

// The 8:7 modes stretch NES pixels; the laserdisc picture is 4:3 at 320x240,
// so only the 1:1 modes are offered.
const uint8_t g_available_screen_modes_dlair[] = {
    0, // SCANLINE_8_7
    0, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1, // NOSCANLINE_1_1
};

// ---------------------------------------------------------------------------
// Framebuffer access: the one place the two video back-ends differ.
// Both are a contiguous 320x240 16-bit buffer, RGB555 on HSTX and RGB444 on
// PicoDVI.
// ---------------------------------------------------------------------------
static inline uint16_t *fbLine(int line)
{
#if HSTX
    return hstx_getlineFromFramebuffer(line);
#else
    return &Frens::framebuffer[line * SCREENWIDTH];
#endif
}

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if HSTX
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
#else
    return (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
#endif
}

static inline uint32_t frameCounter()
{
#if HSTX
    return hstx_getframecounter();
#else
    return dvi_->getFrameCounter();
#endif
}

// ---------------------------------------------------------------------------
// Text on the framebuffer, for the error screen and the FPS overlay. pico_shared's
// putText() only works inside its own menus, but the 8x8 font is exported.
// ---------------------------------------------------------------------------
static void screenClear(uint16_t bg)
{
    for (int y = 0; y < SCREENHEIGHT; y++)
    {
        uint16_t *dst = fbLine(y);
        for (int x = 0; x < SCREENWIDTH; x++)
            dst[x] = bg;
    }
}

// col/row in 8x8 cells of the 40x30 screen; col < 0 centres the text.
static void screenText(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    const int cols = SCREENWIDTH / FONT_CHAR_WIDTH;
    const int len = (int)strlen(s);
    if (col < 0)
        col = (len < cols) ? (cols - len) / 2 : 0;
    for (int i = 0; i < len && col + i < cols; i++)
    {
        char c = s[i];
        if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS)
            c = ' ';
        for (int line = 0; line < FONT_CHAR_HEIGHT; line++)
        {
            uint16_t *dst = fbLine(row * FONT_CHAR_HEIGHT + line) + (col + i) * FONT_CHAR_WIDTH;
            // leftmost pixel in the low bit
            char slice = getcharslicefrom8x8font(c, line);
            for (int bit = 0; bit < FONT_CHAR_WIDTH; bit++, slice >>= 1)
                *dst++ = (slice & 1) ? fg : bg;
        }
    }
}

#define SCR_BG rgb(0, 0, 0)
#define SCR_FG rgb(200, 200, 200)
#define SCR_HI rgb(255, 64, 64)
#define SCR_FILE rgb(255, 255, 255)

static void drawErrorScreen(bool sdOk)
{
    char buf[48];
    int row = 2;

    screenClear(SCR_BG);
    screenText(-1, row++, "picoDragonsLair", SCR_HI, SCR_BG);
    row++;
    if (!sdOk)
    {
        screenText(1, row++, "No SD card found.", SCR_HI, SCR_BG);
        row++;
    }
    else
    {
        if (romsFound != DL_ROMS_ALL)
        {
            int missing = 0;
            for (int i = 0; i < DL_ROMFILE_COUNT; i++)
                missing += !(romsFound & (1u << i));
            if (missing == DL_ROMFILE_COUNT)
                screenText(1, row++, "No Dragon's Lair ROMs (rev. F2)", SCR_HI, SCR_BG);
            else
                screenText(1, row++, "Incomplete ROM set, missing:", SCR_HI, SCR_BG);
            for (int i = 0; i < DL_ROMFILE_COUNT && missing != DL_ROMFILE_COUNT; i++)
            {
                if (!(romsFound & (1u << i)))
                {
                    snprintf(buf, sizeof(buf), "  %s", dl_romfiles[i].name);
                    screenText(1, row++, buf, SCR_FILE, SCR_BG);
                }
            }
            row++;
        }
        if (!haveDisc)
        {
            if (discError < 0)
                screenText(1, row++, "The laserdisc file is missing:", SCR_HI, SCR_BG);
            else if (discError == 4)
                screenText(1, row++, "Old laserdisc file, convert again:", SCR_HI, SCR_BG);
            else
            {
                snprintf(buf, sizeof(buf), "Cannot use the laserdisc file (%d):", discError);
                screenText(1, row++, buf, SCR_HI, SCR_BG);
            }
            snprintf(buf, sizeof(buf), "  %s", DISCFILE);
            screenText(1, row++, buf, SCR_FILE, SCR_BG);
            row++;
        }
    }

    row = 15;
    screenText(1, row++, "Copy MAME's dlair.zip unchanged, or", SCR_FG, SCR_BG);
    screenText(1, row++, "its files unzipped, to the folder", SCR_FG, SCR_BG);
    snprintf(buf, sizeof(buf), "  %s", ROMDIR);
    screenText(1, row++, buf, SCR_FILE, SCR_BG);
    row++;
    screenText(1, row++, "Make lair.pld from MAME's dlair.chd", SCR_FG, SCR_BG);
    screenText(1, row++, "with the ld2pico program (README)", SCR_FG, SCR_BG);
    screenText(1, row++, "and copy it to the same folder.", SCR_FG, SCR_BG);

    screenText(1, 26, "SELECT+START opens settings, which", SCR_HI, SCR_BG);
    screenText(1, 27, "can enter USB drive mode.", SCR_HI, SCR_BG);
}

// ---------------------------------------------------------------------------
// ROM loading through FatFs
// ---------------------------------------------------------------------------
static int ffListDir(void *ctx, const char *dir, dl_dir_cb cb, void *arg)
{
    (void)ctx;
    DIR *d = (DIR *)Frens::f_malloc(sizeof(DIR));
    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    int ok = 0;
    if (d && fno && f_opendir(d, dir) == FR_OK)
    {
        ok = 1;
        while (f_readdir(d, fno) == FR_OK && fno->fname[0])
            cb(arg, fno->fname, (uint32_t)fno->fsize, (fno->fattrib & AM_DIR) ? 1 : 0);
        f_closedir(d);
    }
    Frens::f_free(fno);
    Frens::f_free(d);
    return ok;
}

static void *ffOpen(void *ctx, const char *path)
{
    (void)ctx;
    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (fil && f_open(fil, path, FA_READ) == FR_OK)
        return fil;
    Frens::f_free(fil);
    return nullptr;
}

static uint32_t ffSize(void *ctx, void *file)
{
    (void)ctx;
    return (uint32_t)f_size((FIL *)file);
}

static int ffReadAt(void *ctx, void *file, uint32_t ofs, void *buf, uint32_t len)
{
    (void)ctx;
    UINT br = 0;
    if (f_lseek((FIL *)file, ofs) != FR_OK || f_read((FIL *)file, buf, len, &br) != FR_OK)
        return -1;
    return (int)br;
}

static void ffClose(void *ctx, void *file)
{
    (void)ctx;
    f_close((FIL *)file);
    Frens::f_free(file);
}

static bool loadRoms()
{
    const dl_io_t io = {nullptr, ffListDir, ffOpen, ffSize, ffReadAt, ffClose};
    romsFound = dl_romload(&roms, &io, ROMDIR, 0);
    if (romsFound != DL_ROMS_ALL)
        romsFound = dl_romload(&roms, &io, ROMDIR "/dlair", romsFound);
    printf("ROM set: mask %lx (%s)\n", (unsigned long)romsFound,
           romsFound == DL_ROMS_ALL ? "complete" : "incomplete");
    return romsFound == DL_ROMS_ALL;
}

// ---------------------------------------------------------------------------
// The laserdisc file. The decoder reads whole sectors at sector offsets; FatFs
// reads those straight into the buffer, several sectors per command. The
// pictures and the sound are read through two handles, so that each moves
// forward on its own and f_lseek is rarely more than a check.
// ---------------------------------------------------------------------------
static int discRead(void *ctx, uint32_t ofs, void *dst, uint32_t len)
{
    FIL *f = (FIL *)ctx;
    if (f_tell(f) != ofs && f_lseek(f, ofs) != FR_OK)
        return -1;
    UINT br = 0;
    if (f_read(f, dst, len, &br) != FR_OK)
        return -1;
    return (int)br;
}

static bool openDisc()
{
    if (f_open(&discFile, DISCFILE, FA_READ) != FR_OK)
    {
        discError = -1;
        printf("%s: not found\n", DISCFILE);
        return false;
    }
    if (f_open(&discAudio, DISCFILE, FA_READ) != FR_OK)
    {
        discError = 1;
        f_close(&discFile);
        return false;
    }
    const ldstream_io_t io = {discRead, &discFile, &discAudio};
    uint8_t *ring = nullptr;
    uint32_t ringSize = DISC_BUFFER;
#if PICO_RP2350 && PSRAM_CS_PIN
    if (Frens::isPsramEnabled())
    {
        ringSize = DISC_BUFFER_PSRAM;
        ring = (uint8_t *)Frens::f_malloc(ringSize);
    }
#endif
    discError = ldstream_open(&disc, &io, (uint32_t)f_size(&discFile), ring, ringSize, DISC_AUDIO_BUFFER, picture,
                              SAMPLES_PER_FIELD);
    if (discError)
    {
        printf("%s: cannot use it (error %d)\n", DISCFILE, discError);
        ldstream_close(&disc);
        f_close(&discAudio);
        f_close(&discFile);
        return false;
    }
    disc.read_chunk = DISC_READ_CHUNK;
    printf("%s: pictures %lu-%lu, %lu keys, %s, read-ahead %lu KB%s\n", DISCFILE,
           (unsigned long)ldstream_first_frame(&disc), (unsigned long)ldstream_last_frame(&disc),
           (unsigned long)disc.hdr.key_count, disc.hdr.tool, (unsigned long)(ringSize >> 10),
           ring ? " in PSRAM" : "");
    return true;
}

#if DL_SD_FAST_HZ
static uint32_t sdSlowHz = 0;
static void sdClockFast()
{
#if defined(SDCARD_SPI)
    // only when the card is on the SPI block; boards with PIO SPI leave it off
    if (spi_get_hw(SDCARD_SPI)->cr1 & SPI_SSPCR1_SSE_BITS)
    {
        sdSlowHz = spi_get_baudrate(SDCARD_SPI);
        spi_set_baudrate(SDCARD_SPI, DL_SD_FAST_HZ);
        printf("SD clock %lu -> %lu Hz\n", (unsigned long)sdSlowHz, (unsigned long)spi_get_baudrate(SDCARD_SPI));
    }
#endif
}

// back to the original clock after repeated checksum errors
static void sdClockCheck()
{
    static uint32_t errors = 0;
    if (sdSlowHz && disc.checksum_errors != errors)
    {
        errors = disc.checksum_errors;
        if (errors >= 3)
        {
            spi_set_baudrate(SDCARD_SPI, sdSlowHz);
            printf("SD: %lu checksum errors, clock back to %lu Hz\n", (unsigned long)errors, (unsigned long)sdSlowHz);
            sdSlowHz = 0;
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Audio: one field of mono samples to whichever sink is active.
//
// The field's 735 samples are made in one go, after the field has run. Pushed
// as a burst into a sink with nothing queued in front of it, the sink would
// run dry just before the next burst arrives: a buzz at the frame rate. So
// the PicoDVI and I2S rings keep a cushion of AUDIO_CUSHION samples queued in
// front of each burst, refilled with silence once when the sink has drained,
// and trimmed by one sample per field when the level drifts more than
// AUDIO_TRIM_BAND away from it. The HSTX data-island queue manages its own
// level and is fed as it is. (As in pico-mooncresta.)
// ---------------------------------------------------------------------------
#define AUDIO_CUSHION 512   // ~12 ms
#define AUDIO_TRIM_BAND 192 // drift tolerated before a field is trimmed

static inline int16_t applyGain(int x, int gainQ8)
{
    int32_t v = (x * gainQ8) >> 8;
    if (v > 32767)
        v = 32767;
    else if (v < -32768)
        v = -32768;
    return (int16_t)v;
}

// The two ring sinks: the I2S DAC (ext) or, on PicoDVI, the HDMI audio ring.
static inline int ringQueued(bool ext)
{
#if EXT_AUDIO_IS_ENABLED
    if (ext)
        return I2S_AUDIO_RING_SIZE - 1 - EXT_AUDIO_GET_FREE();
#endif
#if !HSTX
    return (int)dvi_->getAudioRingBuffer().getFullReadableSize();
#else
    return 0;
#endif
}

static inline void ringPut(bool ext, int16_t s)
{
#if EXT_AUDIO_IS_ENABLED
    if (ext)
    {
        EXT_AUDIO_ENQUEUE_SAMPLE(s, s);
        return;
    }
#endif
#if !HSTX
    auto &ring = dvi_->getAudioRingBuffer();
    if (ring.getWritableSize() == 0)
        return; // full: dropped
    *ring.getWritePointer() = {s, s};
    ring.advanceWritePointer(1);
#endif
}

static void __noinline __not_in_flash_func(pushAudio)(const int16_t *buf, int n)
{
    const bool mute = !settings.flags.audioEnabled;

#if EXT_AUDIO_IS_ENABLED
#if HSTX
    const bool ext = settings.flags.useExtAudio || Frens::isHeadPhoneJackConnected();
#else
    const bool ext = settings.flags.useExtAudio;
#endif
#else
    const bool ext = false;
#endif

#if HSTX
    if (!ext)
    {
        for (int i = 0; i < n; i++)
        {
            int16_t s = mute ? 0 : buf[i];
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter)
                addSampleToVUMeter(s);
#endif
            int16_t g = applyGain(s, DVI_AUDIO_GAIN_Q8);
            hstx_push_audio_sample(g, g);
        }
        return;
    }
#endif

    // the cushion and the drift trim, see above
    const int queued = ringQueued(ext);
    int repeat = 0;
    if (queued < AUDIO_CUSHION / 4)
    {
        for (int i = queued; i < AUDIO_CUSHION; i++)
            ringPut(ext, 0);
    }
    else if (queued > AUDIO_CUSHION + AUDIO_TRIM_BAND)
    {
        n--;
    }
    else if (queued < AUDIO_CUSHION - AUDIO_TRIM_BAND)
    {
        repeat = 1;
    }

    int16_t s = 0;
    for (int i = 0; i < n; i++)
    {
        s = mute ? 0 : buf[i];
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter)
            addSampleToVUMeter(s);
#endif
        ringPut(ext, applyGain(s, ext ? EXT_AUDIO_GAIN_Q8 : DVI_AUDIO_GAIN_Q8));
    }
    if (repeat)
        ringPut(ext, applyGain(s, ext ? EXT_AUDIO_GAIN_Q8 : DVI_AUDIO_GAIN_Q8));
}

// ---------------------------------------------------------------------------
// Video: the laserdisc picture is a 320x240 buffer of palette indices; it is
// copied to the framebuffer through a 256-entry table of display colours when
// the player shows a new picture. That happens right after the frame pace
// returns, at the start of output VBLANK, so the single-buffered framebuffer
// is rewritten ahead of scan-out (the copy takes well under a millisecond).
//
// The scoreboard strip covers the top SB_HEIGHT lines and is opaque, so it is
// redrawn on its own whenever a digit changes. While it is on, those lines of
// the picture are not copied at all.
// ---------------------------------------------------------------------------
static uint16_t lut[256];
static uint32_t lutSerial = 0xffffffff;
static uint32_t shownSerial = 0xffffffff;
static uint32_t ledSerial = 0xffffffff;
static bool screenBlank = false;
static bool repaint = false; // the screen was drawn over (settings menu)
static bool scoreboardShown = false;
static bool fpsShown = false;

static void __noinline __not_in_flash_func(blitLines)(int y0, int y1)
{
    for (int y = y0; y < y1; y++)
    {
        const uint32_t *src = (const uint32_t *)(picture + y * PLD_WIDTH);
        uint16_t *dst = fbLine(y);
        for (int x = 0; x < PLD_WIDTH / 4; x++)
        {
            const uint32_t p = src[x];
            dst[0] = lut[p & 0xff];
            dst[1] = lut[(p >> 8) & 0xff];
            dst[2] = lut[(p >> 16) & 0xff];
            dst[3] = lut[p >> 24];
            dst += 4;
        }
    }
}

static void buildLut()
{
    for (int i = 0; i < 256; i++)
    {
        const uint16_t v = disc.palette[i]; // RGB555
#if HSTX
        lut[i] = v;
#else
        lut[i] = (uint16_t)((((v >> 11) & 0x0f) << 8) | (((v >> 6) & 0x0f) << 4) | ((v >> 1) & 0x0f));
#endif
    }
    lutSerial = disc.palette_serial;
}

// The picture buffer still holds what is on the screen.
static inline bool pictureOnScreen()
{
    return disc.buf_frame != LDS_NONE && disc.pic_serial == shownSerial && lutSerial == disc.palette_serial;
}

static void __noinline __not_in_flash_func(presentFrame)()
{
    const int top = scoreboardOn ? SB_HEIGHT : 0;
    bool drawn = false;

    if (machine.ldp.state == LDP_PARKED)
    {
        if (!screenBlank || repaint)
        {
            screenClear(SCR_BG);
            screenBlank = true;
            drawn = true;
        }
    }
    else if (ldstream_picture_due(&disc, ldp_display_frame(&machine.ldp)))
    {
        if (disc.pic_serial != shownSerial || lutSerial != disc.palette_serial || repaint || screenBlank)
        {
            if (lutSerial != disc.palette_serial)
                buildLut();
            blitLines(top, PLD_HEIGHT);
            shownSerial = disc.pic_serial;
            screenBlank = false;
            drawn = true;
        }
        ldstream_picture_shown(&disc);
        picturesShown++;
    }
    else if (repaint)
    {
        // after the menu: the picture on the screen, if the buffer still has it
        if (pictureOnScreen())
            blitLines(top, PLD_HEIGHT);
        else
            screenClear(SCR_BG);
        drawn = true;
    }
    repaint = false;

    if (scoreboardOn)
    {
        if (drawn || !scoreboardShown || ledSerial != machine.leds.serial)
        {
            sb_draw(&machine.leds, fbLine(0), SCREENWIDTH, HSTX ? SB_RGB555 : SB_RGB444);
            ledSerial = machine.leds.serial;
            fpsShown = false;
        }
        scoreboardShown = true;
    }
    else if (scoreboardShown)
    {
        // switched off: put the top of the picture back
        if (pictureOnScreen() && !screenBlank)
            blitLines(0, SB_HEIGHT);
        else
            for (int y = 0; y < SB_HEIGHT; y++)
                memset(fbLine(y), 0, SCREENWIDTH * sizeof(uint16_t));
        scoreboardShown = false;
    }

    const bool showFps = settings.flags.displayFrameRate;
    if (showFps)
    {
        char s[3] = {(char)('0' + (fps / 10) % 10), (char)('0' + fps % 10), 0};
        screenText(38, 2, s, rgb(255, 255, 255), rgb(0, 0, 0));
        fpsShown = true;
    }
    else if (fpsShown)
    {
        repaint = true; // the next frame paints over the counter
        fpsShown = false;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// One GPIO port's pad in io::GamePadState button bits; see pico-outrun's
// nespadGameBits() for why the pad type is read as "SNES or not".
static uint32_t nespadGameBits(uint16_t n, uint8_t type)
{
    typedef io::GamePadState::Button B;
    uint32_t b = 0;
    if (n & (1u << 2)) b |= B::SELECT;
    if (n & (1u << 3)) b |= B::START;
    if (n & (1u << 4)) b |= B::UP;
    if (n & (1u << 5)) b |= B::DOWN;
    if (n & (1u << 6)) b |= B::LEFT;
    if (n & (1u << 7)) b |= B::RIGHT;
    if (type != NESPAD_TYPE_SNES)
    {
        if (n & (1u << 0)) b |= B::A;
        if (n & (1u << 1)) b |= B::B;
    }
    else
    {
        if (n & (1u << 0)) b |= B::B;
        if (n & (1u << 1)) b |= B::Y;
        if (n & (1u << 8)) b |= B::A;
        if (n & (1u << 9)) b |= B::X;
    }
    return b;
}

// Every pad source merged into one: Dragon's Lair is played by one player at
// a time (two players take turns on the same controls).
static uint32_t readPads(uint16_t wii, uint32_t *pad2)
{
    typedef io::GamePadState::Button B;
    uint32_t b = 0;
    auto &gp = io::getCurrentGamePadState(0);
    if (gp.connected)
        b |= gp.buttons;
    auto &gp2 = io::getCurrentGamePadState(1);
    *pad2 = gp2.connected ? gp2.buttons : 0;
#if NES_PIN_CLK != -1
    b |= nespadGameBits(nespad_states_ext[0], nespad_padtype[0]);
#endif
#if NES_PIN_CLK_1 != -1
    b |= nespadGameBits(nespad_states_ext[1], nespad_padtype[1]);
#endif
    if (wii & (1u << 0)) b |= B::A;
    if (wii & (1u << 1)) b |= B::B;
    if (wii & (1u << 2)) b |= B::SELECT;
    if (wii & (1u << 3)) b |= B::START;
    if (wii & (1u << 4)) b |= B::UP;
    if (wii & (1u << 5)) b |= B::DOWN;
    if (wii & (1u << 6)) b |= B::LEFT;
    if (wii & (1u << 7)) b |= B::RIGHT;
    if (wii & (1u << 8)) b |= B::X;
    if (wii & (1u << 9)) b |= B::Y;
    return b;
}

// Maps the pad onto the cabinet:
//   d-pad       joystick        A, B, X, Y   sword
//   SELECT      coin            START        1 player
//   SELECT+UP   2 players       START on a second USB pad: 2 players
//   SELECT+B    scoreboard on/off
//   START+A     frame rate display on/off
//
// SELECT doubles as the modifier of SELECT+START (settings menu) and the other
// combinations, so the coin is inserted when SELECT is released, and only if
// no other button was pressed while it was held. With the default DIP
// switches a credit takes two coins; one SELECT inserts both. The game counts
// a coin when the switch has been closed for three of its 30 Hz interrupts
// and open again for three, so each coin is held for 14 fields and released
// for 20.
static bool selectUsedAsModifier = false;
static uint32_t prevButtons = 0;
static int coinFields = 0;

#define COIN_ON 14
#define COIN_PERIOD 34
#define COINS_PER_CREDIT 2

// Forget the buttons seen before the settings menu opened: its SELECT+START
// never reaches mapInputs(), so the SELECT release afterwards would otherwise
// insert a coin.
static void resetInputs()
{
    selectUsedAsModifier = false;
    prevButtons = 0;
    coinFields = 0;
}

static uint16_t mapInputs(uint32_t buttons, uint32_t pad2)
{
    typedef io::GamePadState::Button B;
    const uint32_t pushed = buttons & ~prevButtons;

    if ((buttons & B::START) && (pushed & B::A))
        settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
    if ((buttons & B::SELECT) && (pushed & B::B))
        scoreboardOn = !scoreboardOn;

    if (buttons & B::SELECT)
    {
        if (buttons & ~(uint32_t)B::SELECT)
            selectUsedAsModifier = true;
    }
    else if (prevButtons & B::SELECT)
    {
        if (!selectUsedAsModifier && coinFields < 4 * COIN_PERIOD * COINS_PER_CREDIT)
            coinFields += COIN_PERIOD * COINS_PER_CREDIT;
        selectUsedAsModifier = false;
    }
    prevButtons = buttons;

    uint16_t in = 0;
    if (coinFields > 0)
    {
        if ((coinFields - 1) % COIN_PERIOD >= COIN_PERIOD - COIN_ON)
            in |= DL_IN_COIN1;
        coinFields--;
    }
    if (pad2 & B::START)
        in |= DL_IN_START2;
    if (buttons & B::SELECT)
    {
        if (buttons & B::UP)
            in |= DL_IN_START2;
        return in; // a modifier combination, not game input
    }

    if (buttons & B::START)
        in |= DL_IN_START1;
    if (buttons & B::UP)
        in |= DL_IN_UP;
    if (buttons & B::DOWN)
        in |= DL_IN_DOWN;
    if (buttons & B::LEFT)
        in |= DL_IN_LEFT;
    if (buttons & B::RIGHT)
        in |= DL_IN_RIGHT;
    if ((buttons & (B::B | B::X | B::Y)) || ((buttons & B::A) && !(buttons & B::START)))
        in |= DL_IN_SWORD;
    return in;
}

// ---------------------------------------------------------------------------
// Wii Classic controller (I2C), as in pico-mooncresta: started before the
// game and retried about once a second while no pad answers.
// ---------------------------------------------------------------------------
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static void wiipadPoll()
{
    static int frames = 0;
    if (wiipad_is_connected() || ++frames < 60)
        return;
    frames = 0;
    wiipad_begin();
}
#endif

// ---------------------------------------------------------------------------
// Frame pacing, on the display's frame counter (both back-ends; see
// pico-mooncresta for why PicoDVI cannot use PaceFrames60fps()). The wait is
// spent on the laserdisc: reading and decoding pictures and sound, one step at
// a time, while there is time left before the next frame.
//
// SD reads are polled, so a read takes as long as the transfer. Each read is
// given only as many bytes as fit in the time left, at the speed the card has
// shown in the reads so far (bytes per microsecond, Q8, smoothed).
// ---------------------------------------------------------------------------
static uint32_t sdRateQ8 = 256;   // 1 byte/us to start with
static uint32_t missedFrames = 0; // frames the loop was too late for

static void __noinline __not_in_flash_func(paceFrame)(bool init)
{
    static uint32_t last = 0;
    uint32_t now = frameCounter();
    if (!init)
    {
        const bool playing = machine.ldp.state == LDP_PLAYING;
        const bool stream = haveRoms && haveDisc;
        while (now == last)
        {
            const uint32_t t0 = time_us_32();
            const int32_t left = (int32_t)(FRAME_US - STREAM_MARGIN_US) - (int32_t)(t0 - lastVsyncUs);
            if (stream && left > 300)
            {
                uint32_t allow = (uint32_t)(((uint64_t)left * sdRateQ8) >> 8);
                const uint32_t before = disc.bytes_read;
                if (ldstream_step(&disc, ldp_display_frame(&machine.ldp), playing, allow))
                {
                    const uint32_t bytes = disc.bytes_read - before;
                    const uint32_t us = time_us_32() - t0;
                    if (bytes >= 512 && us > 0)
                    {
                        uint32_t rate = (uint32_t)(((uint64_t)bytes << 8) / us);
                        sdRateQ8 = (sdRateQ8 * 7 + rate) / 8;
                    }
                    now = frameCounter();
                    continue;
                }
            }
            __compiler_memory_barrier(); // the counter is advanced by core1
            now = frameCounter();
        }
        if (now - last > 1)
            missedFrames += now - last - 1;
    }
    last = now;
    lastVsyncUs = time_us_32();
}

// ---------------------------------------------------------------------------
// Once per frame
// ---------------------------------------------------------------------------
static void processPerFrame()
{
    paceFrame(false);

    if (haveRoms && haveDisc)
        presentFrame();

    Frens::pollHeadPhoneJack();

#if ENABLE_VU_METER
    // Fruit Jam Button 2 toggles the VU meter, as in the sibling emulators.
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
        turnOffAllLeds();
    }
#endif

    nespad_read_start();
    Frens::blinkLed((frameCounter() >> 5) & 1);
    nespad_read_finish();

    tuh_task();
    uint16_t wii = 0;
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipadPoll();
    wii = wiipad_read(); // boards without the Wii port do not link wiipad at all
#endif

    uint32_t pad2 = 0;
    uint32_t buttons = readPads(wii, &pad2);

    if ((buttons & io::GamePadState::Button::SELECT) && (buttons & io::GamePadState::Button::START))
        showSettings = true;

    if (showSettings)
    {
        showSettings = false;
        resetInputs();
        int rval = showSettingsMenu(true);

        // menu.cpp saves only down the SAVE path; `settings` only ever holds
        // committed values, so writing again can never persist a cancelled edit.
        FrensSettings::savesettings();
        Frens::applyScreenMode(settings.screenMode);
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

        // Without the game data there is nothing to return to. Reboot, so files
        // just copied onto the card in USB drive mode are picked up.
        if (!haveRoms || !haveDisc)
        {
            printf("Rebooting to look for the game data again...\n");
            watchdog_reboot(0, 0, 0);
            while (true)
                tight_loop_contents();
        }
        if (rval == 5) // Reset Game
            dl_reset(&machine);
        repaint = true; // the menu drew over the whole screen
        scoreboardShown = false;
        paceFrame(true);
        return;
    }

    if (!haveRoms || !haveDisc)
        return;

    memset(audioBuf, 0, sizeof(audioBuf));
    dl_run_field(&machine, mapInputs(buttons, pad2), audioBuf, SAMPLES_PER_FIELD);
    if (ldp_audio_on(&machine.ldp))
        ldstream_audio_render(&disc, audioBuf, SAMPLES_PER_FIELD, LD_AUDIO_GAIN_Q8);
    pushAudio(audioBuf, SAMPLES_PER_FIELD);

#if DL_SD_FAST_HZ
    sdClockCheck();
#endif

    const uint64_t now = Frens::time_us();
    if (now - fpsStartUs >= 1000000)
    {
        fps = picturesShown;
        picturesShown = 0;
        fpsStartUs = now;
#ifdef DL_STATS
        printf("pictures/s %lu, decoded %lu, read %lu KB, SD %lu KB/s, seeks %lu, sound gaps %lu, "
               "missed frames %lu, checksum errors %lu\n",
               (unsigned long)fps, (unsigned long)disc.frames_decoded, (unsigned long)(disc.bytes_read >> 10),
               (unsigned long)((uint64_t)sdRateQ8 * 1000000u / (256u * 1024u)), (unsigned long)disc.seeks,
               (unsigned long)disc.audio_gaps, (unsigned long)missedFrames, (unsigned long)disc.checksum_errors);
#endif
    }
}

int main()
{
    Frens::setClocksAndStartStdio(CPUFreqKHz, VREG_VOLTAGE_1_20);

    printf("==========================================================================================\n");
    printf("picoDragonsLair %s\n", SWVERSION);
    printf("Build date: %s %s\n", __DATE__, __TIME__);
    printf("HW_CONFIG=%d  HSTX=%d  CPU freq: %lu kHz\n", HW_CONFIG, HSTX, (unsigned long)(clock_get_hz(clk_sys) / 1000));
    printf("==========================================================================================\n");

    FrensSettings::initSettings(FrensSettings::ARCADE);

    // No ROM is ever selected through the browser; the set is read below.
    char dummyRom[FF_MAX_LFN];
    dummyRom[0] = 0;
    bool sdOk = Frens::initAll(dummyRom, CPUFreqKHz, 0, 0, AUDIOBUFFERSIZE, false, true);

    if (sdOk)
    {
        // loadsettings() inside initAll resets every setting when
        // settings.currentDir does not exist. There is no ROM browser to
        // create it, so make it (FR_EXIST later on is fine) and load again.
        // ROMDIR is made too, so the user sees where the game data goes.
        f_mkdir("/roms");
        f_mkdir("/roms/arcade");
        f_mkdir(ROMDIR);
        FrensSettings::loadsettings();
    }
    // All arcade games share /settings_ARC.dat and with it currentDir. Keep it
    // at /roms/arcade, which every arcade game creates: a game's own folder
    // would make the next game reset its settings when that folder is missing.
    strcpy(settings.currentDir, "/roms/arcade");
    g_settings_visibility = g_settings_visibility_dlair;
    g_available_screen_modes = g_available_screen_modes_dlair;
    if (!g_available_screen_modes[static_cast<int>(settings.screenMode)])
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    Frens::applyScreenMode(settings.screenMode);
    // Apply the saved DAC volume now; otherwise it only takes effect after the
    // settings menu has been opened and closed. No-op without a TLV320.
    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    // after initAll(), so after the DAC: see wiipadPoll()
    if (!wiipad_is_connected())
        wiipad_begin();
#endif

    haveRoms = sdOk && loadRoms();
    haveDisc = sdOk && openDisc();
    if (haveRoms && haveDisc)
    {
#if DL_SD_FAST_HZ
        sdClockFast();
#endif
        dl_init(&machine, &roms, &disc, true, SAMPLERATE);
        machine.ay_level = AY_LEVEL;
        screenClear(SCR_BG);
        screenBlank = true;
    }
    else
    {
        drawErrorScreen(sdOk);
    }
    paceFrame(true);
    fpsStartUs = Frens::time_us();
    while (true)
        processPerFrame();
}
