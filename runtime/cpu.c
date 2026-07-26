/*
 * cpu.c — cycle-accurate 6502 (2A03) interpreter, ported 1:1 from
 * src/nes/cpu.js. Every CPU cycle is exactly one bus access; bus_rd/bus_wr
 * advance the master clock before the access, so dummy reads/writes land at
 * hardware-true times.
 *
 * In the recompilation this interpreter is the correctness baseline and the
 * fallback for RAM-executed code; ROM code normally runs via the generated
 * native blocks (recomp_run), which reproduce the same bus-access pattern.
 */
#include "nes.h"

Cpu CPU;

void cpu_set_nmi_line(bool level) {
    if (level && !CPU.nmi_line) CPU.nmi_pending = true;
    CPU.nmi_line = level;
}

static inline uint8_t rd(uint16_t a) { CPU.cycles += 1; return bus_rd(a); }
static inline void wr(uint16_t a, uint8_t v) { CPU.cycles += 1; bus_wr(a, v); }

static inline uint8_t fetch(void) { uint8_t v = rd(CPU.pc); CPU.pc += 1; return v; }

void cpu_push(uint8_t v) { wr(0x100 | CPU.sp, v); CPU.sp -= 1; }
uint8_t cpu_pull(void) { CPU.sp += 1; return rd(0x100 | CPU.sp); }

uint8_t cpu_set_zn(uint8_t v) {
    CPU.p = (uint8_t)((CPU.p & ~(FZ | FN)) | (v == 0 ? FZ : 0) | (v & 0x80));
    return v;
}

void cpu_reset(void) {
    CPU.jammed = false;
    CPU.sp = 0xfd;
    CPU.p |= FI;
    rd(CPU.pc); rd(CPU.pc); rd(0x100 | CPU.sp);
    rd(0x100 | ((CPU.sp - 1) & 0xff)); rd(0x100 | ((CPU.sp - 2) & 0xff));
    uint8_t lo = rd(0xfffc);
    uint8_t hi = rd(0xfffd);
    CPU.pc = (uint16_t)(lo | (hi << 8));
}

static void service(uint16_t vector) {
    rd(CPU.pc); rd(CPU.pc);
    cpu_push((uint8_t)(CPU.pc >> 8)); cpu_push((uint8_t)(CPU.pc & 0xff));
    cpu_push((uint8_t)((CPU.p & ~FB) | FU));
    CPU.p |= FI;
    uint8_t lo = rd(vector);
    uint8_t hi = rd(vector + 1);
    CPU.pc = (uint16_t)(lo | (hi << 8));
}

/* ── Addressing modes (burn exactly the right cycles) ────────────────────── */
static inline uint16_t a_imm(void) { uint16_t a = CPU.pc; CPU.pc += 1; return a; }
static inline uint16_t a_zp(void) { return fetch(); }
static inline uint16_t a_zpx(void) { uint8_t z = fetch(); rd(z); return (uint8_t)(z + CPU.x); }
static inline uint16_t a_zpy(void) { uint8_t z = fetch(); rd(z); return (uint8_t)(z + CPU.y); }
static inline uint16_t a_abs(void) { uint8_t lo = fetch(); return (uint16_t)(lo | (fetch() << 8)); }
static inline uint16_t a_absx_r(void) {
    uint8_t lo = fetch(); uint8_t hi = fetch();
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.x;
    if ((int)lo + CPU.x > 0xff) rd((base & 0xff00) | (eff & 0xff));
    return eff;
}
static inline uint16_t a_absx_w(void) {
    uint8_t lo = fetch(); uint8_t hi = fetch();
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.x;
    rd((base & 0xff00) | (eff & 0xff));
    return eff;
}
static inline uint16_t a_absy_r(void) {
    uint8_t lo = fetch(); uint8_t hi = fetch();
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.y;
    if ((int)lo + CPU.y > 0xff) rd((base & 0xff00) | (eff & 0xff));
    return eff;
}
static inline uint16_t a_absy_w(void) {
    uint8_t lo = fetch(); uint8_t hi = fetch();
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.y;
    rd((base & 0xff00) | (eff & 0xff));
    return eff;
}
static inline uint16_t a_izx(void) {
    uint8_t z = fetch(); rd(z);
    uint8_t lo = rd((uint8_t)(z + CPU.x));
    uint8_t hi = rd((uint8_t)(z + CPU.x + 1));
    return (uint16_t)(lo | (hi << 8));
}
static inline uint16_t a_izy_r(void) {
    uint8_t z = fetch();
    uint8_t lo = rd(z); uint8_t hi = rd((uint8_t)(z + 1));
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.y;
    if ((int)lo + CPU.y > 0xff) rd((base & 0xff00) | (eff & 0xff));
    return eff;
}
static inline uint16_t a_izy_w(void) {
    uint8_t z = fetch();
    uint8_t lo = rd(z); uint8_t hi = rd((uint8_t)(z + 1));
    uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t eff = base + CPU.y;
    rd((base & 0xff00) | (eff & 0xff));
    return eff;
}

