/*
 * win32_main.c — playable Win32 frontend for the recompilation.
 * Zero dependencies beyond user32/gdi32/winmm.
 *
 *  - pre-game menu rendered NES-style straight into a 256x240 framebuffer
 *    (8x8 bitmap font, palette colors lifted from the game's deck/HUD):
 *    sprite-limit tickbox (authentic flicker vs unlimited), full control
 *    remapping including the save/load-state keys, persisted settings
 *  - video: StretchDIBits, BI_BITFIELDS matching the PPU's 0xAABBGGRR
 *    framebuffer directly, integer 3x window
 *  - audio: waveOut 48kHz mono, 4 rotating buffers; emulation is
 *    AUDIO-CLOCKED (frames run until the APU ring can fill the next buffer)
 *  - save states: F5/F7 by default (remappable), single slot file next to
 *    the exe, frame-boundary snapshots (see runtime/state.c)
 *  - ROM: loaded from "Battletoads Double Dragon (U).nes" next to the exe —
 *    assets stay on the user's disk, nothing copyrighted ships in the exe
 */
#include "nes.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE 3
#define AUDIO_RATE 48000
#define AUDIO_BUF_SAMPLES 1024
#define AUDIO_NBUF 4

#define SETTINGS_FILE "recomp-settings.txt"
#define STATE_FILE "recomp-state.sav"

/* ── remappable actions ──────────────────────────────────────────────────── */
enum {
    ACT_A, ACT_B, ACT_START, ACT_SELECT,
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_SAVE, ACT_LOAD,
    ACT_COUNT,
};
static const char *ACT_NAMES[ACT_COUNT] = {
    "JUMP  A", "ATTACK  B", "START", "SELECT",
    "UP", "DOWN", "LEFT", "RIGHT",
    "SAVE STATE", "LOAD STATE",
};
static const int ACT_DEFAULTS[ACT_COUNT] = {
    'Z', 'X', VK_RETURN, VK_RSHIFT,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    VK_F5, VK_F7,
};
static int g_keymap[ACT_COUNT];
static int g_flicker_limit = 1; /* 1 = authentic 8-sprite limit */

static HWND g_wnd;
static BITMAPV4HEADER g_bmi;
static bool g_running = true;
static int g_last_vk = 0; /* set by WM_KEYDOWN, consumed by the menu */

static HWAVEOUT g_wo;
static WAVEHDR g_hdr[AUDIO_NBUF];
static int16_t g_abuf[AUDIO_NBUF][AUDIO_BUF_SAMPLES];
static bool g_audio_ok = false;

/* ── NES-style menu framebuffer + font ───────────────────────────────────── */
static uint32_t g_menu_fb[256 * 240];

/* palette colors from the game (packed 0xAABBGGRR like the PPU fb):
 * deck cyan $2C, HUD gold $28, white $30, dark blue $01, grey $00 */
#define COL_BG    0xff100800u
#define COL_PANEL 0xff200e00u
#define COL_EDGE  0xff882a00u  /* dark blue $01 */
#define COL_TEXT  0xffdecd48u  /* cyan $2C */
#define COL_GOLD  0xff00bebcu  /* gold $28 */
#define COL_WHITE 0xfffffeffu  /* white $30 */
#define COL_GREY  0xff666666u  /* grey $00 */

