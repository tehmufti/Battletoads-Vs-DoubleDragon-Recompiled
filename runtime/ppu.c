/*
 * ppu.c — dot-accurate NES PPU (2C02 NTSC), ported 1:1 from src/nes/ppu.js.
 * Loopy v/t/x/w, background shifters on the hardware schedule, per-scanline
 * sprite evaluation with the 8-sprite limit, true per-pixel sprite-0 hit,
 * 8x16 sprites, vblank/NMI with the $2002 race, odd-frame dot skip, $2007
 * read buffer, forced-blank backdrop hack.
 */
#include "nes.h"
#include <string.h>
#include <math.h>

Ppu PPU;
int ppu_no_flicker = 0;

static const uint8_t BASE_PALETTE[64 * 3] = {
    0x66,0x66,0x66, 0x00,0x2A,0x88, 0x14,0x12,0xA7, 0x3B,0x00,0xA4,
    0x5C,0x00,0x7E, 0x6E,0x00,0x40, 0x6C,0x06,0x00, 0x56,0x1D,0x00,
    0x33,0x35,0x00, 0x0B,0x48,0x00, 0x00,0x52,0x00, 0x00,0x4F,0x08,
    0x00,0x40,0x4D, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
    0xAD,0xAD,0xAD, 0x15,0x5F,0xD9, 0x42,0x40,0xFF, 0x75,0x27,0xFE,
    0xA0,0x1A,0xCC, 0xB7,0x1E,0x7B, 0xB5,0x31,0x20, 0x99,0x4E,0x00,
    0x6B,0x6D,0x00, 0x38,0x87,0x00, 0x0C,0x93,0x00, 0x00,0x8F,0x32,
    0x00,0x7C,0x8D, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
    0xFF,0xFE,0xFF, 0x64,0xB0,0xFF, 0x92,0x90,0xFF, 0xC6,0x76,0xFF,
    0xF3,0x6A,0xFF, 0xFE,0x6E,0xCC, 0xFE,0x81,0x70, 0xEA,0x9E,0x22,
    0xBC,0xBE,0x00, 0x88,0xD8,0x00, 0x5C,0xE4,0x30, 0x45,0xE0,0x82,
    0x48,0xCD,0xDE, 0x4F,0x4F,0x4F, 0x00,0x00,0x00, 0x00,0x00,0x00,
    0xFF,0xFE,0xFF, 0xC0,0xDF,0xFF, 0xD3,0xD2,0xFF, 0xE8,0xC8,0xFF,
    0xFB,0xC2,0xFF, 0xFE,0xC4,0xEA, 0xFE,0xCC,0xC5, 0xF7,0xD8,0xA5,
    0xE4,0xE5,0x94, 0xCF,0xEF,0x96, 0xBD,0xF4,0xAB, 0xB3,0xF3,0xCC,
    0xB5,0xEB,0xF2, 0xB8,0xB8,0xB8, 0x00,0x00,0x00, 0x00,0x00,0x00,
};

static uint32_t RGB_LUT[512];
static bool lut_built = false;

/* Matches the JS build exactly: JS Math.round is round-half-up on positive
 * values; use floor(x + 0.5) rather than C's round-half-away semantics
 * (identical for these inputs, but be explicit). */
static void build_rgb_lut(void) {
    for (int emph = 0; emph < 8; emph += 1) {
        int eR = emph & 1, eG = emph & 2, eB = emph & 4;
        for (int c = 0; c < 64; c += 1) {
            double r = BASE_PALETTE[c * 3];
            double g = BASE_PALETTE[c * 3 + 1];
            double b = BASE_PALETTE[c * 3 + 2];
            if (emph != 0) {
                const double att = 0.746;
                if (!eR) r *= att;
                if (!eG) g *= att;
                if (!eB) b *= att;
            }
            int ri = (int)floor(r + 0.5); if (ri > 255) ri = 255;
            int gi = (int)floor(g + 0.5); if (gi > 255) gi = 255;
            int bi = (int)floor(b + 0.5); if (bi > 255) bi = 255;
            RGB_LUT[(emph << 6) | c] =
                (0xffu << 24) | ((uint32_t)bi << 16) | ((uint32_t)gi << 8) | (uint32_t)ri;
        }
    }
    lut_built = true;
}