/* ── ALU ops ─────────────────────────────────────────────────────────────── */
void cpu_adc(uint8_t v) {
    int sum = CPU.a + v + (CPU.p & FC);
    uint8_t r = (uint8_t)sum;
    CPU.p = (uint8_t)((CPU.p & ~(FC | FV)) | (sum > 0xff ? FC : 0) |
                      (((~(CPU.a ^ v) & (CPU.a ^ r)) & 0x80) ? FV : 0));
    CPU.a = cpu_set_zn(r);
}
void cpu_sbc(uint8_t v) { cpu_adc(v ^ 0xff); }
void cpu_cmp(uint8_t reg, uint8_t v) {
    uint8_t r = (uint8_t)(reg - v);
    CPU.p = (uint8_t)((CPU.p & ~FC) | (reg >= v ? FC : 0));
    cpu_set_zn(r);
}
uint8_t cpu_asl(uint8_t v) { CPU.p = (uint8_t)((CPU.p & ~FC) | ((v >> 7) & 1)); return cpu_set_zn((uint8_t)(v << 1)); }
uint8_t cpu_lsr(uint8_t v) { CPU.p = (uint8_t)((CPU.p & ~FC) | (v & 1)); return cpu_set_zn(v >> 1); }
uint8_t cpu_rol(uint8_t v) { uint8_t c = CPU.p & FC; CPU.p = (uint8_t)((CPU.p & ~FC) | ((v >> 7) & 1)); return cpu_set_zn((uint8_t)((v << 1) | c)); }
uint8_t cpu_ror(uint8_t v) { uint8_t c = (uint8_t)((CPU.p & FC) << 7); CPU.p = (uint8_t)((CPU.p & ~FC) | (v & 1)); return cpu_set_zn((uint8_t)((v >> 1) | c)); }
void cpu_bit(uint8_t v) {
    CPU.p = (uint8_t)((CPU.p & ~(FZ | FV | FN)) | ((CPU.a & v) == 0 ? FZ : 0) | (v & (FV | FN)));
}

/* RMW helper: dummy write of the old value, then the new one */
typedef uint8_t (*RmwFn)(uint8_t);
static inline uint8_t rmw(uint16_t addr, RmwFn fn) {
    uint8_t v = rd(addr);
    wr(addr, v);
    uint8_t r = fn(v);
    wr(addr, r);
    return r;
}
static uint8_t fn_inc(uint8_t v) { return cpu_set_zn((uint8_t)(v + 1)); }
static uint8_t fn_dec(uint8_t v) { return cpu_set_zn((uint8_t)(v - 1)); }
static uint8_t fn_dec_raw(uint8_t v) { return (uint8_t)(v - 1); }
static uint8_t fn_inc_raw(uint8_t v) { return (uint8_t)(v + 1); }

static inline void branch(bool cond) {
    uint8_t off = fetch();
    if (!cond) return;
    rd(CPU.pc);
    uint16_t target = (uint16_t)(CPU.pc + (off < 0x80 ? off : off - 0x100));
    if ((target & 0xff00) != (CPU.pc & 0xff00)) {
        rd((CPU.pc & 0xff00) | (target & 0xff));
    }
    CPU.pc = target;
}