/* 8x8 font, rows top-down, bit7 = leftmost pixel */
static const char FONT_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:.->[]&/+!?";
static const uint8_t FONT[][8] = {
    {0,0,0,0,0,0,0,0},                                          /* space */
    {0x38,0x44,0x44,0x7c,0x44,0x44,0x44,0}, {0x78,0x44,0x44,0x78,0x44,0x44,0x78,0},
    {0x38,0x44,0x40,0x40,0x40,0x44,0x38,0}, {0x78,0x44,0x44,0x44,0x44,0x44,0x78,0},
    {0x7c,0x40,0x40,0x78,0x40,0x40,0x7c,0}, {0x7c,0x40,0x40,0x78,0x40,0x40,0x40,0},
    {0x38,0x44,0x40,0x4c,0x44,0x44,0x3c,0}, {0x44,0x44,0x44,0x7c,0x44,0x44,0x44,0},
    {0x38,0x10,0x10,0x10,0x10,0x10,0x38,0}, {0x1c,0x08,0x08,0x08,0x08,0x48,0x30,0},
    {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0}, {0x40,0x40,0x40,0x40,0x40,0x40,0x7c,0},
    {0x44,0x6c,0x54,0x54,0x44,0x44,0x44,0}, {0x44,0x64,0x54,0x4c,0x44,0x44,0x44,0},
    {0x38,0x44,0x44,0x44,0x44,0x44,0x38,0}, {0x78,0x44,0x44,0x78,0x40,0x40,0x40,0},
    {0x38,0x44,0x44,0x44,0x54,0x48,0x34,0}, {0x78,0x44,0x44,0x78,0x50,0x48,0x44,0},
    {0x3c,0x40,0x40,0x38,0x04,0x04,0x78,0}, {0x7c,0x10,0x10,0x10,0x10,0x10,0x10,0},
    {0x44,0x44,0x44,0x44,0x44,0x44,0x38,0}, {0x44,0x44,0x44,0x44,0x44,0x28,0x10,0},
    {0x44,0x44,0x44,0x54,0x54,0x6c,0x44,0}, {0x44,0x44,0x28,0x10,0x28,0x44,0x44,0},
    {0x44,0x44,0x28,0x10,0x10,0x10,0x10,0}, {0x7c,0x04,0x08,0x10,0x20,0x40,0x7c,0},
    {0x38,0x44,0x4c,0x54,0x64,0x44,0x38,0}, {0x10,0x30,0x10,0x10,0x10,0x10,0x38,0},
    {0x38,0x44,0x04,0x18,0x20,0x40,0x7c,0}, {0x38,0x44,0x04,0x18,0x04,0x44,0x38,0},
    {0x08,0x18,0x28,0x48,0x7c,0x08,0x08,0}, {0x7c,0x40,0x78,0x04,0x04,0x44,0x38,0},
    {0x38,0x44,0x40,0x78,0x44,0x44,0x38,0}, {0x7c,0x04,0x08,0x10,0x20,0x20,0x20,0},
    {0x38,0x44,0x44,0x38,0x44,0x44,0x38,0}, {0x38,0x44,0x44,0x3c,0x04,0x44,0x38,0},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0},                     /* : */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0},                     /* . */
    {0x00,0x00,0x00,0x7c,0x00,0x00,0x00,0},                     /* - */
    {0x40,0x20,0x10,0x08,0x10,0x20,0x40,0},                     /* > */
    {0x38,0x20,0x20,0x20,0x20,0x20,0x38,0},                     /* [ */
    {0x38,0x08,0x08,0x08,0x08,0x08,0x38,0},                     /* ] */
    {0x30,0x48,0x48,0x30,0x4a,0x44,0x3a,0},                     /* & */
    {0x04,0x04,0x08,0x10,0x20,0x40,0x40,0},                     /* / */
    {0x00,0x10,0x10,0x7c,0x10,0x10,0x00,0},                     /* + */
    {0x10,0x10,0x10,0x10,0x10,0x00,0x10,0},                     /* ! */
    {0x38,0x44,0x04,0x08,0x10,0x00,0x10,0},                     /* ? */
};

static void mfill(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j += 1) {
        if (j < 0 || j >= 240) continue;
        for (int i = x; i < x + w; i += 1) {
            if (i < 0 || i >= 256) continue;
            g_menu_fb[j * 256 + i] = c;
        }
    }
}

static void mtext(int x, int y, const char *s, uint32_t c) {
    for (; *s; s += 1, x += 8) {
        const char *p = strchr(FONT_CHARS, (*s >= 'a' && *s <= 'z') ? *s - 32 : *s);
        if (!p) continue;
        const uint8_t *g = FONT[p - FONT_CHARS];
        for (int r = 0; r < 8; r += 1) {
            uint8_t bits = g[r];
            for (int b = 0; b < 8; b += 1) {
                if (bits & (0x80 >> b)) {
                    int px = x + b, py = y + r;
                    if (px >= 0 && px < 256 && py >= 0 && py < 240)
                        g_menu_fb[py * 256 + px] = c;
                }
            }
        }
    }
}

static void mtext_center(int y, const char *s, uint32_t c) {
    mtext((256 - (int)strlen(s) * 8) / 2, y, s, c);
}