void ppu_reset(void) {
    if (!lut_built) build_rgb_lut();
    PPU.ctrl = 0; PPU.mask = 0; PPU.status = 0;
    PPU.oam_addr = 0;
    PPU.v = 0; PPU.t = 0; PPU.x = 0; PPU.w = 0;
    PPU.read_buffer = 0;
    PPU.scanline = 261; PPU.dot = 0;
    PPU.frame = 0; PPU.odd_frame = false;
    PPU.nmi_suppress = false;
    PPU.bg_shift_lo = PPU.bg_shift_hi = 0;
    PPU.at_shift_lo = PPU.at_shift_hi = 0;
    memset(PPU.sp_color, 0, sizeof PPU.sp_color);
}

static inline bool rendering_enabled(void) { return (PPU.mask & 0x18) != 0; }

void ppu_update_nmi_line(void) {
    cpu_set_nmi_line((PPU.ctrl & 0x80) != 0 && (PPU.status & 0x80) != 0);
}

/* ── Memory map ──────────────────────────────────────────────────────────── */
static inline int nt_index(uint16_t addr) {
    switch (PPU.mirror) {
    case MIRROR_HORIZONTAL: return ((addr >> 1) & 0x400) | (addr & 0x3ff);
    case MIRROR_VERTICAL: return addr & 0x7ff;
    case MIRROR_SINGLE0: return addr & 0x3ff;
    default: return 0x400 | (addr & 0x3ff);
    }
}

static uint8_t ppu_read(uint16_t addr) {
    addr &= 0x3fff;
    if (addr < 0x2000) return PPU.chr[addr];
    if (addr < 0x3f00) return PPU.vram[nt_index(addr)];
    int p = addr & 0x1f;
    if (p >= 0x10 && (p & 0x03) == 0) p -= 0x10;
    return PPU.palette[p];
}

static void ppu_write(uint16_t addr, uint8_t value) {
    addr &= 0x3fff;
    if (addr < 0x2000) {
        if (PPU.chr_writable) PPU.chr[addr] = value;
        return;
    }
    if (addr < 0x3f00) {
        PPU.vram[nt_index(addr)] = value;
        return;
    }
    int p = addr & 0x1f;
    if (p >= 0x10 && (p & 0x03) == 0) p -= 0x10;
    PPU.palette[p] = value;
}

/* ── CPU-visible registers ───────────────────────────────────────────────── */
uint8_t ppu_read_reg(int reg) {
    switch (reg) {
    case 2: {
        uint8_t value = (uint8_t)((PPU.status & 0xe0) | (PPU.open_bus & 0x1f));
        if (PPU.scanline == 241 && PPU.dot <= 2) {
            value &= 0x7f;
            PPU.nmi_suppress = true;
            PPU.status &= 0x7f;
            ppu_update_nmi_line();
        }
        PPU.status &= 0x7f;
        ppu_update_nmi_line();
        PPU.w = 0;
        PPU.open_bus = value;
        return value;
    }
    case 4: {
        uint8_t value = PPU.oam[PPU.oam_addr];
        PPU.open_bus = value;
        return value;
    }
    case 7: {
        uint16_t addr = PPU.v & 0x3fff;
        uint8_t value;
        if (addr >= 0x3f00) {
            value = ppu_read(addr) & ((PPU.mask & 0x01) ? 0x30 : 0x3f);
            PPU.read_buffer = PPU.vram[nt_index(addr)];
        } else {
            value = PPU.read_buffer;
            PPU.read_buffer = ppu_read(addr);
        }
        PPU.v = (PPU.v + ((PPU.ctrl & 0x04) ? 32 : 1)) & 0x7fff;
        PPU.open_bus = value;
        return value;
    }
    default:
        return PPU.open_bus;
    }
}

