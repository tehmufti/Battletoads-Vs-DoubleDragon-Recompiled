/*
 * cpu.js — cycle-accurate 6502 (2A03) core.
 *
 * Every CPU cycle is exactly one bus access. The machine's read()/write()
 * advance the master clock (3 PPU dots + 1 APU cycle) per call, so all
 * dummy reads/writes that real hardware performs are emitted here in the
 * correct order. This makes mid-instruction PPU/APU state visible at the
 * right cycle — required for timing-critical games like Battletoads&DD.
 *
 * Interrupts: NMI is edge-triggered (latched on rising line), IRQ is level
 * sensitive, both polled between instructions. The service sequence is the
 * hardware 7-cycle sequence.
 *
 * Unofficial opcodes implemented (stable set): NOPs, LAX, SAX, DCP, ISC,
 * SLO, RLA, SRE, RRA, ANC, ALR, ARR, SBX, USBC, LAS, SHA/SHX/SHY/TAS
 * (approximated), XAA (approximated). JAM opcodes set `jammed`.
 */

const C = 0x01, Z = 0x02, I = 0x04, D = 0x08, B = 0x10, U = 0x20, V = 0x40, N = 0x80;

export class Cpu {
  constructor(machine) {
    this.m = machine;
    this.a = 0; this.x = 0; this.y = 0;
    this.sp = 0xfd; this.pc = 0;
    this.p = I | U;
    this.nmiLine = false;
    this.nmiPending = false;
    this.jammed = false;
    this.cycles = 0; // informational; machine tracks real time
  }

  // ── Interrupt lines ────────────────────────────────────────────────────────
  setNmiLine(level) {
    if (level && !this.nmiLine) this.nmiPending = true;
    this.nmiLine = level;
  }

  // ── Bus helpers (each call = one CPU cycle) ────────────────────────────────
  rd(a) { this.cycles += 1; return this.m.read(a & 0xffff); }
  wr(a, v) { this.cycles += 1; this.m.write(a & 0xffff, v & 0xff); }

