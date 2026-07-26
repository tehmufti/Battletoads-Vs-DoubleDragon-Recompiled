/*
 * nes.h — the recompilation's C runtime: cycle-accurate NES machine state.
 *
 * A direct port of the verified JS core (src/nes/): same clocking model
 * (every CPU bus access advances the master clock by 1 CPU cycle = 3 PPU
 * dots + 1 APU cycle, BEFORE the access), same register semantics, same
 * rendering pipeline. Ported 1:1 so the two implementations can be verified
 * frame-exact against each other (tools/recomp/verify-recomp.mjs).
 *
 * Single-machine design: one global machine, plain C, no allocations after
 * nes_init(). Compiles as C11 (MSVC /std:c11 or newer, or any GCC/Clang).
 */
#ifndef NES_H
#define NES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── CPU ─────────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t a, x, y, sp, p;
    uint16_t pc;
    bool nmi_line, nmi_pending, jammed;
    uint64_t cycles; /* informational */
} Cpu;

/* status flags */
#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

/* ── PPU ─────────────────────────────────────────────────────────────────── */
enum { MIRROR_HORIZONTAL = 0, MIRROR_VERTICAL = 1, MIRROR_SINGLE0 = 2, MIRROR_SINGLE1 = 3 };

typedef struct {
    uint8_t chr[0x2000];       /* CHR-RAM */
    bool chr_writable;
    uint8_t vram[0x800];
    uint8_t palette[0x20];
    uint8_t oam[0x100];
    int mirror;

    uint32_t framebuffer[256 * 240]; /* 0xAABBGGRR little-endian RGBA */

    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;
    uint8_t x, w;
    uint8_t read_buffer, open_bus;

    int scanline, dot;
    uint32_t frame;
    bool odd_frame, nmi_suppress;

    uint8_t nt_byte, at_byte, bg_lo_byte, bg_hi_byte;
    uint16_t bg_shift_lo, bg_shift_hi;
    uint8_t at_shift_lo, at_shift_hi, at_latch_lo, at_latch_hi;

    uint8_t sp_color[256], sp_pal[256], sp_flags[256];
    int sp_count;
} Ppu;

/* ── APU ─────────────────────────────────────────────────────────────────── */
typedef struct {
    bool start;
    uint8_t divider, decay, volume;
    bool loop, constant;
} Envelope;

typedef struct {
    int channel;
    bool enabled;
    uint8_t duty, seq_pos;
    uint16_t timer, period;
    uint8_t length;
    bool length_halt;
    Envelope env;
    bool sweep_enabled, sweep_negate, sweep_reload;
    uint8_t sweep_period, sweep_shift, sweep_divider;
} Pulse;

typedef struct {
    bool enabled;
    uint16_t timer, period;
    uint8_t length;
    bool length_halt;
    uint8_t linear, linear_reload;
    bool linear_reload_flag;
    uint8_t seq_pos;
} Triangle;

typedef struct {
    bool enabled;
    uint16_t timer, period;
    uint8_t length;
    bool length_halt, mode;
    uint16_t shift;
    Envelope env;
} Noise;

typedef struct {
    bool enabled, irq_enabled, loop, silence, irq;
    uint16_t rate, timer;
    uint8_t level;
    uint16_t sample_addr, sample_length, current_addr, bytes_remaining;
    uint8_t shift;
    int bits_remaining;
    int buffer; /* -1 = empty */
} Dmc;

#define APU_RING_LEN (1 << 15)

typedef struct {
    Pulse pulse1, pulse2;
    Triangle triangle;
    Noise noise;
    Dmc dmc;

    int frame_counter_cycle;
    bool frame_mode5, frame_irq_inhibit, frame_irq, odd_cycle;
    int pending_frame_write, pending_frame_delay;

    double sample_rate, cycles_per_sample, sample_counter, sample_acc, hp_last, hp_out;
    int sample_acc_count;
    float ring[APU_RING_LEN];
    int ring_head, ring_tail;
} Apu;