void ppu_write_reg(int reg, uint8_t value) {
    PPU.open_bus = value;
    switch (reg) {
    case 0:
        PPU.ctrl = value;
        PPU.t = (uint16_t)((PPU.t & 0x73ff) | ((value & 0x03) << 10));
        ppu_update_nmi_line();
        return;
    case 1: PPU.mask = value; return;
    case 2: return;
    case 3: PPU.oam_addr = value; return;
    case 4:
        PPU.oam[PPU.oam_addr] = value;
        PPU.oam_addr += 1;
        return;
    case 5:
        if (PPU.w == 0) {
            PPU.t = (uint16_t)((PPU.t & 0x7fe0) | (value >> 3));
            PPU.x = value & 0x07;
            PPU.w = 1;
        } else {
            PPU.t = (uint16_t)((PPU.t & 0x0c1f) | ((value & 0x07) << 12) | ((value & 0xf8) << 2));
            PPU.w = 0;
        }
        return;
    case 6:
        if (PPU.w == 0) {
            PPU.t = (uint16_t)((PPU.t & 0x00ff) | ((value & 0x3f) << 8));
            PPU.w = 1;
        } else {
            PPU.t = (uint16_t)((PPU.t & 0x7f00) | value);
            PPU.v = PPU.t;
            PPU.w = 0;
        }
        return;
    case 7:
        ppu_write(PPU.v & 0x3fff, value);
        PPU.v = (PPU.v + ((PPU.ctrl & 0x04) ? 32 : 1)) & 0x7fff;
        return;
    }
}

/* ── Internal helpers ────────────────────────────────────────────────────── */
static inline void inc_horiz(void) {
    if ((PPU.v & 0x001f) == 31) PPU.v = (PPU.v & ~0x001f) ^ 0x0400;
    else PPU.v += 1;
}