static void menu_chrome(void) {
    mfill(0, 0, 256, 240, COL_BG);
    mfill(8, 8, 240, 224, COL_PANEL);
    mfill(8, 8, 240, 2, COL_EDGE); mfill(8, 230, 240, 2, COL_EDGE);
    mfill(8, 8, 2, 224, COL_EDGE); mfill(246, 8, 2, 224, COL_EDGE);
    mtext_center(24, "BATTLETOADS &", COL_GOLD);
    mtext_center(36, "DOUBLE DRAGON", COL_GOLD);
    mtext_center(52, "RECOMPILED", COL_TEXT);
    mfill(48, 64, 160, 1, COL_EDGE);
}

/* ── key names ───────────────────────────────────────────────────────────── */
static void vk_name(int vk, char *out, int cap) {
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LONG l = (LONG)(sc << 16);
    switch (vk) { /* extended keys need bit 24 for the right name */
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
        l |= 1 << 24;
        break;
    }
    if (GetKeyNameTextA(l, out, cap) == 0) snprintf(out, (size_t)cap, "VK%02X", vk);
    for (char *p = out; *p; p += 1) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

/* ── settings persistence ────────────────────────────────────────────────── */
static void settings_load(void) {
    for (int i = 0; i < ACT_COUNT; i += 1) g_keymap[i] = ACT_DEFAULTS[i];
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char k[64];
    int v;
    while (fscanf(f, "%63[^=]=%d\n", k, &v) == 2) {
        if (strcmp(k, "flicker_limit") == 0) g_flicker_limit = v ? 1 : 0;
        else if (strncmp(k, "key", 3) == 0) {
            int i = atoi(k + 3);
            if (i >= 0 && i < ACT_COUNT && v > 0 && v < 256) g_keymap[i] = v;
        }
    }
    fclose(f);
}

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "flicker_limit=%d\n", g_flicker_limit);
    for (int i = 0; i < ACT_COUNT; i += 1) fprintf(f, "key%d=%d\n", i, g_keymap[i]);
    fclose(f);
}

/* ── window plumbing ─────────────────────────────────────────────────────── */
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (!(l & (1 << 30))) g_last_vk = (int)w; /* ignore auto-repeat for capture */
        else g_last_vk = (int)w;                   /* keep repeat for nav feel */
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static void present_buf(const void *buf) {
    HDC dc = GetDC(g_wnd);
    StretchDIBits(dc, 0, 0, 256 * SCALE, 240 * SCALE, 0, 0, 256, 240,
                  buf, (const BITMAPINFO *)&g_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_wnd, dc);
}

static void pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ── the menu ────────────────────────────────────────────────────────────── */
enum { MM_START, MM_FLICKER, MM_CONTROLS, MM_QUIT, MM_COUNT };

static void draw_main_menu(int sel, int tick) {
    menu_chrome();
    const int y0 = 96, dy = 18;
    const char *items[MM_COUNT] = { "START GAME", NULL, "CONTROLS", "QUIT" };
    char flick[48];
    snprintf(flick, sizeof flick, "SPRITE LIMIT [%s]", g_flicker_limit ? "X" : " ");
    items[MM_FLICKER] = flick;
    for (int i = 0; i < MM_COUNT; i += 1) {
        uint32_t c = i == sel ? COL_GOLD : COL_TEXT;
        if (i == sel && (tick / 16) % 2 == 0) mtext(56, y0 + i * dy, ">", COL_WHITE);
        mtext(72, y0 + i * dy, items[i], c);
    }
    mtext_center(y0 + MM_COUNT * dy + 8,
                 g_flicker_limit ? "SPRITES: AUTHENTIC FLICKER" : "SPRITES: NO FLICKER",
                 COL_GREY);
    mtext_center(202, "ARROWS: MOVE  Z/ENTER: SELECT", COL_GREY);
    mtext_center(214, "F5 SAVE  F7 LOAD  ESC QUIT", COL_GREY);
}