/* ── Main step ───────────────────────────────────────────────────────────── */
void cpu_step(void) {
    if (CPU.jammed) { CPU.cycles += 1; tick_units(); return; }

    if (CPU.nmi_pending) {
        CPU.nmi_pending = false;
        service(0xfffa);
        return;
    }
    if (M.irq_line != 0 && (CPU.p & FI) == 0) {
        service(0xfffe);
        return;
    }

    M.interp_instr += 1;
    uint8_t op = fetch();
    switch (op) {
    /* ── Loads / stores ── */
    case 0xa9: CPU.a = cpu_set_zn(rd(a_imm())); break;
    case 0xa5: CPU.a = cpu_set_zn(rd(a_zp())); break;
    case 0xb5: CPU.a = cpu_set_zn(rd(a_zpx())); break;
    case 0xad: CPU.a = cpu_set_zn(rd(a_abs())); break;
    case 0xbd: CPU.a = cpu_set_zn(rd(a_absx_r())); break;
    case 0xb9: CPU.a = cpu_set_zn(rd(a_absy_r())); break;
    case 0xa1: CPU.a = cpu_set_zn(rd(a_izx())); break;
    case 0xb1: CPU.a = cpu_set_zn(rd(a_izy_r())); break;

    case 0xa2: CPU.x = cpu_set_zn(rd(a_imm())); break;
    case 0xa6: CPU.x = cpu_set_zn(rd(a_zp())); break;
    case 0xb6: CPU.x = cpu_set_zn(rd(a_zpy())); break;
    case 0xae: CPU.x = cpu_set_zn(rd(a_abs())); break;
    case 0xbe: CPU.x = cpu_set_zn(rd(a_absy_r())); break;

    case 0xa0: CPU.y = cpu_set_zn(rd(a_imm())); break;
    case 0xa4: CPU.y = cpu_set_zn(rd(a_zp())); break;
    case 0xb4: CPU.y = cpu_set_zn(rd(a_zpx())); break;
    case 0xac: CPU.y = cpu_set_zn(rd(a_abs())); break;
    case 0xbc: CPU.y = cpu_set_zn(rd(a_absx_r())); break;

    case 0x85: wr(a_zp(), CPU.a); break;
    case 0x95: wr(a_zpx(), CPU.a); break;
    case 0x8d: wr(a_abs(), CPU.a); break;
    case 0x9d: wr(a_absx_w(), CPU.a); break;
    case 0x99: wr(a_absy_w(), CPU.a); break;
    case 0x81: wr(a_izx(), CPU.a); break;
    case 0x91: wr(a_izy_w(), CPU.a); break;

    case 0x86: wr(a_zp(), CPU.x); break;
    case 0x96: wr(a_zpy(), CPU.x); break;
    case 0x8e: wr(a_abs(), CPU.x); break;

    case 0x84: wr(a_zp(), CPU.y); break;
    case 0x94: wr(a_zpx(), CPU.y); break;
    case 0x8c: wr(a_abs(), CPU.y); break;

    /* ── Transfers ── */
    case 0xaa: rd(CPU.pc); CPU.x = cpu_set_zn(CPU.a); break;
    case 0xa8: rd(CPU.pc); CPU.y = cpu_set_zn(CPU.a); break;
    case 0x8a: rd(CPU.pc); CPU.a = cpu_set_zn(CPU.x); break;
    case 0x98: rd(CPU.pc); CPU.a = cpu_set_zn(CPU.y); break;
    case 0xba: rd(CPU.pc); CPU.x = cpu_set_zn(CPU.sp); break;
    case 0x9a: rd(CPU.pc); CPU.sp = CPU.x; break;

    /* ── ALU ── */
    case 0x69: cpu_adc(rd(a_imm())); break;
    case 0x65: cpu_adc(rd(a_zp())); break;
    case 0x75: cpu_adc(rd(a_zpx())); break;
    case 0x6d: cpu_adc(rd(a_abs())); break;
    case 0x7d: cpu_adc(rd(a_absx_r())); break;
    case 0x79: cpu_adc(rd(a_absy_r())); break;
    case 0x61: cpu_adc(rd(a_izx())); break;
    case 0x71: cpu_adc(rd(a_izy_r())); break;

    case 0xe9: case 0xeb: cpu_sbc(rd(a_imm())); break;
    case 0xe5: cpu_sbc(rd(a_zp())); break;
    case 0xf5: cpu_sbc(rd(a_zpx())); break;
    case 0xed: cpu_sbc(rd(a_abs())); break;
    case 0xfd: cpu_sbc(rd(a_absx_r())); break;
    case 0xf9: cpu_sbc(rd(a_absy_r())); break;
    case 0xe1: cpu_sbc(rd(a_izx())); break;
    case 0xf1: cpu_sbc(rd(a_izy_r())); break;

    case 0x29: CPU.a = cpu_set_zn(CPU.a & rd(a_imm())); break;
    case 0x25: CPU.a = cpu_set_zn(CPU.a & rd(a_zp())); break;
    case 0x35: CPU.a = cpu_set_zn(CPU.a & rd(a_zpx())); break;
    case 0x2d: CPU.a = cpu_set_zn(CPU.a & rd(a_abs())); break;
    case 0x3d: CPU.a = cpu_set_zn(CPU.a & rd(a_absx_r())); break;
    case 0x39: CPU.a = cpu_set_zn(CPU.a & rd(a_absy_r())); break;
    case 0x21: CPU.a = cpu_set_zn(CPU.a & rd(a_izx())); break;
    case 0x31: CPU.a = cpu_set_zn(CPU.a & rd(a_izy_r())); break;

    case 0x09: CPU.a = cpu_set_zn(CPU.a | rd(a_imm())); break;
    case 0x05: CPU.a = cpu_set_zn(CPU.a | rd(a_zp())); break;
    case 0x15: CPU.a = cpu_set_zn(CPU.a | rd(a_zpx())); break;
    case 0x0d: CPU.a = cpu_set_zn(CPU.a | rd(a_abs())); break;
    case 0x1d: CPU.a = cpu_set_zn(CPU.a | rd(a_absx_r())); break;
    case 0x19: CPU.a = cpu_set_zn(CPU.a | rd(a_absy_r())); break;
    case 0x01: CPU.a = cpu_set_zn(CPU.a | rd(a_izx())); break;
    case 0x11: CPU.a = cpu_set_zn(CPU.a | rd(a_izy_r())); break;

    case 0x49: CPU.a = cpu_set_zn(CPU.a ^ rd(a_imm())); break;
    case 0x45: CPU.a = cpu_set_zn(CPU.a ^ rd(a_zp())); break;
    case 0x55: CPU.a = cpu_set_zn(CPU.a ^ rd(a_zpx())); break;
    case 0x4d: CPU.a = cpu_set_zn(CPU.a ^ rd(a_abs())); break;
    case 0x5d: CPU.a = cpu_set_zn(CPU.a ^ rd(a_absx_r())); break;
    case 0x59: CPU.a = cpu_set_zn(CPU.a ^ rd(a_absy_r())); break;
    case 0x41: CPU.a = cpu_set_zn(CPU.a ^ rd(a_izx())); break;
    case 0x51: CPU.a = cpu_set_zn(CPU.a ^ rd(a_izy_r())); break;

    case 0xc9: cpu_cmp(CPU.a, rd(a_imm())); break;
    case 0xc5: cpu_cmp(CPU.a, rd(a_zp())); break;
    case 0xd5: cpu_cmp(CPU.a, rd(a_zpx())); break;
    case 0xcd: cpu_cmp(CPU.a, rd(a_abs())); break;
    case 0xdd: cpu_cmp(CPU.a, rd(a_absx_r())); break;
    case 0xd9: cpu_cmp(CPU.a, rd(a_absy_r())); break;
    case 0xc1: cpu_cmp(CPU.a, rd(a_izx())); break;
    case 0xd1: cpu_cmp(CPU.a, rd(a_izy_r())); break;

    case 0xe0: cpu_cmp(CPU.x, rd(a_imm())); break;
    case 0xe4: cpu_cmp(CPU.x, rd(a_zp())); break;
    case 0xec: cpu_cmp(CPU.x, rd(a_abs())); break;
    case 0xc0: cpu_cmp(CPU.y, rd(a_imm())); break;
    case 0xc4: cpu_cmp(CPU.y, rd(a_zp())); break;
    case 0xcc: cpu_cmp(CPU.y, rd(a_abs())); break;

    case 0x24: cpu_bit(rd(a_zp())); break;
    case 0x2c: cpu_bit(rd(a_abs())); break;

    /* ── Shifts / rotates ── */
    case 0x0a: rd(CPU.pc); CPU.a = cpu_asl(CPU.a); break;
    case 0x06: rmw(a_zp(), cpu_asl); break;
    case 0x16: rmw(a_zpx(), cpu_asl); break;
    case 0x0e: rmw(a_abs(), cpu_asl); break;
    case 0x1e: rmw(a_absx_w(), cpu_asl); break;

    case 0x4a: rd(CPU.pc); CPU.a = cpu_lsr(CPU.a); break;
    case 0x46: rmw(a_zp(), cpu_lsr); break;
    case 0x56: rmw(a_zpx(), cpu_lsr); break;
    case 0x4e: rmw(a_abs(), cpu_lsr); break;
    case 0x5e: rmw(a_absx_w(), cpu_lsr); break;

    case 0x2a: rd(CPU.pc); CPU.a = cpu_rol(CPU.a); break;
    case 0x26: rmw(a_zp(), cpu_rol); break;
    case 0x36: rmw(a_zpx(), cpu_rol); break;
    case 0x2e: rmw(a_abs(), cpu_rol); break;
    case 0x3e: rmw(a_absx_w(), cpu_rol); break;

    case 0x6a: rd(CPU.pc); CPU.a = cpu_ror(CPU.a); break;
    case 0x66: rmw(a_zp(), cpu_ror); break;
    case 0x76: rmw(a_zpx(), cpu_ror); break;
    case 0x6e: rmw(a_abs(), cpu_ror); break;
    case 0x7e: rmw(a_absx_w(), cpu_ror); break;

    /* ── Inc / dec ── */
    case 0xe6: rmw(a_zp(), fn_inc); break;
    case 0xf6: rmw(a_zpx(), fn_inc); break;
    case 0xee: rmw(a_abs(), fn_inc); break;
    case 0xfe: rmw(a_absx_w(), fn_inc); break;
    case 0xc6: rmw(a_zp(), fn_dec); break;
    case 0xd6: rmw(a_zpx(), fn_dec); break;
    case 0xce: rmw(a_abs(), fn_dec); break;
    case 0xde: rmw(a_absx_w(), fn_dec); break;
    case 0xe8: rd(CPU.pc); CPU.x = cpu_set_zn((uint8_t)(CPU.x + 1)); break;
    case 0xc8: rd(CPU.pc); CPU.y = cpu_set_zn((uint8_t)(CPU.y + 1)); break;
    case 0xca: rd(CPU.pc); CPU.x = cpu_set_zn((uint8_t)(CPU.x - 1)); break;
    case 0x88: rd(CPU.pc); CPU.y = cpu_set_zn((uint8_t)(CPU.y - 1)); break;

    /* ── Flags ── */
    case 0x18: rd(CPU.pc); CPU.p &= (uint8_t)~FC; break;
    case 0x38: rd(CPU.pc); CPU.p |= FC; break;
    case 0x58: rd(CPU.pc); CPU.p &= (uint8_t)~FI; break;
    case 0x78: rd(CPU.pc); CPU.p |= FI; break;
    case 0xb8: rd(CPU.pc); CPU.p &= (uint8_t)~FV; break;
    case 0xd8: rd(CPU.pc); CPU.p &= (uint8_t)~FD; break;
    case 0xf8: rd(CPU.pc); CPU.p |= FD; break;

    /* ── Jumps / subroutines ── */
    case 0x4c: { uint8_t lo = fetch(); uint8_t hi = fetch(); CPU.pc = (uint16_t)(lo | (hi << 8)); break; }
    case 0x6c: {
        uint8_t lo = fetch(); uint8_t hi = fetch();
        uint16_t ptr = (uint16_t)(lo | (hi << 8));
        uint8_t tlo = rd(ptr);
        uint8_t thi = rd((ptr & 0xff00) | ((ptr + 1) & 0xff)); /* page-wrap bug */
        CPU.pc = (uint16_t)(tlo | (thi << 8));
        break;
    }
    case 0x20: {
        uint8_t lo = fetch();
        rd(0x100 | CPU.sp);
        cpu_push((uint8_t)(CPU.pc >> 8)); cpu_push((uint8_t)(CPU.pc & 0xff));
        uint8_t hi = rd(CPU.pc);
        CPU.pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x60: {
        rd(CPU.pc);
        rd(0x100 | CPU.sp);
        uint8_t lo = cpu_pull(); uint8_t hi = cpu_pull();
        CPU.pc = (uint16_t)(lo | (hi << 8));
        rd(CPU.pc);
        CPU.pc += 1;
        break;
    }
    case 0x40: {
        rd(CPU.pc);
        rd(0x100 | CPU.sp);
        CPU.p = (uint8_t)((cpu_pull() & ~FB) | FU);
        uint8_t lo = cpu_pull(); uint8_t hi = cpu_pull();
        CPU.pc = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x00: { /* BRK */
        fetch();
        cpu_push((uint8_t)(CPU.pc >> 8)); cpu_push((uint8_t)(CPU.pc & 0xff));
        cpu_push((uint8_t)(CPU.p | FB | FU));
        CPU.p |= FI;
        uint8_t lo = rd(0xfffe); uint8_t hi = rd(0xffff);
        CPU.pc = (uint16_t)(lo | (hi << 8));
        break;
    }

    /* ── Stack ── */
    case 0x48: rd(CPU.pc); cpu_push(CPU.a); break;
    case 0x08: rd(CPU.pc); cpu_push((uint8_t)(CPU.p | FB | FU)); break;
    case 0x68: rd(CPU.pc); rd(0x100 | CPU.sp); CPU.a = cpu_set_zn(cpu_pull()); break;
    case 0x28: rd(CPU.pc); rd(0x100 | CPU.sp); CPU.p = (uint8_t)((cpu_pull() & ~FB) | FU); break;

    /* ── Branches ── */
    case 0x10: branch((CPU.p & FN) == 0); break;
    case 0x30: branch((CPU.p & FN) != 0); break;
    case 0x50: branch((CPU.p & FV) == 0); break;
    case 0x70: branch((CPU.p & FV) != 0); break;
    case 0x90: branch((CPU.p & FC) == 0); break;
    case 0xb0: branch((CPU.p & FC) != 0); break;
    case 0xd0: branch((CPU.p & FZ) == 0); break;
    case 0xf0: branch((CPU.p & FZ) != 0); break;

    case 0xea: rd(CPU.pc); break; /* NOP */

    /* ── Unofficial: NOP variants ── */
    case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        rd(CPU.pc); break;
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        rd(a_imm()); break;
    case 0x04: case 0x44: case 0x64:
        rd(a_zp()); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
        rd(a_zpx()); break;
    case 0x0c:
        rd(a_abs()); break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
        rd(a_absx_r()); break;

    /* ── Unofficial: LAX / SAX ── */
    case 0xa7: CPU.a = CPU.x = cpu_set_zn(rd(a_zp())); break;
    case 0xb7: CPU.a = CPU.x = cpu_set_zn(rd(a_zpy())); break;
    case 0xaf: CPU.a = CPU.x = cpu_set_zn(rd(a_abs())); break;
    case 0xbf: CPU.a = CPU.x = cpu_set_zn(rd(a_absy_r())); break;
    case 0xa3: CPU.a = CPU.x = cpu_set_zn(rd(a_izx())); break;
    case 0xb3: CPU.a = CPU.x = cpu_set_zn(rd(a_izy_r())); break;
    case 0xab: CPU.a = CPU.x = cpu_set_zn(rd(a_imm())); break; /* LXA approx */

    case 0x87: wr(a_zp(), CPU.a & CPU.x); break;
    case 0x97: wr(a_zpy(), CPU.a & CPU.x); break;
    case 0x8f: wr(a_abs(), CPU.a & CPU.x); break;
    case 0x83: wr(a_izx(), CPU.a & CPU.x); break;

    /* ── Unofficial: RMW combos ── */
    case 0xc7: cpu_cmp(CPU.a, rmw(a_zp(), fn_dec_raw)); break; /* DCP */
    case 0xd7: cpu_cmp(CPU.a, rmw(a_zpx(), fn_dec_raw)); break;
    case 0xcf: cpu_cmp(CPU.a, rmw(a_abs(), fn_dec_raw)); break;
    case 0xdf: cpu_cmp(CPU.a, rmw(a_absx_w(), fn_dec_raw)); break;
    case 0xdb: cpu_cmp(CPU.a, rmw(a_absy_w(), fn_dec_raw)); break;
    case 0xc3: cpu_cmp(CPU.a, rmw(a_izx(), fn_dec_raw)); break;
    case 0xd3: cpu_cmp(CPU.a, rmw(a_izy_w(), fn_dec_raw)); break;

    case 0xe7: cpu_sbc(rmw(a_zp(), fn_inc_raw)); break; /* ISC */
    case 0xf7: cpu_sbc(rmw(a_zpx(), fn_inc_raw)); break;
    case 0xef: cpu_sbc(rmw(a_abs(), fn_inc_raw)); break;
    case 0xff: cpu_sbc(rmw(a_absx_w(), fn_inc_raw)); break;
    case 0xfb: cpu_sbc(rmw(a_absy_w(), fn_inc_raw)); break;
    case 0xe3: cpu_sbc(rmw(a_izx(), fn_inc_raw)); break;
    case 0xf3: cpu_sbc(rmw(a_izy_w(), fn_inc_raw)); break;

    case 0x07: CPU.a = cpu_set_zn(CPU.a | rmw(a_zp(), cpu_asl)); break; /* SLO */
    case 0x17: CPU.a = cpu_set_zn(CPU.a | rmw(a_zpx(), cpu_asl)); break;
    case 0x0f: CPU.a = cpu_set_zn(CPU.a | rmw(a_abs(), cpu_asl)); break;
    case 0x1f: CPU.a = cpu_set_zn(CPU.a | rmw(a_absx_w(), cpu_asl)); break;
    case 0x1b: CPU.a = cpu_set_zn(CPU.a | rmw(a_absy_w(), cpu_asl)); break;
    case 0x03: CPU.a = cpu_set_zn(CPU.a | rmw(a_izx(), cpu_asl)); break;
    case 0x13: CPU.a = cpu_set_zn(CPU.a | rmw(a_izy_w(), cpu_asl)); break;

    case 0x27: CPU.a = cpu_set_zn(CPU.a & rmw(a_zp(), cpu_rol)); break; /* RLA */
    case 0x37: CPU.a = cpu_set_zn(CPU.a & rmw(a_zpx(), cpu_rol)); break;
    case 0x2f: CPU.a = cpu_set_zn(CPU.a & rmw(a_abs(), cpu_rol)); break;
    case 0x3f: CPU.a = cpu_set_zn(CPU.a & rmw(a_absx_w(), cpu_rol)); break;
    case 0x3b: CPU.a = cpu_set_zn(CPU.a & rmw(a_absy_w(), cpu_rol)); break;
    case 0x23: CPU.a = cpu_set_zn(CPU.a & rmw(a_izx(), cpu_rol)); break;
    case 0x33: CPU.a = cpu_set_zn(CPU.a & rmw(a_izy_w(), cpu_rol)); break;

    case 0x47: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_zp(), cpu_lsr)); break; /* SRE */
    case 0x57: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_zpx(), cpu_lsr)); break;
    case 0x4f: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_abs(), cpu_lsr)); break;
    case 0x5f: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_absx_w(), cpu_lsr)); break;
    case 0x5b: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_absy_w(), cpu_lsr)); break;
    case 0x43: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_izx(), cpu_lsr)); break;
    case 0x53: CPU.a = cpu_set_zn(CPU.a ^ rmw(a_izy_w(), cpu_lsr)); break;

    case 0x67: cpu_adc(rmw(a_zp(), cpu_ror)); break; /* RRA */
    case 0x77: cpu_adc(rmw(a_zpx(), cpu_ror)); break;
    case 0x6f: cpu_adc(rmw(a_abs(), cpu_ror)); break;
    case 0x7f: cpu_adc(rmw(a_absx_w(), cpu_ror)); break;
    case 0x7b: cpu_adc(rmw(a_absy_w(), cpu_ror)); break;
    case 0x63: cpu_adc(rmw(a_izx(), cpu_ror)); break;
    case 0x73: cpu_adc(rmw(a_izy_w(), cpu_ror)); break;

    /* ── Unofficial: immediate ALU oddities ── */
    case 0x0b: case 0x2b: { /* ANC */
        CPU.a = cpu_set_zn(CPU.a & rd(a_imm()));
        CPU.p = (uint8_t)((CPU.p & ~FC) | ((CPU.a & 0x80) ? FC : 0));
        break;
    }
    case 0x4b: { /* ALR */
        CPU.a = cpu_set_zn(CPU.a & rd(a_imm()));
        CPU.a = cpu_lsr(CPU.a);
        break;
    }
    case 0x6b: { /* ARR */
        uint8_t v = CPU.a & rd(a_imm());
        uint8_t r = (uint8_t)((v >> 1) | ((CPU.p & FC) << 7));
        CPU.a = cpu_set_zn(r);
        CPU.p = (uint8_t)((CPU.p & ~(FC | FV)) | ((r & 0x40) ? FC : 0) |
                          ((((r >> 6) ^ (r >> 5)) & 1) ? FV : 0));
        break;
    }
    case 0xcb: { /* SBX */
        uint8_t v = rd(a_imm());
        int t = (CPU.a & CPU.x) - v;
        CPU.p = (uint8_t)((CPU.p & ~FC) | ((CPU.a & CPU.x) >= v ? FC : 0));
        CPU.x = cpu_set_zn((uint8_t)t);
        break;
    }
    case 0x8b: { /* XAA approx */
        CPU.a = cpu_set_zn(CPU.x & rd(a_imm()));
        break;
    }
    case 0xbb: { /* LAS */
        uint8_t v = rd(a_absy_r()) & CPU.sp;
        CPU.a = CPU.x = CPU.sp = cpu_set_zn(v);
        break;
    }
    /* SHA/SHX/SHY/TAS — standard "& (high+1)" approximation */
    case 0x9f: { uint16_t a = a_absy_w(); wr(a, CPU.a & CPU.x & (uint8_t)((a >> 8) + 1)); break; }
    case 0x93: { uint16_t a = a_izy_w(); wr(a, CPU.a & CPU.x & (uint8_t)((a >> 8) + 1)); break; }
    case 0x9e: { uint16_t a = a_absy_w(); wr(a, CPU.x & (uint8_t)((a >> 8) + 1)); break; }
    case 0x9c: { uint16_t a = a_absx_w(); wr(a, CPU.y & (uint8_t)((a >> 8) + 1)); break; }
    case 0x9b: { uint16_t a = a_absy_w(); CPU.sp = CPU.a & CPU.x; wr(a, CPU.sp & (uint8_t)((a >> 8) + 1)); break; }

    /* ── JAM ── */
    default:
        CPU.jammed = true;
        CPU.pc -= 1;
        break;
    }
}
