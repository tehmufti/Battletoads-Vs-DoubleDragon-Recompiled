/*
 * bus.c — NES machine: bus, AxROM mapper (plus 0/2/3), OAM DMA, controllers,
 * master clock, frame loop. Ported 1:1 from src/nes/nes.js.
 *
 * Clocking model: every CPU bus access advances the master clock by exactly
 * one CPU cycle = 3 PPU dots + 1 APU cycle, BEFORE the access is performed.
 *
 * The frame loop is the hybrid dispatcher: recompiled native blocks run via
 * recomp_run() (Stage B, linked from recomp/gen/); whenever it returns —
 * unknown pc, RAM execution, pending interrupt, frame boundary — the
 * interpreter takes one step. With the interpreter-only build (recomp_stub.c)
 * this degrades to a pure emulator with identical behavior.
 */
#include "nes.h"
#include <stdlib.h>
#include <string.h>

Machine M;

int nes_init(const uint8_t *rom_bytes, size_t rom_len, double sample_rate) {
    if (rom_len < 16 || rom_bytes[0] != 0x4e || rom_bytes[1] != 0x45 ||
        rom_bytes[2] != 0x53 || rom_bytes[3] != 0x1a) {
        return -1; /* not iNES */
    }
    int prg_banks16 = rom_bytes[4];
    int chr_banks8 = rom_bytes[5];
    int flags6 = rom_bytes[6];
    int flags7 = rom_bytes[7];
    int mapper = (flags7 & 0xf0) | (flags6 >> 4);
    size_t trainer = (flags6 & 0x04) ? 512 : 0;
    size_t prg_start = 16 + trainer;
    size_t prg_size = (size_t)prg_banks16 * 0x4000;
    size_t chr_size = (size_t)chr_banks8 * 0x2000;
    if (prg_start + prg_size > rom_len) return -2;

    memset(&M, 0, sizeof M);
    memset(&CPU, 0, sizeof CPU);
    memset(&PPU, 0, sizeof PPU);
    memset(&APU, 0, sizeof APU);

    M.mapper = mapper;
    M.prg = (uint8_t *)malloc(prg_size);
    if (!M.prg) return -3;
    memcpy(M.prg, rom_bytes + prg_start, prg_size);
    M.prg_len = prg_size;
    M.prg_mask32 = (int)(prg_size >> 15) - 1;
    M.prg_mask16 = (int)(prg_size >> 14) - 1;

    CPU.p = FI | FU;
    CPU.sp = 0xfd;

    PPU.chr_writable = true;
    switch (mapper) {
    case 7:
        PPU.mirror = MIRROR_SINGLE0;
        break;
    case 0: case 2: case 3:
        PPU.mirror = (flags6 & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
        break;
    default:
        return -4; /* mapper not supported */
    }
    if (chr_size > 0) {
        size_t n = chr_size < 0x2000 ? chr_size : 0x2000;
        memcpy(PPU.chr, rom_bytes + prg_start + prg_size, n);
        PPU.chr_writable = false;
    }

    apu_init(sample_rate);
    nes_reset(true);
    return 0;
}

void nes_reset(bool power) {
    if (power) {
        memset(M.ram, 0, sizeof M.ram);
        if (PPU.chr_writable) memset(PPU.chr, 0, sizeof PPU.chr);
    }
    ppu_reset();
    M.irq_line = 0;
    M.stall = 0;
    M.frame_done = false;
    if (M.mapper == 7) {
        M.prg_bank = 0;
        PPU.mirror = MIRROR_SINGLE0;
    }
    CPU.nmi_line = false;
    CPU.nmi_pending = false;
    cpu_reset();
}

/* ── Master clock ────────────────────────────────────────────────────────── */
void tick_units(void) {
    ppu_tick(); ppu_tick(); ppu_tick();
    apu_tick();
    M.cycle_count += 1;
}

static uint8_t bus_read_raw(uint16_t addr);
static void bus_write_raw(uint16_t addr, uint8_t value);

uint8_t bus_rd(uint16_t addr) {
    if (M.stall > 0) {
        do { M.stall -= 1; tick_units(); } while (M.stall > 0);
    }
    tick_units();
    uint8_t v = bus_read_raw(addr);
    M.open_bus = v;
    return v;
}

void bus_wr(uint16_t addr, uint8_t value) {
    tick_units();
    M.open_bus = value;
    bus_write_raw(addr, value);
}

/* ── PRG mapping ─────────────────────────────────────────────────────────── */
uint8_t prg_read(uint16_t addr) {
    switch (M.mapper) {
    case 7:
        return M.prg[((size_t)(M.prg_bank & M.prg_mask32) << 15) | (addr & 0x7fff)];
    case 2:
        if (addr < 0xc000) {
            return M.prg[((size_t)(M.prg_bank & M.prg_mask16) << 14) | (addr & 0x3fff)];
        }
        return M.prg[((size_t)M.prg_mask16 << 14) | (addr & 0x3fff)];
    default:
        return M.prg[(size_t)(addr - 0x8000) & (M.prg_len - 1)];
    }
}

static void mapper_write(uint16_t addr, uint8_t value) {
    (void)addr;
    switch (M.mapper) {
    case 7:
        M.prg_bank = value & 0x07;
        PPU.mirror = (value & 0x10) ? MIRROR_SINGLE1 : MIRROR_SINGLE0;
        break;
    case 2:
        M.prg_bank = value;
        break;
    default:
        break;
    }
}

/* ── Controllers ─────────────────────────────────────────────────────────── */
static uint8_t read_pad(int port) {
    int bit;
    if (M.pad_strobe & 1) {
        bit = M.pad[port] & 1;
    } else if (M.pad_index[port] < 8) {
        bit = (M.pad_latch[port] >> M.pad_index[port]) & 1;
        M.pad_index[port] += 1;
    } else {
        bit = 1;
    }
    return (uint8_t)(0x40 | bit);
}

void nes_set_buttons(int port, uint8_t mask) {
    M.pad[port] = mask;
}

/* ── OAM DMA ($4014): real reads through the bus ─────────────────────────── */
static void oam_dma(uint8_t page) {
    uint16_t base = (uint16_t)(page << 8);
    tick_units(); /* alignment dummy cycle */
    if (M.cycle_count & 1) tick_units();
    for (int i = 0; i < 256; i += 1) {
        tick_units();
        uint8_t v = bus_read_raw((uint16_t)(base + i));
        tick_units();
        PPU.oam[PPU.oam_addr] = v;
        PPU.oam_addr += 1;
    }
}

/* ── Raw bus (no clocking; clocking happens in bus_rd/bus_wr) ────────────── */
static uint8_t bus_read_raw(uint16_t addr) {
    if (addr < 0x2000) return M.ram[addr & 0x7ff];
    if (addr < 0x4000) return ppu_read_reg(addr & 7);
    if (addr < 0x4020) {
        if (addr == 0x4015) return apu_read_4015();
        if (addr == 0x4016) return read_pad(0);
        if (addr == 0x4017) return read_pad(1);
        return M.open_bus;
    }
    if (addr < 0x6000) return M.open_bus;
    if (addr < 0x8000) return M.wram[addr - 0x6000];
    return prg_read(addr);
}

static void bus_write_raw(uint16_t addr, uint8_t value) {
    if (addr < 0x2000) { M.ram[addr & 0x7ff] = value; return; }
    if (addr < 0x4000) { ppu_write_reg(addr & 7, value); return; }
    if (addr < 0x4020) {
        if (addr == 0x4014) { oam_dma(value); return; }
        if (addr == 0x4016) {
            int prev = M.pad_strobe;
            M.pad_strobe = value & 1;
            if (M.pad_strobe) {
                M.pad_latch[0] = M.pad[0];
                M.pad_latch[1] = M.pad[1];
                M.pad_index[0] = 0;
                M.pad_index[1] = 0;
            } else if (prev == 1) {
                M.pad_latch[0] = M.pad[0];
                M.pad_latch[1] = M.pad[1];
                M.pad_index[0] = 0;
                M.pad_index[1] = 0;
            }
            return;
        }
        if (addr <= 0x4013 || addr == 0x4015 || addr == 0x4017) {
            apu_write(addr, value);
        }
        return;
    }
    if (addr < 0x6000) return;
    if (addr < 0x8000) { M.wram[addr - 0x6000] = value; return; }
    mapper_write(addr, value);
}

/* ── IRQ / DMC ───────────────────────────────────────────────────────────── */
void set_irq(int bit, bool level) {
    if (level) M.irq_line |= (1 << bit);
    else M.irq_line &= ~(1 << bit);
}

uint8_t dmc_fetch(uint16_t addr) {
    M.stall += 4;
    return prg_read(addr | 0x8000);
}

/* ── Frame loop (hybrid dispatcher) ──────────────────────────────────────── */
void nes_run_frame(void) {
    M.frame_done = false;
    long guard = 200000;
    while (!M.frame_done && guard > 0) {
        if (recomp_enabled) {
            recomp_run(); /* runs native blocks until it must yield */
            if (M.frame_done) break;
        }
        cpu_step();
        guard -= 1;
    }
}

/* ── Peek (no clock side effects) ────────────────────────────────────────── */
uint8_t nes_peek(uint16_t addr) {
    if (addr < 0x2000) return M.ram[addr & 0x7ff];
    if (addr >= 0x8000) return prg_read(addr);
    if (addr >= 0x6000) return M.wram[addr - 0x6000];
    return 0;
}