/* ── Machine ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t *prg;          /* PRG ROM (owned) */
    size_t prg_len;
    uint8_t ram[0x800];
    uint8_t wram[0x2000];

    int mapper;
    int prg_bank;
    int prg_mask32, prg_mask16;

    int irq_line;          /* bit0 = APU frame, bit1 = DMC */
    int stall;             /* pending DMC stall cycles */
    uint64_t cycle_count;
    bool frame_done;
    uint8_t open_bus;

    uint8_t pad[2], pad_latch[2];
    int pad_index[2], pad_strobe;

    uint64_t native_instr, interp_instr; /* execution-mix instrumentation */
} Machine;

extern Machine M;
extern Cpu CPU;
extern Ppu PPU;
extern Apu APU;

/* button bits (A,B,Sel,St,U,D,L,R = bits 0..7) */
#define BTN_A 0x01
#define BTN_B 0x02
#define BTN_SELECT 0x04
#define BTN_START 0x08
#define BTN_UP 0x10
#define BTN_DOWN 0x20
#define BTN_LEFT 0x40
#define BTN_RIGHT 0x80

/* ── Machine API (bus.c) ─────────────────────────────────────────────────── */
int nes_init(const uint8_t *rom_bytes, size_t rom_len, double sample_rate);
void nes_reset(bool power);
void nes_run_frame(void);
void nes_set_buttons(int port, uint8_t mask);
uint8_t nes_peek(uint16_t addr); /* no clock side effects */

void tick_units(void);
uint8_t bus_rd(uint16_t addr);        /* ticked read (one CPU cycle) */
void bus_wr(uint16_t addr, uint8_t v);/* ticked write (one CPU cycle) */
uint8_t prg_read(uint16_t addr);
void set_irq(int bit, bool level);
uint8_t dmc_fetch(uint16_t addr);

/* ── CPU API (cpu.c) ─────────────────────────────────────────────────────── */
void cpu_reset(void);
void cpu_step(void);
void cpu_set_nmi_line(bool level);
/* helpers shared with generated code (Stage B) */
uint8_t cpu_set_zn(uint8_t v);
void cpu_adc(uint8_t v);
void cpu_sbc(uint8_t v);
void cpu_cmp(uint8_t reg, uint8_t v);
uint8_t cpu_asl(uint8_t v);
uint8_t cpu_lsr(uint8_t v);
uint8_t cpu_rol(uint8_t v);
uint8_t cpu_ror(uint8_t v);
void cpu_bit(uint8_t v);
void cpu_push(uint8_t v);
uint8_t cpu_pull(void);

/* ── PPU API (ppu.c) ─────────────────────────────────────────────────────── */
void ppu_reset(void);
void ppu_tick(void);
uint8_t ppu_read_reg(int reg);
void ppu_write_reg(int reg, uint8_t value);
void ppu_update_nmi_line(void);
/* Frontend option: lift the hardware 8-sprites-per-scanline limit (removes
 * flicker). Default 0 = authentic; the sprite-overflow status bit is still
 * raised either way so game logic reading $2002 behaves identically. */
extern int ppu_no_flicker;

/* ── Save states (state.c) ───────────────────────────────────────────────── */
int nes_save_state(const char *path);
int nes_load_state(const char *path);

/* ── APU API (apu.c) ─────────────────────────────────────────────────────── */
void apu_init(double sample_rate);
void apu_tick(void);
void apu_write(uint16_t addr, uint8_t value);
uint8_t apu_read_4015(void);
int apu_available_samples(void);
int apu_read_samples(float *out, int count);

/* ── Recompiled code entry (Stage B; weak default in bus.c) ──────────────── */
/* Runs natively-compiled blocks while possible; returns when it wants the
 * interpreter to take over (unknown pc, RAM execution). Returns number of
 * instructions executed natively (0 = nothing ran). */
int recomp_run(void);
extern int recomp_enabled;

#endif /* NES_H */
