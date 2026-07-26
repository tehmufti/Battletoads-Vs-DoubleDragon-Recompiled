/*
 * nes.js — NES machine: bus, mappers (7/0/2/3), OAM DMA, controllers,
 * master clock, frame loop, save states.
 *
 * Clocking model: every CPU bus access advances the master clock by exactly
 * one CPU cycle = 3 PPU dots + 1 APU cycle, *before* the access is performed.
 * The cycle-accurate CPU emits every access (including dummy ones), so PPU
 * and APU observe register reads/writes at hardware-true times.
 */

import { Cpu } from "./cpu.js";
import { Ppu, MIRROR_HORIZONTAL, MIRROR_VERTICAL, MIRROR_SINGLE0, MIRROR_SINGLE1 } from "./ppu.js";
import { Apu } from "./apu.js";

function parseInes(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.length < 16 || bytes[0] !== 0x4e || bytes[1] !== 0x45 || bytes[2] !== 0x53 || bytes[3] !== 0x1a) {
    throw new Error("Not an iNES ROM");
  }
  const prgBanks16 = bytes[4];
  const chrBanks8 = bytes[5];
  const flags6 = bytes[6];
  const flags7 = bytes[7];
  const mapper = (flags7 & 0xf0) | (flags6 >> 4);
  const trainer = (flags6 & 0x04) ? 512 : 0;
  const prgStart = 16 + trainer;
  const prgSize = prgBanks16 * 0x4000;
  const chrSize = chrBanks8 * 0x2000;
  return {
    mapper,
    mirroring: (flags6 & 0x01) ? "vertical" : "horizontal",
    fourScreen: (flags6 & 0x08) !== 0,
    prg: bytes.slice(prgStart, prgStart + prgSize),
    chr: chrSize > 0 ? bytes.slice(prgStart + prgSize, prgStart + prgSize + chrSize) : null,
  };
}

export class Nes {
  constructor(romBytes, options = {}) {
    const rom = parseInes(romBytes);
    this.mapper = rom.mapper;
    this.prg = rom.prg;
    this.chrRom = rom.chr;
    this.ram = new Uint8Array(0x800);
    this.wram = new Uint8Array(0x2000);

    this.ppu = new Ppu(this);
    this.apu = new Apu(this, options.sampleRate ?? 0);
    this.cpu = new Cpu(this);

    this.irqLine = 0;       // bit0 = APU frame, bit1 = DMC
    this.stall = 0;         // pending DMC stall cycles
    this.cycleCount = 0;    // master CPU cycle counter (for DMA parity)
    this.frameDone = false;
    this.openBus = 0;
    this.covMark = null;    // optional (pc)=>void hook: instruction-start coverage

    // mapper state
    this.prgBank = 0;       // mapper-specific meaning
    this.chrBank = 0;
    this.prgMask32 = (this.prg.length >> 15) - 1;  // for 32K banking
    this.prgMask16 = (this.prg.length >> 14) - 1;  // for 16K banking

    // controllers
    this.pad = [0, 0];        // current button state (A,B,Sel,St,U,D,L,R = bits 0..7)
    this.padLatch = [0, 0];
    this.padIndex = [0, 0];
    this.padStrobe = 0;

    this.initMapper(rom);
    this.reset(true);
  }

  initMapper(rom) {
    switch (this.mapper) {
      case 7:
        this.ppu.mirror = MIRROR_SINGLE0;
        this.prgBank = 0;
        break;
      case 0:
      case 2:
      case 3:
        this.ppu.mirror = rom.mirroring === "vertical" ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
        this.prgBank = 0;
        break;
      default:
        throw new Error(`Mapper ${this.mapper} not supported`);
    }
    if (this.chrRom) {
      this.ppu.chr = this.chrRom.slice(0, 0x2000);
      this.ppu.chrWritable = false;
    }
  }

  reset(power = false) {
    if (power) {
      this.ram.fill(0);
      this.ppu.chr.fill?.(0);
    }
    this.ppu.reset();
    this.irqLine = 0;
    this.stall = 0;
    this.frameDone = false;
    this.prgBank = this.mapper === 7 ? 0 : this.prgBank;
    if (this.mapper === 7) this.ppu.mirror = MIRROR_SINGLE0;
    this.cpu.nmiLine = false;
    this.cpu.nmiPending = false;
    this.cpu.reset();
  }