static void draw_ctrl_menu(int sel, int capturing, int tick) {
    menu_chrome();
    mtext_center(70, "CONTROLS - PLAYER 1", COL_WHITE);
    const int y0 = 84, dy = 10;
    for (int i = 0; i < ACT_COUNT; i += 1) {
        uint32_t c = i == sel ? COL_GOLD : COL_TEXT;
        if (i == sel && (tick / 16) % 2 == 0) mtext(20, y0 + i * dy, ">", COL_WHITE);
        mtext(32, y0 + i * dy, ACT_NAMES[i], c);
        char kn[32];
        if (capturing && i == sel) {
            if ((tick / 8) % 2 == 0) mtext(150, y0 + i * dy, "PRESS A KEY", COL_WHITE);
        } else {
            vk_name(g_keymap[i], kn, sizeof kn);
            kn[11] = 0;
            mtext(150, y0 + i * dy, kn, i == sel ? COL_WHITE : COL_GREY);
        }
    }
    uint32_t c1 = sel == ACT_COUNT ? COL_GOLD : COL_TEXT;
    uint32_t c2 = sel == ACT_COUNT + 1 ? COL_GOLD : COL_TEXT;
    if (sel == ACT_COUNT && (tick / 16) % 2 == 0) mtext(20, y0 + ACT_COUNT * dy + 4, ">", COL_WHITE);
    mtext(32, y0 + ACT_COUNT * dy + 4, "RESET DEFAULTS", c1);
    if (sel == ACT_COUNT + 1 && (tick / 16) % 2 == 0) mtext(20, y0 + (ACT_COUNT + 1) * dy + 4, ">", COL_WHITE);
    mtext(32, y0 + (ACT_COUNT + 1) * dy + 4, "BACK", c2);
    mtext_center(216, "P2 IS FIXED: WASD + F/G", COL_GREY);
}

/* returns 1 to start the game, 0 to quit */
static int menu_loop(void) {
    int screen = 0; /* 0 = main, 1 = controls */
    int sel = 0, csel = 0, capturing = 0, tick = 0;
    g_last_vk = 0;
    while (g_running) {
        pump();
        if (!g_running) return 0;
        int vk = g_last_vk;
        g_last_vk = 0;

        if (screen == 0) {
            if (vk == VK_ESCAPE) return 0;
            if (vk == VK_UP) sel = (sel + MM_COUNT - 1) % MM_COUNT;
            if (vk == VK_DOWN) sel = (sel + 1) % MM_COUNT;
            if ((vk == VK_LEFT || vk == VK_RIGHT) && sel == MM_FLICKER)
                g_flicker_limit = !g_flicker_limit;
            if (vk == VK_RETURN || vk == 'Z') {
                if (sel == MM_START) return 1;
                if (sel == MM_FLICKER) g_flicker_limit = !g_flicker_limit;
                if (sel == MM_CONTROLS) { screen = 1; csel = 0; }
                if (sel == MM_QUIT) return 0;
            }
            draw_main_menu(sel, tick);
        } else {
            const int NROWS = ACT_COUNT + 2;
            if (capturing) {
                if (vk == VK_ESCAPE) capturing = 0;
                else if (vk != 0) {
                    g_keymap[csel] = vk;
                    capturing = 0;
                    settings_save();
                }
            } else {
                if (vk == VK_ESCAPE) { screen = 0; settings_save(); }
                if (vk == VK_UP) csel = (csel + NROWS - 1) % NROWS;
                if (vk == VK_DOWN) csel = (csel + 1) % NROWS;
                if (vk == VK_RETURN || vk == 'Z') {
                    if (csel < ACT_COUNT) capturing = 1;
                    else if (csel == ACT_COUNT) {
                        for (int i = 0; i < ACT_COUNT; i += 1) g_keymap[i] = ACT_DEFAULTS[i];
                        settings_save();
                    } else { screen = 0; settings_save(); }
                }
            }
            draw_ctrl_menu(csel, capturing, tick);
        }
        present_buf(g_menu_fb);
        tick += 1;
        Sleep(16);
    }
    return 0;
}

/* ── input + game loop ───────────────────────────────────────────────────── */
static inline bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static uint8_t poll_pad0(void) {
    uint8_t m = 0;
    if (key_down(g_keymap[ACT_A])) m |= BTN_A;
    if (key_down(g_keymap[ACT_B])) m |= BTN_B;
    if (key_down(g_keymap[ACT_SELECT])) m |= BTN_SELECT;
    if (key_down(g_keymap[ACT_START])) m |= BTN_START;
    if (key_down(g_keymap[ACT_UP])) m |= BTN_UP;
    if (key_down(g_keymap[ACT_DOWN])) m |= BTN_DOWN;
    if (key_down(g_keymap[ACT_LEFT])) m |= BTN_LEFT;
    if (key_down(g_keymap[ACT_RIGHT])) m |= BTN_RIGHT;
    return m;
}