  fetch() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xffff; return v; }

  push(v) { this.wr(0x100 | this.sp, v); this.sp = (this.sp - 1) & 0xff; }
  pull() { this.sp = (this.sp + 1) & 0xff; return this.rd(0x100 | this.sp); }

  // ── Flag helpers ───────────────────────────────────────────────────────────
  setZN(v) {
    this.p = (this.p & ~(Z | N)) | (v === 0 ? Z : 0) | (v & 0x80);
    return v;
  }

  // ── Reset / interrupts ─────────────────────────────────────────────────────
  reset() {
    this.jammed = false;
    this.sp = 0xfd;
    this.p |= I;
    // 7-cycle reset sequence approximation (vector fetch is what matters)
    this.rd(this.pc); this.rd(this.pc); this.rd(0x100 | this.sp);
    this.rd(0x100 | ((this.sp - 1) & 0xff)); this.rd(0x100 | ((this.sp - 2) & 0xff));
    const lo = this.rd(0xfffc);
    const hi = this.rd(0xfffd);
    this.pc = lo | (hi << 8);
  }

  service(vector) {
    this.rd(this.pc); this.rd(this.pc);
    this.push(this.pc >> 8); this.push(this.pc & 0xff);
    this.push((this.p & ~B) | U);
    this.p |= I;
    const lo = this.rd(vector);
    const hi = this.rd(vector + 1);
    this.pc = lo | (hi << 8);
  }

  // ── Addressing modes ──────────────────────────────────────────────────────
  // Each returns the effective address, having burnt exactly the right cycles.
  aImm() { const a = this.pc; this.pc = (this.pc + 1) & 0xffff; this.cycles += 0; return a; }
  aZp() { return this.fetch(); }
  aZpX() { const z = this.fetch(); this.rd(z); return (z + this.x) & 0xff; }
  aZpY() { const z = this.fetch(); this.rd(z); return (z + this.y) & 0xff; }
  aAbs() { const lo = this.fetch(); return lo | (this.fetch() << 8); }
  aAbsXr() { // read variant: +1 cycle only on page cross
    const lo = this.fetch(); const hi = this.fetch();
    const base = lo | (hi << 8); const eff = (base + this.x) & 0xffff;
    if ((lo + this.x) > 0xff) this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }
  aAbsXw() { // write/RMW variant: dummy read always
    const lo = this.fetch(); const hi = this.fetch();
    const base = lo | (hi << 8); const eff = (base + this.x) & 0xffff;
    this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }
  aAbsYr() {
    const lo = this.fetch(); const hi = this.fetch();
    const base = lo | (hi << 8); const eff = (base + this.y) & 0xffff;
    if ((lo + this.y) > 0xff) this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }
  aAbsYw() {
    const lo = this.fetch(); const hi = this.fetch();
    const base = lo | (hi << 8); const eff = (base + this.y) & 0xffff;
    this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }
  aIzx() {
    const z = this.fetch(); this.rd(z);
    const lo = this.rd((z + this.x) & 0xff);
    const hi = this.rd((z + this.x + 1) & 0xff);
    return lo | (hi << 8);
  }
  aIzyR() {
    const z = this.fetch();
    const lo = this.rd(z); const hi = this.rd((z + 1) & 0xff);
    const base = lo | (hi << 8); const eff = (base + this.y) & 0xffff;
    if ((lo + this.y) > 0xff) this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }
  aIzyW() {
    const z = this.fetch();
    const lo = this.rd(z); const hi = this.rd((z + 1) & 0xff);
    const base = lo | (hi << 8); const eff = (base + this.y) & 0xffff;
    this.rd((base & 0xff00) | (eff & 0xff));
    return eff;
  }

  // ── ALU ops ────────────────────────────────────────────────────────────────
  adcVal(v) {
    const sum = this.a + v + (this.p & C);
    const r = sum & 0xff;
    this.p = (this.p & ~(C | V)) | (sum > 0xff ? C : 0) | (((~(this.a ^ v) & (this.a ^ r)) & 0x80) ? V : 0);
    this.a = this.setZN(r);
  }
  sbcVal(v) { this.adcVal(v ^ 0xff); }
  cmpVal(reg, v) {
    const r = (reg - v) & 0xff;
    this.p = (this.p & ~C) | (reg >= v ? C : 0);
    this.setZN(r);
  }
  aslVal(v) { this.p = (this.p & ~C) | ((v >> 7) & 1); return this.setZN((v << 1) & 0xff); }
  lsrVal(v) { this.p = (this.p & ~C) | (v & 1); return this.setZN(v >> 1); }
  rolVal(v) { const c = this.p & C; this.p = (this.p & ~C) | ((v >> 7) & 1); return this.setZN(((v << 1) | c) & 0xff); }
  rorVal(v) { const c = (this.p & C) << 7; this.p = (this.p & ~C) | (v & 1); return this.setZN((v >> 1) | c); }
  bitVal(v) {
    this.p = (this.p & ~(Z | V | N)) | ((this.a & v) === 0 ? Z : 0) | (v & (V | N));
  }

  rmw(addr, fn) {
    const v = this.rd(addr);
    this.wr(addr, v);          // dummy write of old value
    const r = fn(v);
    this.wr(addr, r);
    return r;
  }

  branch(cond) {
    const off = this.fetch();
    if (!cond) return;
    this.rd(this.pc); // taken: dummy fetch
    const target = (this.pc + (off < 0x80 ? off : off - 0x100)) & 0xffff;
    if ((target & 0xff00) !== (this.pc & 0xff00)) {
      this.rd((this.pc & 0xff00) | (target & 0xff)); // page-cross fixup
    }
    this.pc = target;
  }

  // ── Main step: one instruction (or interrupt service) ─────────────────────
  step() {
    if (this.jammed) { this.cycles += 1; this.m.tickJammed(); return; }

    if (this.nmiPending) {
      this.nmiPending = false;
      this.service(0xfffa);
      return;
    }
    if (this.m.irqLine !== 0 && (this.p & I) === 0) {
      this.service(0xfffe);
      return;
    }

    if (this.m.covMark) this.m.covMark(this.pc);
    const op = this.fetch();
    switch (op) {
      // ── Loads / stores ──
      case 0xa9: this.a = this.setZN(this.rd(this.aImm())); break;
      case 0xa5: this.a = this.setZN(this.rd(this.aZp())); break;
      case 0xb5: this.a = this.setZN(this.rd(this.aZpX())); break;
      case 0xad: this.a = this.setZN(this.rd(this.aAbs())); break;
      case 0xbd: this.a = this.setZN(this.rd(this.aAbsXr())); break;
      case 0xb9: this.a = this.setZN(this.rd(this.aAbsYr())); break;
      case 0xa1: this.a = this.setZN(this.rd(this.aIzx())); break;
      case 0xb1: this.a = this.setZN(this.rd(this.aIzyR())); break;

      case 0xa2: this.x = this.setZN(this.rd(this.aImm())); break;
      case 0xa6: this.x = this.setZN(this.rd(this.aZp())); break;
      case 0xb6: this.x = this.setZN(this.rd(this.aZpY())); break;
      case 0xae: this.x = this.setZN(this.rd(this.aAbs())); break;
      case 0xbe: this.x = this.setZN(this.rd(this.aAbsYr())); break;

      case 0xa0: this.y = this.setZN(this.rd(this.aImm())); break;
      case 0xa4: this.y = this.setZN(this.rd(this.aZp())); break;
      case 0xb4: this.y = this.setZN(this.rd(this.aZpX())); break;
      case 0xac: this.y = this.setZN(this.rd(this.aAbs())); break;
      case 0xbc: this.y = this.setZN(this.rd(this.aAbsXr())); break;

      case 0x85: this.wr(this.aZp(), this.a); break;
      case 0x95: this.wr(this.aZpX(), this.a); break;
      case 0x8d: this.wr(this.aAbs(), this.a); break;
      case 0x9d: this.wr(this.aAbsXw(), this.a); break;
      case 0x99: this.wr(this.aAbsYw(), this.a); break;
      case 0x81: this.wr(this.aIzx(), this.a); break;
      case 0x91: this.wr(this.aIzyW(), this.a); break;

      case 0x86: this.wr(this.aZp(), this.x); break;
      case 0x96: this.wr(this.aZpY(), this.x); break;
      case 0x8e: this.wr(this.aAbs(), this.x); break;

      case 0x84: this.wr(this.aZp(), this.y); break;
      case 0x94: this.wr(this.aZpX(), this.y); break;
      case 0x8c: this.wr(this.aAbs(), this.y); break;

      // ── Transfers ──
      case 0xaa: this.rd(this.pc); this.x = this.setZN(this.a); break;
      case 0xa8: this.rd(this.pc); this.y = this.setZN(this.a); break;
      case 0x8a: this.rd(this.pc); this.a = this.setZN(this.x); break;
      case 0x98: this.rd(this.pc); this.a = this.setZN(this.y); break;
      case 0xba: this.rd(this.pc); this.x = this.setZN(this.sp); break;
      case 0x9a: this.rd(this.pc); this.sp = this.x; break;

      // ── ALU ──
      case 0x69: this.adcVal(this.rd(this.aImm())); break;
      case 0x65: this.adcVal(this.rd(this.aZp())); break;
      case 0x75: this.adcVal(this.rd(this.aZpX())); break;
      case 0x6d: this.adcVal(this.rd(this.aAbs())); break;
      case 0x7d: this.adcVal(this.rd(this.aAbsXr())); break;
      case 0x79: this.adcVal(this.rd(this.aAbsYr())); break;
      case 0x61: this.adcVal(this.rd(this.aIzx())); break;
      case 0x71: this.adcVal(this.rd(this.aIzyR())); break;

      case 0xe9: case 0xeb: this.sbcVal(this.rd(this.aImm())); break;
      case 0xe5: this.sbcVal(this.rd(this.aZp())); break;
      case 0xf5: this.sbcVal(this.rd(this.aZpX())); break;
      case 0xed: this.sbcVal(this.rd(this.aAbs())); break;
      case 0xfd: this.sbcVal(this.rd(this.aAbsXr())); break;
      case 0xf9: this.sbcVal(this.rd(this.aAbsYr())); break;
      case 0xe1: this.sbcVal(this.rd(this.aIzx())); break;
      case 0xf1: this.sbcVal(this.rd(this.aIzyR())); break;

      case 0x29: this.a = this.setZN(this.a & this.rd(this.aImm())); break;
      case 0x25: this.a = this.setZN(this.a & this.rd(this.aZp())); break;
      case 0x35: this.a = this.setZN(this.a & this.rd(this.aZpX())); break;
      case 0x2d: this.a = this.setZN(this.a & this.rd(this.aAbs())); break;
      case 0x3d: this.a = this.setZN(this.a & this.rd(this.aAbsXr())); break;
      case 0x39: this.a = this.setZN(this.a & this.rd(this.aAbsYr())); break;
      case 0x21: this.a = this.setZN(this.a & this.rd(this.aIzx())); break;
      case 0x31: this.a = this.setZN(this.a & this.rd(this.aIzyR())); break;

      case 0x09: this.a = this.setZN(this.a | this.rd(this.aImm())); break;
      case 0x05: this.a = this.setZN(this.a | this.rd(this.aZp())); break;
      case 0x15: this.a = this.setZN(this.a | this.rd(this.aZpX())); break;
      case 0x0d: this.a = this.setZN(this.a | this.rd(this.aAbs())); break;
      case 0x1d: this.a = this.setZN(this.a | this.rd(this.aAbsXr())); break;
      case 0x19: this.a = this.setZN(this.a | this.rd(this.aAbsYr())); break;
      case 0x01: this.a = this.setZN(this.a | this.rd(this.aIzx())); break;
      case 0x11: this.a = this.setZN(this.a | this.rd(this.aIzyR())); break;

      case 0x49: this.a = this.setZN(this.a ^ this.rd(this.aImm())); break;
      case 0x45: this.a = this.setZN(this.a ^ this.rd(this.aZp())); break;
      case 0x55: this.a = this.setZN(this.a ^ this.rd(this.aZpX())); break;
      case 0x4d: this.a = this.setZN(this.a ^ this.rd(this.aAbs())); break;
      case 0x5d: this.a = this.setZN(this.a ^ this.rd(this.aAbsXr())); break;
      case 0x59: this.a = this.setZN(this.a ^ this.rd(this.aAbsYr())); break;
      case 0x41: this.a = this.setZN(this.a ^ this.rd(this.aIzx())); break;
      case 0x51: this.a = this.setZN(this.a ^ this.rd(this.aIzyR())); break;

      case 0xc9: this.cmpVal(this.a, this.rd(this.aImm())); break;
      case 0xc5: this.cmpVal(this.a, this.rd(this.aZp())); break;
      case 0xd5: this.cmpVal(this.a, this.rd(this.aZpX())); break;
      case 0xcd: this.cmpVal(this.a, this.rd(this.aAbs())); break;
      case 0xdd: this.cmpVal(this.a, this.rd(this.aAbsXr())); break;
      case 0xd9: this.cmpVal(this.a, this.rd(this.aAbsYr())); break;
      case 0xc1: this.cmpVal(this.a, this.rd(this.aIzx())); break;
      case 0xd1: this.cmpVal(this.a, this.rd(this.aIzyR())); break;

      case 0xe0: this.cmpVal(this.x, this.rd(this.aImm())); break;
      case 0xe4: this.cmpVal(this.x, this.rd(this.aZp())); break;
      case 0xec: this.cmpVal(this.x, this.rd(this.aAbs())); break;
      case 0xc0: this.cmpVal(this.y, this.rd(this.aImm())); break;
      case 0xc4: this.cmpVal(this.y, this.rd(this.aZp())); break;
      case 0xcc: this.cmpVal(this.y, this.rd(this.aAbs())); break;

      case 0x24: this.bitVal(this.rd(this.aZp())); break;
      case 0x2c: this.bitVal(this.rd(this.aAbs())); break;

      // ── Shifts / rotates ──
      case 0x0a: this.rd(this.pc); this.a = this.aslVal(this.a); break;
      case 0x06: this.rmw(this.aZp(), (v) => this.aslVal(v)); break;
      case 0x16: this.rmw(this.aZpX(), (v) => this.aslVal(v)); break;
      case 0x0e: this.rmw(this.aAbs(), (v) => this.aslVal(v)); break;
      case 0x1e: this.rmw(this.aAbsXw(), (v) => this.aslVal(v)); break;

      case 0x4a: this.rd(this.pc); this.a = this.lsrVal(this.a); break;
      case 0x46: this.rmw(this.aZp(), (v) => this.lsrVal(v)); break;
      case 0x56: this.rmw(this.aZpX(), (v) => this.lsrVal(v)); break;
      case 0x4e: this.rmw(this.aAbs(), (v) => this.lsrVal(v)); break;
      case 0x5e: this.rmw(this.aAbsXw(), (v) => this.lsrVal(v)); break;

      case 0x2a: this.rd(this.pc); this.a = this.rolVal(this.a); break;
      case 0x26: this.rmw(this.aZp(), (v) => this.rolVal(v)); break;
      case 0x36: this.rmw(this.aZpX(), (v) => this.rolVal(v)); break;
      case 0x2e: this.rmw(this.aAbs(), (v) => this.rolVal(v)); break;
      case 0x3e: this.rmw(this.aAbsXw(), (v) => this.rolVal(v)); break;

      case 0x6a: this.rd(this.pc); this.a = this.rorVal(this.a); break;
      case 0x66: this.rmw(this.aZp(), (v) => this.rorVal(v)); break;
      case 0x76: this.rmw(this.aZpX(), (v) => this.rorVal(v)); break;
      case 0x6e: this.rmw(this.aAbs(), (v) => this.rorVal(v)); break;
      case 0x7e: this.rmw(this.aAbsXw(), (v) => this.rorVal(v)); break;

      // ── Inc / dec ──
      case 0xe6: this.rmw(this.aZp(), (v) => this.setZN((v + 1) & 0xff)); break;
      case 0xf6: this.rmw(this.aZpX(), (v) => this.setZN((v + 1) & 0xff)); break;
      case 0xee: this.rmw(this.aAbs(), (v) => this.setZN((v + 1) & 0xff)); break;
      case 0xfe: this.rmw(this.aAbsXw(), (v) => this.setZN((v + 1) & 0xff)); break;
      case 0xc6: this.rmw(this.aZp(), (v) => this.setZN((v - 1) & 0xff)); break;
      case 0xd6: this.rmw(this.aZpX(), (v) => this.setZN((v - 1) & 0xff)); break;
      case 0xce: this.rmw(this.aAbs(), (v) => this.setZN((v - 1) & 0xff)); break;
      case 0xde: this.rmw(this.aAbsXw(), (v) => this.setZN((v - 1) & 0xff)); break;
      case 0xe8: this.rd(this.pc); this.x = this.setZN((this.x + 1) & 0xff); break;
      case 0xc8: this.rd(this.pc); this.y = this.setZN((this.y + 1) & 0xff); break;
      case 0xca: this.rd(this.pc); this.x = this.setZN((this.x - 1) & 0xff); break;
      case 0x88: this.rd(this.pc); this.y = this.setZN((this.y - 1) & 0xff); break;

      // ── Flags ──
      case 0x18: this.rd(this.pc); this.p &= ~C; break;
      case 0x38: this.rd(this.pc); this.p |= C; break;
      case 0x58: this.rd(this.pc); this.p &= ~I; break;
      case 0x78: this.rd(this.pc); this.p |= I; break;
      case 0xb8: this.rd(this.pc); this.p &= ~V; break;
      case 0xd8: this.rd(this.pc); this.p &= ~D; break;
      case 0xf8: this.rd(this.pc); this.p |= D; break;

      // ── Jumps / subroutines ──
      case 0x4c: { const lo = this.fetch(); const hi = this.fetch(); this.pc = lo | (hi << 8); break; }
      case 0x6c: {
        const lo = this.fetch(); const hi = this.fetch();
        const ptr = lo | (hi << 8);
        const tlo = this.rd(ptr);
        const thi = this.rd((ptr & 0xff00) | ((ptr + 1) & 0xff)); // 6502 page-wrap bug
        this.pc = tlo | (thi << 8);
        break;
      }
      case 0x20: {
        const lo = this.fetch();
        this.rd(0x100 | this.sp); // internal
        this.push(this.pc >> 8); this.push(this.pc & 0xff);
        const hi = this.rd(this.pc); // fetch hi (pc not incremented past it on hardware; target replaces pc)
        this.pc = lo | (hi << 8);
        break;
      }
      case 0x60: {
        this.rd(this.pc);
        this.rd(0x100 | this.sp);
        const lo = this.pull(); const hi = this.pull();
        this.pc = (lo | (hi << 8));
        this.rd(this.pc);
        this.pc = (this.pc + 1) & 0xffff;
        break;
      }
      case 0x40: {
        this.rd(this.pc);
        this.rd(0x100 | this.sp);
        this.p = (this.pull() & ~B) | U;
        const lo = this.pull(); const hi = this.pull();
        this.pc = lo | (hi << 8);
        break;
      }
      case 0x00: { // BRK
        this.fetch(); // padding byte
        this.push(this.pc >> 8); this.push(this.pc & 0xff);
        this.push(this.p | B | U);
        this.p |= I;
        const lo = this.rd(0xfffe); const hi = this.rd(0xffff);
        this.pc = lo | (hi << 8);
        break;
      }

      // ── Stack ──
      case 0x48: this.rd(this.pc); this.push(this.a); break;
      case 0x08: this.rd(this.pc); this.push(this.p | B | U); break;
      case 0x68: this.rd(this.pc); this.rd(0x100 | this.sp); this.a = this.setZN(this.pull()); break;
      case 0x28: this.rd(this.pc); this.rd(0x100 | this.sp); this.p = (this.pull() & ~B) | U; break;

      // ── Branches ──
      case 0x10: this.branch((this.p & N) === 0); break;
      case 0x30: this.branch((this.p & N) !== 0); break;
      case 0x50: this.branch((this.p & V) === 0); break;
      case 0x70: this.branch((this.p & V) !== 0); break;
      case 0x90: this.branch((this.p & C) === 0); break;
      case 0xb0: this.branch((this.p & C) !== 0); break;
      case 0xd0: this.branch((this.p & Z) === 0); break;
      case 0xf0: this.branch((this.p & Z) !== 0); break;

      case 0xea: this.rd(this.pc); break; // NOP

      // ── Unofficial: NOP variants ──
      case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        this.rd(this.pc); break;
      case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        this.rd(this.aImm()); break;
      case 0x04: case 0x44: case 0x64:
        this.rd(this.aZp()); break;
      case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
        this.rd(this.aZpX()); break;
      case 0x0c:
        this.rd(this.aAbs()); break;
      case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
        this.rd(this.aAbsXr()); break;

      // ── Unofficial: LAX / SAX ──
      case 0xa7: this.a = this.x = this.setZN(this.rd(this.aZp())); break;
      case 0xb7: this.a = this.x = this.setZN(this.rd(this.aZpY())); break;
      case 0xaf: this.a = this.x = this.setZN(this.rd(this.aAbs())); break;
      case 0xbf: this.a = this.x = this.setZN(this.rd(this.aAbsYr())); break;
      case 0xa3: this.a = this.x = this.setZN(this.rd(this.aIzx())); break;
      case 0xb3: this.a = this.x = this.setZN(this.rd(this.aIzyR())); break;
      case 0xab: this.a = this.x = this.setZN(this.rd(this.aImm())); break; // LXA approx

      case 0x87: this.wr(this.aZp(), this.a & this.x); break;
      case 0x97: this.wr(this.aZpY(), this.a & this.x); break;
      case 0x8f: this.wr(this.aAbs(), this.a & this.x); break;
      case 0x83: this.wr(this.aIzx(), this.a & this.x); break;

      // ── Unofficial: RMW combos ──
      case 0xc7: this.cmpVal(this.a, this.rmw(this.aZp(), (v) => (v - 1) & 0xff)); break; // DCP
      case 0xd7: this.cmpVal(this.a, this.rmw(this.aZpX(), (v) => (v - 1) & 0xff)); break;
      case 0xcf: this.cmpVal(this.a, this.rmw(this.aAbs(), (v) => (v - 1) & 0xff)); break;
      case 0xdf: this.cmpVal(this.a, this.rmw(this.aAbsXw(), (v) => (v - 1) & 0xff)); break;
      case 0xdb: this.cmpVal(this.a, this.rmw(this.aAbsYw(), (v) => (v - 1) & 0xff)); break;
      case 0xc3: this.cmpVal(this.a, this.rmw(this.aIzx(), (v) => (v - 1) & 0xff)); break;
      case 0xd3: this.cmpVal(this.a, this.rmw(this.aIzyW(), (v) => (v - 1) & 0xff)); break;

      case 0xe7: this.sbcVal(this.rmw(this.aZp(), (v) => (v + 1) & 0xff)); break; // ISC
      case 0xf7: this.sbcVal(this.rmw(this.aZpX(), (v) => (v + 1) & 0xff)); break;
      case 0xef: this.sbcVal(this.rmw(this.aAbs(), (v) => (v + 1) & 0xff)); break;
      case 0xff: this.sbcVal(this.rmw(this.aAbsXw(), (v) => (v + 1) & 0xff)); break;
      case 0xfb: this.sbcVal(this.rmw(this.aAbsYw(), (v) => (v + 1) & 0xff)); break;
      case 0xe3: this.sbcVal(this.rmw(this.aIzx(), (v) => (v + 1) & 0xff)); break;
      case 0xf3: this.sbcVal(this.rmw(this.aIzyW(), (v) => (v + 1) & 0xff)); break;

      case 0x07: this.a = this.setZN(this.a | this.rmw(this.aZp(), (v) => this.aslVal(v))); break; // SLO
      case 0x17: this.a = this.setZN(this.a | this.rmw(this.aZpX(), (v) => this.aslVal(v))); break;
      case 0x0f: this.a = this.setZN(this.a | this.rmw(this.aAbs(), (v) => this.aslVal(v))); break;
      case 0x1f: this.a = this.setZN(this.a | this.rmw(this.aAbsXw(), (v) => this.aslVal(v))); break;
      case 0x1b: this.a = this.setZN(this.a | this.rmw(this.aAbsYw(), (v) => this.aslVal(v))); break;
      case 0x03: this.a = this.setZN(this.a | this.rmw(this.aIzx(), (v) => this.aslVal(v))); break;
      case 0x13: this.a = this.setZN(this.a | this.rmw(this.aIzyW(), (v) => this.aslVal(v))); break;

      case 0x27: this.a = this.setZN(this.a & this.rmw(this.aZp(), (v) => this.rolVal(v))); break; // RLA
      case 0x37: this.a = this.setZN(this.a & this.rmw(this.aZpX(), (v) => this.rolVal(v))); break;
      case 0x2f: this.a = this.setZN(this.a & this.rmw(this.aAbs(), (v) => this.rolVal(v))); break;
      case 0x3f: this.a = this.setZN(this.a & this.rmw(this.aAbsXw(), (v) => this.rolVal(v))); break;
      case 0x3b: this.a = this.setZN(this.a & this.rmw(this.aAbsYw(), (v) => this.rolVal(v))); break;
      case 0x23: this.a = this.setZN(this.a & this.rmw(this.aIzx(), (v) => this.rolVal(v))); break;
      case 0x33: this.a = this.setZN(this.a & this.rmw(this.aIzyW(), (v) => this.rolVal(v))); break;

      case 0x47: this.a = this.setZN(this.a ^ this.rmw(this.aZp(), (v) => this.lsrVal(v))); break; // SRE
      case 0x57: this.a = this.setZN(this.a ^ this.rmw(this.aZpX(), (v) => this.lsrVal(v))); break;
      case 0x4f: this.a = this.setZN(this.a ^ this.rmw(this.aAbs(), (v) => this.lsrVal(v))); break;
      case 0x5f: this.a = this.setZN(this.a ^ this.rmw(this.aAbsXw(), (v) => this.lsrVal(v))); break;
      case 0x5b: this.a = this.setZN(this.a ^ this.rmw(this.aAbsYw(), (v) => this.lsrVal(v))); break;
      case 0x43: this.a = this.setZN(this.a ^ this.rmw(this.aIzx(), (v) => this.lsrVal(v))); break;
      case 0x53: this.a = this.setZN(this.a ^ this.rmw(this.aIzyW(), (v) => this.lsrVal(v))); break;

      case 0x67: this.adcVal(this.rmw(this.aZp(), (v) => this.rorVal(v))); break; // RRA
      case 0x77: this.adcVal(this.rmw(this.aZpX(), (v) => this.rorVal(v))); break;
      case 0x6f: this.adcVal(this.rmw(this.aAbs(), (v) => this.rorVal(v))); break;
      case 0x7f: this.adcVal(this.rmw(this.aAbsXw(), (v) => this.rorVal(v))); break;
      case 0x7b: this.adcVal(this.rmw(this.aAbsYw(), (v) => this.rorVal(v))); break;
      case 0x63: this.adcVal(this.rmw(this.aIzx(), (v) => this.rorVal(v))); break;
      case 0x73: this.adcVal(this.rmw(this.aIzyW(), (v) => this.rorVal(v))); break;

      // ── Unofficial: immediate ALU oddities ──
      case 0x0b: case 0x2b: { // ANC
        this.a = this.setZN(this.a & this.rd(this.aImm()));
        this.p = (this.p & ~C) | ((this.a & 0x80) ? C : 0);
        break;
      }
      case 0x4b: { // ALR
        this.a = this.setZN(this.a & this.rd(this.aImm()));
        this.a = this.lsrVal(this.a);
        break;
      }
      case 0x6b: { // ARR
        const v = this.a & this.rd(this.aImm());
        const r = ((v >> 1) | ((this.p & C) << 7)) & 0xff;
        this.a = this.setZN(r);
        this.p = (this.p & ~(C | V)) | ((r & 0x40) ? C : 0) | ((((r >> 6) ^ (r >> 5)) & 1) ? V : 0);
        break;
      }
      case 0xcb: { // SBX
        const v = this.rd(this.aImm());
        const t = (this.a & this.x) - v;
        this.p = (this.p & ~C) | ((this.a & this.x) >= v ? C : 0);
        this.x = this.setZN(t & 0xff);
        break;
      }
      case 0x8b: { // XAA — unstable; common approximation
        this.a = this.setZN(this.x & this.rd(this.aImm()));
        break;
      }
      case 0xbb: { // LAS
        const v = this.rd(this.aAbsYr()) & this.sp;
        this.a = this.x = this.sp = this.setZN(v);
        break;
      }
      // SHA/SHX/SHY/TAS — approximate with the standard "& (high+1)" rule.
      case 0x9f: { const a = this.aAbsYw(); this.wr(a, this.a & this.x & (((a >> 8) + 1) & 0xff)); break; }
      case 0x93: { const a = this.aIzyW(); this.wr(a, this.a & this.x & (((a >> 8) + 1) & 0xff)); break; }
      case 0x9e: { const a = this.aAbsYw(); this.wr(a, this.x & (((a >> 8) + 1) & 0xff)); break; }
      case 0x9c: { const a = this.aAbsXw(); this.wr(a, this.y & (((a >> 8) + 1) & 0xff)); break; }
      case 0x9b: { const a = this.aAbsYw(); this.sp = this.a & this.x; this.wr(a, this.sp & (((a >> 8) + 1) & 0xff)); break; }

      // ── JAM ──
      default:
        // 0x02,0x12,0x22,0x32,0x42,0x52,0x62,0x72,0x92,0xB2,0xD2,0xF2
        this.jammed = true;
        this.pc = (this.pc - 1) & 0xffff;
        break;
    }
  }
}