  // ── Master clock ───────────────────────────────────────────────────────────
  tickUnits() {
    const p = this.ppu;
    p.tick(); p.tick(); p.tick();
    this.apu.tick();
    this.cycleCount += 1;
  }

  tickJammed() { this.tickUnits(); }

  // CPU bus access entry points (each = one CPU cycle)
  read(addr) {
    if (this.stall > 0) {
      do { this.stall -= 1; this.tickUnits(); } while (this.stall > 0);
    }
    this.tickUnits();
    const v = this.busRead(addr);
    this.openBus = v;
    return v;
  }

  write(addr, value) {
    this.tickUnits();
    this.openBus = value;
    this.busWrite(addr, value);
  }

  busRead(addr) {
    if (addr < 0x2000) return this.ram[addr & 0x7ff];
    if (addr < 0x4000) return this.ppu.readReg(addr & 7);
    if (addr < 0x4020) {
      if (addr === 0x4015) return this.apu.read4015();
      if (addr === 0x4016) return this.readPad(0);
      if (addr === 0x4017) return this.readPad(1);
      return this.openBus;
    }
    if (addr < 0x6000) return this.openBus;
    if (addr < 0x8000) return this.wram[addr - 0x6000];
    return this.prgRead(addr);
  }

  busWrite(addr, value) {
    if (addr < 0x2000) { this.ram[addr & 0x7ff] = value; return; }
    if (addr < 0x4000) { this.ppu.writeReg(addr & 7, value); return; }
    if (addr < 0x4020) {
      if (addr === 0x4014) { this.oamDma(value); return; }
      if (addr === 0x4016) {
        const prev = this.padStrobe;
        this.padStrobe = value & 1;
        if (this.padStrobe) {
          this.padLatch[0] = this.pad[0];
          this.padLatch[1] = this.pad[1];
          this.padIndex[0] = 0;
          this.padIndex[1] = 0;
        } else if (prev === 1) {
          this.padLatch[0] = this.pad[0];
          this.padLatch[1] = this.pad[1];
          this.padIndex[0] = 0;
          this.padIndex[1] = 0;
        }
        return;
      }
      if (addr <= 0x4013 || addr === 0x4015 || addr === 0x4017) {
        this.apu.write(addr, value);
      }
      return;
    }
    if (addr < 0x6000) return;
    if (addr < 0x8000) { this.wram[addr - 0x6000] = value; return; }
    this.mapperWrite(addr, value);
  }

  // ── PRG mapping ────────────────────────────────────────────────────────────
  prgRead(addr) {
    switch (this.mapper) {
      case 7:
        return this.prg[((this.prgBank & this.prgMask32) << 15) | (addr & 0x7fff)];
      case 2: {
        if (addr < 0xc000) {
          return this.prg[((this.prgBank & this.prgMask16) << 14) | (addr & 0x3fff)];
        }
        return this.prg[(this.prgMask16 << 14) | (addr & 0x3fff)];
      }
      default: { // 0, 3: fixed PRG (16K mirrored or 32K)
        return this.prg[(addr - 0x8000) & (this.prg.length - 1)];
      }
    }
  }

  mapperWrite(addr, value) {
    switch (this.mapper) {
      case 7:
        this.prgBank = value & 0x07;
        this.ppu.mirror = (value & 0x10) ? MIRROR_SINGLE1 : MIRROR_SINGLE0;
        break;
      case 2:
        this.prgBank = value;
        break;
      case 3:
        if (this.chrRom) {
          const bank = (value & 0x03) % Math.max(1, this.chrRom.length >> 13);
          this.ppu.chr = this.chrRom.slice(bank << 13, (bank << 13) + 0x2000);
        }
        break;
      default:
        break;
    }
  }

  // DMC sample fetch: direct PRG read + 4-cycle CPU stall
  dmcFetch(addr) {
    this.stall += 4;
    return this.prgRead(addr | 0x8000);
  }