static uint8_t poll_pad1(void) {
    uint8_t m = 0;
    if (key_down('F')) m |= BTN_A;
    if (key_down('G')) m |= BTN_B;
    if (key_down('W')) m |= BTN_UP;
    if (key_down('S')) m |= BTN_DOWN;
    if (key_down('A')) m |= BTN_LEFT;
    if (key_down('D')) m |= BTN_RIGHT;
    return m;
}

static void flash_osd(const char *msg); /* fwd */

static void run_one_frame(void) {
    static bool prev_save = false, prev_load = false;
    bool focused = GetForegroundWindow() == g_wnd;
    if (focused) {
        nes_set_buttons(0, poll_pad0());
        nes_set_buttons(1, poll_pad1());
        bool s = key_down(g_keymap[ACT_SAVE]);
        bool l = key_down(g_keymap[ACT_LOAD]);
        if (s && !prev_save) {
            flash_osd(nes_save_state(STATE_FILE) == 0 ? "STATE SAVED" : "SAVE FAILED");
        }
        if (l && !prev_load) {
            flash_osd(nes_load_state(STATE_FILE) == 0 ? "STATE LOADED" : "NO STATE");
        }
        prev_save = s;
        prev_load = l;
    } else {
        nes_set_buttons(0, 0);
        nes_set_buttons(1, 0);
    }
    nes_run_frame();
}

/* tiny on-screen flash for save/load feedback, drawn over the frame */
static char g_osd[24];
static int g_osd_timer = 0;
static void flash_osd(const char *msg) {
    snprintf(g_osd, sizeof g_osd, "%s", msg);
    g_osd_timer = 90;
}