static inline void inc_vert(void) {
    if ((PPU.v & 0x7000) != 0x7000) {
        PPU.v += 0x1000;
    } else {
        PPU.v &= ~0x7000;
        int y = (PPU.v & 0x03e0) >> 5;
        if (y == 29) { y = 0; PPU.v ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else { y += 1; }
        PPU.v = (uint16_t)((PPU.v & ~0x03e0) | (y << 5));
    }
}

static inline void copy_horiz(void) { PPU.v = (uint16_t)((PPU.v & ~0x041f) | (PPU.t & 0x041f)); }
static inline void copy_vert(void) { PPU.v = (uint16_t)((PPU.v & ~0x7be0) | (PPU.t & 0x7be0)); }

static inline void reload_shifters(void) {
    PPU.bg_shift_lo = (uint16_t)((PPU.bg_shift_lo & 0xff00) | PPU.bg_lo_byte);
    PPU.bg_shift_hi = (uint16_t)((PPU.bg_shift_hi & 0xff00) | PPU.bg_hi_byte);
    PPU.at_latch_lo = PPU.at_byte & 1;
    PPU.at_latch_hi = (PPU.at_byte >> 1) & 1;
}

static inline void shift_bg(void) {
    PPU.bg_shift_lo <<= 1;
    PPU.bg_shift_hi <<= 1;
    PPU.at_shift_lo = (uint8_t)((PPU.at_shift_lo << 1) | PPU.at_latch_lo);
    PPU.at_shift_hi = (uint8_t)((PPU.at_shift_hi << 1) | PPU.at_latch_hi);
}

static void fetch_step(int phase) {
    switch (phase) {
    case 0:
        PPU.nt_byte = ppu_read(0x2000 | (PPU.v & 0x0fff));
        break;
    case 2: {
        uint8_t at = ppu_read((uint16_t)(0x23c0 | (PPU.v & 0x0c00) | ((PPU.v >> 4) & 0x38) | ((PPU.v >> 2) & 0x07)));
        int shift = ((PPU.v >> 4) & 4) | (PPU.v & 2);
        PPU.at_byte = (at >> shift) & 3;
        break;
    }
    case 4: {
        int base = ((PPU.ctrl & 0x10) << 8) + PPU.nt_byte * 16 + (PPU.v >> 12);
        PPU.bg_lo_byte = PPU.chr[base];
        break;
    }
    case 6: {
        int base = ((PPU.ctrl & 0x10) << 8) + PPU.nt_byte * 16 + (PPU.v >> 12) + 8;
        PPU.bg_hi_byte = PPU.chr[base];
        break;
    }
    case 7:
        inc_horiz();
        break;
    }
}

static void eval_sprites(int line) {
    memset(PPU.sp_color, 0, sizeof PPU.sp_color);
    PPU.sp_count = 0;
    int h = (PPU.ctrl & 0x20) ? 16 : 8;
    int found = 0;
    for (int i = 0; i < 64; i += 1) {
        int y = PPU.oam[i * 4];
        int row = line - y;
        if (row < 0 || row >= h) continue;
        if (found == 8) {
            PPU.status |= 0x20;          /* overflow flag: game-visible, keep */
            if (!ppu_no_flicker) break;  /* authentic hardware limit */
        }
        found += 1;
        uint8_t tile = PPU.oam[i * 4 + 1];
        uint8_t attr = PPU.oam[i * 4 + 2];
        int sx = PPU.oam[i * 4 + 3];
        bool flip_h = (attr & 0x40) != 0;
        bool flip_v = (attr & 0x80) != 0;
        int r = flip_v ? (h - 1 - row) : row;
        int base;
        if (h == 16) {
            int table = (tile & 1) << 12;
            int t = tile & 0xfe;
            if (r >= 8) { t += 1; r -= 8; }
            base = table + t * 16 + r;
        } else {
            base = ((PPU.ctrl & 0x08) << 9) + tile * 16 + r;
        }
        uint8_t lo = PPU.chr[base];
        uint8_t hi = PPU.chr[base + 8];
        uint8_t pal = attr & 3;
        uint8_t flags = (uint8_t)(((attr & 0x20) ? 1 : 0) | (i == 0 ? 2 : 0));
        for (int px = 0; px < 8; px += 1) {
            int x_pos = sx + px;
            if (x_pos > 255) break;
            if (PPU.sp_color[x_pos] != 0) continue;
            int bit = flip_h ? px : (7 - px);
            uint8_t c = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
            if (c == 0) continue;
            PPU.sp_color[x_pos] = c;
            PPU.sp_pal[x_pos] = pal;
            PPU.sp_flags[x_pos] = flags;
        }
    }
    PPU.sp_count = found;
}

static uint32_t backdrop_color(void) {
    uint8_t idx;
    if (!rendering_enabled() && (PPU.v & 0x3f00) == 0x3f00) {
        int p = PPU.v & 0x1f;
        if (p >= 0x10 && (p & 0x03) == 0) p -= 0x10;
        idx = PPU.palette[p];
    } else {
        idx = PPU.palette[0];
    }
    idx &= (PPU.mask & 0x01) ? 0x30 : 0x3f;
    return RGB_LUT[((PPU.mask & 0xe0) << 1) | idx];
}

static void render_pixel(int sl, int x) {
    uint8_t mask = PPU.mask;
    int bg_pix = 0, bg_pal = 0;
    if ((mask & 0x08) && (x >= 8 || (mask & 0x02))) {
        int shift = 15 - PPU.x;
        bg_pix = ((PPU.bg_shift_lo >> shift) & 1) | (((PPU.bg_shift_hi >> shift) & 1) << 1);
        if (bg_pix != 0) {
            int a_shift = 7 - PPU.x;
            bg_pal = ((PPU.at_shift_lo >> a_shift) & 1) | (((PPU.at_shift_hi >> a_shift) & 1) << 1);
        }
    }

    int sp_pix = 0, sp_pal = 0, sp_flags = 0;
    if ((mask & 0x10) && (x >= 8 || (mask & 0x04)) && sl != 0) {
        sp_pix = PPU.sp_color[x];
        sp_pal = PPU.sp_pal[x];
        sp_flags = PPU.sp_flags[x];
    }

    int pal_index;
    if (sp_pix != 0 && (bg_pix == 0 || (sp_flags & 1) == 0)) {
        pal_index = 0x10 + sp_pal * 4 + sp_pix;
    } else if (bg_pix != 0) {
        pal_index = bg_pal * 4 + bg_pix;
    } else {
        pal_index = 0;
    }

    if (sp_pix != 0 && bg_pix != 0 && (sp_flags & 2) != 0 && x != 255) {
        PPU.status |= 0x40;
    }

    uint8_t color = PPU.palette[pal_index == 0 ? 0 : pal_index];
    color &= (mask & 0x01) ? 0x30 : 0x3f;
    PPU.framebuffer[sl * 256 + x] = RGB_LUT[((mask & 0xe0) << 1) | color];
}

static void tick_render_line(int sl, int d, bool visible) {
    if (d >= 1 && d <= 256) {
        if (d >= 2) shift_bg();
        int ph = (d - 1) & 7;
        if (ph == 0 && d >= 9) reload_shifters();
        fetch_step(ph);
        if (d == 256) inc_vert();
        if (visible) render_pixel(sl, d - 1);
    } else if (d == 257) {
        shift_bg();
        reload_shifters();
        copy_horiz();
        PPU.oam_addr = 0;
        if (visible && sl < 239) {
            eval_sprites(sl);
        } else if (!visible) {
            memset(PPU.sp_color, 0, sizeof PPU.sp_color);
        }
    } else if (d >= 258 && d <= 320) {
        PPU.oam_addr = 0;
    } else if (d >= 321 && d <= 336) {
        shift_bg();
        int ph = (d - 1) & 7;
        if (ph == 0 && d == 329) reload_shifters();
        fetch_step(ph);
    } else if (d == 337) {
        shift_bg();
        reload_shifters();
        PPU.nt_byte = ppu_read(0x2000 | (PPU.v & 0x0fff));
    } else if (d == 339) {
        PPU.nt_byte = ppu_read(0x2000 | (PPU.v & 0x0fff));
    }
}

void ppu_tick(void) {
    int sl = PPU.scanline;
    int d = PPU.dot;
    bool rendering = (PPU.mask & 0x18) != 0;

    if (sl < 240) {
        if (rendering) {
            tick_render_line(sl, d, true);
        } else if (d >= 1 && d <= 256) {
            PPU.framebuffer[sl * 256 + (d - 1)] = backdrop_color();
        }
    } else if (sl == 241) {
        if (d == 1) {
            if (!PPU.nmi_suppress) {
                PPU.status |= 0x80;
                ppu_update_nmi_line();
            }
            PPU.nmi_suppress = false;
            /* onVblankStart: no-op in the machine */
        }
    } else if (sl == 261) {
        if (d == 1) {
            PPU.status &= (uint8_t)~(0x80 | 0x40 | 0x20);
            ppu_update_nmi_line();
        }
        if (rendering) {
            tick_render_line(sl, d, false);
            if (d >= 280 && d <= 304) copy_vert();
        }
    }

    int dot = d + 1;
    if (sl == 261 && d == 339 && PPU.odd_frame && rendering) {
        dot = 341;
    }
    if (dot >= 341) {
        PPU.dot = 0;
        int next = sl + 1;
        if (next >= 262) {
            next = 0;
            PPU.frame += 1;
            PPU.odd_frame = !PPU.odd_frame;
            M.frame_done = true; /* onFrameComplete */
        }
        PPU.scanline = next;
    } else {
        PPU.dot = dot;
    }
}