  setIrq(bit, level) {
    if (level) this.irqLine |= (1 << bit);
    else this.irqLine &= ~(1 << bit);
  }

  // ── OAM DMA ($4014): 513/514 cycles, real reads through the bus ───────────
  oamDma(page) {
    const base = page << 8;
    this.tickUnits(); // alignment dummy cycle
    if (this.cycleCount & 1) this.tickUnits(); // extra on odd cycle
    for (let i = 0; i < 256; i += 1) {
      this.tickUnits();
      const v = this.busRead((base + i) & 0xffff);
      this.tickUnits();
      this.ppu.oam[this.ppu.oamAddr] = v;
      this.ppu.oamAddr = (this.ppu.oamAddr + 1) & 0xff;
    }
  }

  // ── Controllers ────────────────────────────────────────────────────────────
  readPad(port) {
    let bit;
    if (this.padStrobe & 1) {
      bit = this.pad[port] & 1;
    } else if (this.padIndex[port] < 8) {
      bit = (this.padLatch[port] >> this.padIndex[port]) & 1;
      this.padIndex[port] += 1;
    } else {
      bit = 1;
    }
    return 0x40 | bit; // open-bus upper bits as on a stock console
  }

  setButtons(port, mask) {
    this.pad[port] = mask & 0xff;
  }

  // ── PPU callbacks ──────────────────────────────────────────────────────────
  onVblankStart() {}
  onFrameComplete() { this.frameDone = true; }

  // ── Frame loop ─────────────────────────────────────────────────────────────
  runFrame() {
    this.frameDone = false;
    let guard = 200000;
    while (!this.frameDone && guard > 0) {
      this.cpu.step();
      guard -= 1;
    }
    return this.ppu.framebuffer;
  }

  // Debug peeks (no clock side effects)
  peek(addr) {
    addr &= 0xffff;
    if (addr < 0x2000) return this.ram[addr & 0x7ff];
    if (addr >= 0x8000) return this.prgRead(addr);
    if (addr >= 0x6000) return this.wram[addr - 0x6000];
    return 0;
  }

  // ── Save state ─────────────────────────────────────────────────────────────
  serialize() {
    return {
      version: 1,
      ram: Array.from(this.ram),
      wram: Array.from(this.wram),
      cpu: {
        a: this.cpu.a, x: this.cpu.x, y: this.cpu.y, sp: this.cpu.sp,
        pc: this.cpu.pc, p: this.cpu.p,
        nmiLine: this.cpu.nmiLine, nmiPending: this.cpu.nmiPending,
        jammed: this.cpu.jammed,
      },
      ppu: this.ppu.serialize(),
      apu: this.apu.serialize(),
      irqLine: this.irqLine,
      stall: this.stall,
      cycleCount: this.cycleCount,
      prgBank: this.prgBank,
      pad: [...this.pad], padLatch: [...this.padLatch],
      padIndex: [...this.padIndex], padStrobe: this.padStrobe,
    };
  }

  deserialize(s) {
    this.ram.set(s.ram);
    this.wram.set(s.wram);
    Object.assign(this.cpu, {
      a: s.cpu.a, x: s.cpu.x, y: s.cpu.y, sp: s.cpu.sp,
      pc: s.cpu.pc, p: s.cpu.p,
      nmiLine: s.cpu.nmiLine, nmiPending: s.cpu.nmiPending,
      jammed: s.cpu.jammed,
    });
    this.ppu.deserialize(s.ppu);
    this.apu.deserialize(s.apu);
    this.irqLine = s.irqLine;
    this.stall = s.stall;
    this.cycleCount = s.cycleCount;
    this.prgBank = s.prgBank;
    this.pad = [...s.pad]; this.padLatch = [...s.padLatch];
    this.padIndex = [...s.padIndex]; this.padStrobe = s.padStrobe;
  }
}

// Button bit positions for setButtons masks
export const BUTTON_A = 0x01;
export const BUTTON_B = 0x02;
export const BUTTON_SELECT = 0x04;
export const BUTTON_START = 0x08;
export const BUTTON_UP = 0x10;
export const BUTTON_DOWN = 0x20;
export const BUTTON_LEFT = 0x40;
export const BUTTON_RIGHT = 0x80;