static uint32_t g_present_fb[256 * 240];
static void present_game(void) {
    const uint32_t *src = PPU.framebuffer;
    if (g_osd_timer > 0) {
        memcpy(g_present_fb, PPU.framebuffer, sizeof g_present_fb);
        /* draw the OSD text into the copy using the menu font path */
        uint32_t *save = g_menu_fb;
        (void)save;
        int len = (int)strlen(g_osd);
        int x = 256 - len * 8 - 6, y = 6;
        for (int j = y - 2; j < y + 10; j += 1)
            for (int i = x - 4; i < 254; i += 1)
                g_present_fb[j * 256 + i] = COL_BG;
        /* mtext writes to g_menu_fb; temporarily alias */
        memcpy(g_menu_fb, g_present_fb, sizeof g_present_fb);
        mtext(x, y, g_osd, COL_GOLD);
        memcpy(g_present_fb, g_menu_fb, sizeof g_present_fb);
        src = g_present_fb;
        g_osd_timer -= 1;
    }
    present_buf(src);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* hidden: --menushot <raw.out> renders the main menu once and exits
     * (used by the build's own visual check; no window needed) */
    if (argc >= 3 && strcmp(argv[1], "--menushot") == 0) {
        settings_load();
        draw_main_menu(0, 0);
        FILE *mf = fopen(argv[2], "wb");
        if (!mf) return 1;
        fwrite(g_menu_fb, 1, sizeof g_menu_fb, mf);
        fclose(mf);
        draw_ctrl_menu(8, 0, 0);
        char p2[512];
        snprintf(p2, sizeof p2, "%s.ctrl", argv[2]);
        mf = fopen(p2, "wb");
        if (mf) { fwrite(g_menu_fb, 1, sizeof g_menu_fb, mf); fclose(mf); }
        return 0;
    }

    /* ── locate + load the ROM (exe dir first, then CWD) ─────────────────── */
    char rom_path[MAX_PATH * 2];
    const char *rom_name = "Battletoads Double Dragon (U).nes";
    if (argc > 1) {
        snprintf(rom_path, sizeof rom_path, "%s", argv[1]);
    } else {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(rom_path, sizeof rom_path, "%s\\%s", exe, rom_name);
        FILE *probe = fopen(rom_path, "rb");
        if (probe) fclose(probe);
        else snprintf(rom_path, sizeof rom_path, "%s", rom_name);
    }
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        MessageBoxA(NULL,
            "Could not find 'Battletoads Double Dragon (U).nes'.\n"
            "Put the exe next to your ROM (assets load from your disk).",
            "Battletoads Recomp", MB_ICONERROR);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc((size_t)len);
    if (fread(rom, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 1; }
    fclose(f);
    if (nes_init(rom, (size_t)len, AUDIO_RATE) != 0) {
        MessageBoxA(NULL, "ROM load failed (not iNES / unsupported mapper).",
                    "Battletoads Recomp", MB_ICONERROR);
        return 1;
    }
    free(rom);

    settings_load();

    /* ── window ──────────────────────────────────────────────────────────── */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "BtddRecomp";
    RegisterClassA(&wc);
    RECT r = { 0, 0, 256 * SCALE, 240 * SCALE };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowA("BtddRecomp",
        recomp_enabled
            ? "Battletoads & Double Dragon - RECOMPILED (native)"
            : "Battletoads & Double Dragon - recomp (interpreter)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, NULL, wc.hInstance, NULL);
    ShowWindow(g_wnd, SW_SHOW);

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bV4Size = sizeof g_bmi;
    g_bmi.bV4Width = 256;
    g_bmi.bV4Height = -240; /* top-down */
    g_bmi.bV4Planes = 1;
    g_bmi.bV4BitCount = 32;
    g_bmi.bV4V4Compression = BI_BITFIELDS;
    g_bmi.bV4RedMask = 0x000000ff;   /* PPU packs 0xAABBGGRR */
    g_bmi.bV4GreenMask = 0x0000ff00;
    g_bmi.bV4BlueMask = 0x00ff0000;

    /* ── pre-game menu ───────────────────────────────────────────────────── */
    if (!menu_loop()) return 0;
    settings_save();
    ppu_no_flicker = g_flicker_limit ? 0 : 1;

    /* ── audio ───────────────────────────────────────────────────────────── */
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = AUDIO_RATE * 2;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
        g_audio_ok = true;
        for (int i = 0; i < AUDIO_NBUF; i += 1) {
            memset(&g_hdr[i], 0, sizeof g_hdr[i]);
            g_hdr[i].lpData = (LPSTR)g_abuf[i];
            g_hdr[i].dwBufferLength = AUDIO_BUF_SAMPLES * 2;
            waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
            g_hdr[i].dwFlags |= WHDR_DONE;
        }
    }

    LARGE_INTEGER qpf, last;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&last);
    double acc = 0;
    const double FRAME_S = 1.0 / 60.0988;
    float fbuf[AUDIO_BUF_SAMPLES];

    while (g_running) {
        pump();
        if (!g_running) break;
        if (g_last_vk == VK_ESCAPE) break;

        if (g_audio_ok) {
            bool filled = false;
            for (int i = 0; i < AUDIO_NBUF; i += 1) {
                if (!(g_hdr[i].dwFlags & WHDR_DONE)) continue;
                int guard = 8;
                while (apu_available_samples() < AUDIO_BUF_SAMPLES && guard-- > 0) {
                    run_one_frame();
                    present_game();
                }
                int got = apu_read_samples(fbuf, AUDIO_BUF_SAMPLES);
                for (int s = 0; s < AUDIO_BUF_SAMPLES; s += 1) {
                    float v = s < got ? fbuf[s] : 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    g_abuf[i][s] = (int16_t)(v * 30000.0f);
                }
                g_hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
                filled = true;
            }
            if (!filled) Sleep(1);
        } else {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            acc += (double)(now.QuadPart - last.QuadPart) / qpf.QuadPart;
            last = now;
            if (acc > 0.25) acc = 0.25;
            int steps = 0;
            while (acc >= FRAME_S && steps < 5) {
                run_one_frame();
                present_game();
                acc -= FRAME_S;
                steps += 1;
            }
            if (steps == 0) Sleep(1);
        }
    }

    if (g_audio_ok) {
        waveOutReset(g_wo);
        for (int i = 0; i < AUDIO_NBUF; i += 1) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        waveOutClose(g_wo);
    }
    return 0;
}
