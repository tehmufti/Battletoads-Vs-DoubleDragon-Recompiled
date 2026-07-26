/*
 * ppu.js — dot-accurate NES PPU (2C02, NTSC).
 *
 * Runs 3 dots per CPU cycle, driven by the machine. Implements the real
 * rendering pipeline: loopy v/t/x/w registers, background fetch/shifters,
 * per-scanline sprite evaluation with the 8-sprite limit, true per-pixel
 * sprite-0 hit, 8x16 sprites, vblank/NMI timing with the $2002 race,
 * odd-frame dot skip, $2007 read buffer, forced-blank backdrop hack.
 *
 * Because timing is real, mid-frame $2005/$2006/$2001 writes (Battletoads'
 * forced-blank CHR streaming, status-bar splits, vertical elevator scroll)
 * render correctly with no game-specific heuristics.
 */

// 2C02 base palette (Nestopia NTSC-ish), 64 RGB triplets.
const BASE_PALETTE = new Uint8Array([
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
]);

// Emphasis attenuation per channel for the 8 emphasis combos (R,G,B bits).
// Simple multiplicative model — close enough visually.
function buildRgbLut() {
  const lut = new Uint32Array(512);
  for (let emph = 0; emph < 8; emph += 1) {
    const eR = emph & 1, eG = emph & 2, eB = emph & 4;
    for (let c = 0; c < 64; c += 1) {
      let r = BASE_PALETTE[c * 3];
      let g = BASE_PALETTE[c * 3 + 1];
      let b = BASE_PALETTE[c * 3 + 2];
      if (emph !== 0) {
        // attenuate channels whose emphasis bit is NOT set
        const att = 0.746;
        if (!eR) r *= att;
        if (!eG) g *= att;
        if (!eB) b *= att;
      }
      r = Math.min(255, Math.round(r));
      g = Math.min(255, Math.round(g));
      b = Math.min(255, Math.round(b));
      // little-endian RGBA (alpha = 0xff)
      lut[(emph << 6) | c] = (0xff << 24) | (b << 16) | (g << 8) | r;
    }
  }
  return lut;
}
const RGB_LUT = buildRgbLut();

// Mirroring modes
export const MIRROR_HORIZONTAL = 0;
export const MIRROR_VERTICAL = 1;
export const MIRROR_SINGLE0 = 2;
export const MIRROR_SINGLE1 = 3;

export class Ppu {
  constructor(machine) {
    this.m = machine;
    this.chr = new Uint8Array(0x2000);   // CHR-RAM (mapper may swap to ROM)
    this.chrWritable = true;
    this.vram = new Uint8Array(0x800);
    this.paletteRam = new Uint8Array(0x20);
    this.oam = new Uint8Array(0x100);
    this.mirror = MIRROR_SINGLE0;

    this.framebuffer = new Uint32Array(256 * 240);

    // registers
    this.ctrl = 0; this.mask = 0; this.status = 0;
    this.oamAddr = 0;
    this.v = 0; this.t = 0; this.x = 0; this.w = 0;
    this.readBuffer = 0;
    this.openBus = 0;

    // timing
    this.scanline = 261;
    this.dot = 0;
    this.frame = 0;
    this.oddFrame = false;
    this.nmiSuppress = false;

    // background pipeline
    this.ntByte = 0; this.atByte = 0; this.bgLoByte = 0; this.bgHiByte = 0;
    this.bgShiftLo = 0; this.bgShiftHi = 0;
    this.atShiftLo = 0; this.atShiftHi = 0;
    this.atLatchLo = 0; this.atLatchHi = 0;

    // sprite line buffers (computed at dot 257 for the next scanline)
    this.spColor = new Uint8Array(256);   // 0 = transparent, else palette-line color 1..3
    this.spPal = new Uint8Array(256);     // sprite palette 0..3
    this.spFlags = new Uint8Array(256);   // bit0 = behind bg, bit1 = is sprite 0
    this.spCount = 0;
  }

  reset() {
    this.ctrl = 0; this.mask = 0; this.status = 0;
    this.oamAddr = 0;
    this.v = 0; this.t = 0; this.x = 0; this.w = 0;
    this.readBuffer = 0;
    this.scanline = 261; this.dot = 0;
    this.frame = 0; this.oddFrame = false;
    this.nmiSuppress = false;
    this.bgShiftLo = this.bgShiftHi = this.atShiftLo = this.atShiftHi = 0;
    this.spColor.fill(0);
  }

  renderingEnabled() { return (this.mask & 0x18) !== 0; }

  updateNmiLine() {
    this.m.cpu.setNmiLine((this.ctrl & 0x80) !== 0 && (this.status & 0x80) !== 0);
  }

  // ── Memory map ─────────────────────────────────────────────────────────────
  ntIndex(addr) {
    switch (this.mirror) {
      case MIRROR_HORIZONTAL: return ((addr >> 1) & 0x400) | (addr & 0x3ff);
      case MIRROR_VERTICAL: return addr & 0x7ff;
      case MIRROR_SINGLE0: return addr & 0x3ff;
      default: return 0x400 | (addr & 0x3ff);
    }
  }

  ppuRead(addr) {
    addr &= 0x3fff;
    if (addr < 0x2000) return this.chr[addr];
    if (addr < 0x3f00) return this.vram[this.ntIndex(addr)];
    let p = addr & 0x1f;
    if (p >= 0x10 && (p & 0x03) === 0) p -= 0x10;
    return this.paletteRam[p];
  }

  ppuWrite(addr, value) {
    addr &= 0x3fff;
    if (addr < 0x2000) {
      if (this.chrWritable) this.chr[addr] = value;
      return;
    }
    if (addr < 0x3f00) {
      this.vram[this.ntIndex(addr)] = value;
      return;
    }
    let p = addr & 0x1f;
    if (p >= 0x10 && (p & 0x03) === 0) p -= 0x10;
    this.paletteRam[p] = value;
  }

  // ── CPU-visible registers ──────────────────────────────────────────────────
  readReg(reg) {
    switch (reg) {
      case 2: {
        let value = (this.status & 0xe0) | (this.openBus & 0x1f);
        // $2002 read race: reading right as vblank begins returns the flag
        // clear and suppresses the NMI for this frame.
        if (this.scanline === 241 && this.dot <= 2) {
          value &= 0x7f;
          this.nmiSuppress = true;
          this.status &= 0x7f;
          this.updateNmiLine();
        }
        this.status &= 0x7f; // clear vblank
        this.updateNmiLine();
        this.w = 0;
        this.openBus = value;
        return value;
      }
      case 4: {
        const value = this.oam[this.oamAddr];
        this.openBus = value;
        return value;
      }
      case 7: {
        const addr = this.v & 0x3fff;
        let value;
        if (addr >= 0x3f00) {
          value = this.ppuRead(addr) & ((this.mask & 0x01) ? 0x30 : 0x3f);
          this.readBuffer = this.vram[this.ntIndex(addr)]; // buffer loads NT under palette
        } else {
          value = this.readBuffer;
          this.readBuffer = this.ppuRead(addr);
        }
        this.v = (this.v + ((this.ctrl & 0x04) ? 32 : 1)) & 0x7fff;
        this.openBus = value;
        return value;
      }
      default:
        return this.openBus;
    }
  }

  writeReg(reg, value) {
    this.openBus = value;
    switch (reg) {
      case 0: {
        this.ctrl = value;
        this.t = (this.t & 0x73ff) | ((value & 0x03) << 10);
        this.updateNmiLine();
        return;
      }
      case 1: this.mask = value; return;
      case 2: return;
      case 3: this.oamAddr = value; return;
      case 4:
        this.oam[this.oamAddr] = value;
        this.oamAddr = (this.oamAddr + 1) & 0xff;
        return;
      case 5:
        if (this.w === 0) {
          this.t = (this.t & 0x7fe0) | (value >> 3);
          this.x = value & 0x07;
          this.w = 1;
        } else {
          this.t = (this.t & 0x0c1f) | ((value & 0x07) << 12) | ((value & 0xf8) << 2);
          this.w = 0;
        }
        return;
      case 6:
        if (this.w === 0) {
          this.t = (this.t & 0x00ff) | ((value & 0x3f) << 8);
          this.w = 1;
        } else {
          this.t = (this.t & 0x7f00) | value;
          this.v = this.t;
          this.w = 0;
        }
        return;
      case 7: {
        this.ppuWrite(this.v & 0x3fff, value);
        this.v = (this.v + ((this.ctrl & 0x04) ? 32 : 1)) & 0x7fff;
        return;
      }
    }
  }

  // ── Internal helpers ───────────────────────────────────────────────────────
  incHoriz() {
    if ((this.v & 0x001f) === 31) {
      this.v = (this.v & ~0x001f) ^ 0x0400;
    } else {
      this.v += 1;
    }
  }

  incVert() {
    if ((this.v & 0x7000) !== 0x7000) {
      this.v += 0x1000;
    } else {
      this.v &= ~0x7000;
      let y = (this.v & 0x03e0) >> 5;
      if (y === 29) { y = 0; this.v ^= 0x0800; }
      else if (y === 31) { y = 0; }
      else { y += 1; }
      this.v = (this.v & ~0x03e0) | (y << 5);
    }
  }

  copyHoriz() { this.v = (this.v & ~0x041f) | (this.t & 0x041f); }
  copyVert() { this.v = (this.v & ~0x7be0) | (this.t & 0x7be0); }

  reloadShifters() {
    this.bgShiftLo = (this.bgShiftLo & 0xff00) | this.bgLoByte;
    this.bgShiftHi = (this.bgShiftHi & 0xff00) | this.bgHiByte;
    this.atLatchLo = this.atByte & 1;
    this.atLatchHi = (this.atByte >> 1) & 1;
  }

  shiftBg() {
    this.bgShiftLo = (this.bgShiftLo << 1) & 0xffff;
    this.bgShiftHi = (this.bgShiftHi << 1) & 0xffff;
    this.atShiftLo = ((this.atShiftLo << 1) | this.atLatchLo) & 0xff;
    this.atShiftHi = ((this.atShiftHi << 1) | this.atLatchHi) & 0xff;
  }

  fetchStep(phase) {
    switch (phase) {
      case 0:
        this.ntByte = this.ppuRead(0x2000 | (this.v & 0x0fff));
        break;
      case 2: {
        const at = this.ppuRead(
          0x23c0 | (this.v & 0x0c00) | ((this.v >> 4) & 0x38) | ((this.v >> 2) & 0x07)
        );
        const shift = ((this.v >> 4) & 4) | (this.v & 2);
        this.atByte = (at >> shift) & 3;
        break;
      }
      case 4: {
        const base = ((this.ctrl & 0x10) << 8) + this.ntByte * 16 + (this.v >> 12);
        this.bgLoByte = this.chr[base];
        break;
      }
      case 6: {
        const base = ((this.ctrl & 0x10) << 8) + this.ntByte * 16 + (this.v >> 12) + 8;
        this.bgHiByte = this.chr[base];
        break;
      }
      case 7:
        this.incHoriz();
        break;
    }
  }

  // Sprite evaluation + pattern fetch for the NEXT scanline (called at dot 257).
  evalSprites(line) {
    this.spColor.fill(0);
    this.spCount = 0;
    const next = line; // comparison line per hardware: eval on line n covers line n+1 rows via (n - y)
    const h = (this.ctrl & 0x20) ? 16 : 8;
    let found = 0;
    for (let i = 0; i < 64; i += 1) {
      const y = this.oam[i * 4];
      const row = next - y;
      if (row < 0 || row >= h) continue;
      if (found === 8) {
        this.status |= 0x20; // sprite overflow (simplified)
        break;
      }
      found += 1;
      const tile = this.oam[i * 4 + 1];
      const attr = this.oam[i * 4 + 2];
      const sx = this.oam[i * 4 + 3];
      const flipH = (attr & 0x40) !== 0;
      const flipV = (attr & 0x80) !== 0;
      let r = flipV ? (h - 1 - row) : row;
      let base;
      if (h === 16) {
        const table = (tile & 1) << 12;
        let t = tile & 0xfe;
        if (r >= 8) { t += 1; r -= 8; }
        base = table + t * 16 + r;
      } else {
        base = ((this.ctrl & 0x08) << 9) + tile * 16 + r;
      }
      const lo = this.chr[base];
      const hi = this.chr[base + 8];
      const pal = attr & 3;
      const flags = ((attr & 0x20) ? 1 : 0) | (i === 0 ? 2 : 0);
      for (let px = 0; px < 8; px += 1) {
        const xPos = sx + px;
        if (xPos > 255) break;
        if (this.spColor[xPos] !== 0) continue; // first (lowest-index) sprite wins
        const bit = flipH ? px : (7 - px);
        const c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        if (c === 0) continue;
        this.spColor[xPos] = c;
        this.spPal[xPos] = pal;
        this.spFlags[xPos] = flags;
      }
    }
    this.spCount = found;
  }

  backdropColor() {
    // Forced-blank palette hack: when rendering is disabled and v points into
    // palette space, the PPU shows that palette entry instead of $3F00.
    let idx;
    if (!this.renderingEnabled() && (this.v & 0x3f00) === 0x3f00) {
      let p = this.v & 0x1f;
      if (p >= 0x10 && (p & 0x03) === 0) p -= 0x10;
      idx = this.paletteRam[p];
    } else {
      idx = this.paletteRam[0];
    }
    idx &= (this.mask & 0x01) ? 0x30 : 0x3f;
    return RGB_LUT[((this.mask & 0xe0) << 1) | idx];
  }

  // ── The dot ────────────────────────────────────────────────────────────────
  tick() {
    const sl = this.scanline;
    const d = this.dot;
    const rendering = (this.mask & 0x18) !== 0;

    if (sl < 240) {
      if (rendering) {
        this.tickRenderLine(sl, d, true);
      } else if (d >= 1 && d <= 256) {
        // rendering disabled: backdrop fill
        this.framebuffer[sl * 256 + (d - 1)] = this.backdropColor();
      }
    } else if (sl === 241) {
      if (d === 1) {
        if (!this.nmiSuppress) {
          this.status |= 0x80;
          this.updateNmiLine();
        }
        this.nmiSuppress = false;
        this.m.onVblankStart();
      }
    } else if (sl === 261) {
      if (d === 1) {
        this.status &= ~(0x80 | 0x40 | 0x20);
        this.updateNmiLine();
      }
      if (rendering) {
        this.tickRenderLine(sl, d, false);
        if (d >= 280 && d <= 304) this.copyVert();
      }
    }

    // advance dot/scanline
    let dot = d + 1;
    if (sl === 261 && d === 339 && this.oddFrame && rendering) {
      dot = 341; // skip the last dot of pre-render on odd frames
    }
    if (dot >= 341) {
      this.dot = 0;
      let next = sl + 1;
      if (next >= 262) {
        next = 0;
        this.frame += 1;
        this.oddFrame = !this.oddFrame;
        this.m.onFrameComplete();
      }
      this.scanline = next;
    } else {
      this.dot = dot;
    }
  }

  tickRenderLine(sl, d, visible) {
    // Background pipeline — exact hardware schedule:
    //   fetch phases at (d-1)&7: 0=NT 2=AT 4=BGlo 6=BGhi 7=incHoriz
    //   shifters shift on dots 2-257 and 322-337, reload on 9,17,…,257 and 329,337
    //   incVert at 256, copyHoriz at 257
    if (d >= 1 && d <= 256) {
      if (d >= 2) this.shiftBg();
      const ph = (d - 1) & 7;
      if (ph === 0 && d >= 9) this.reloadShifters();
      this.fetchStep(ph);
      if (d === 256) this.incVert();
      if (visible) this.renderPixel(sl, d - 1);
    } else if (d === 257) {
      this.shiftBg();
      this.reloadShifters();
      this.copyHoriz();
      this.oamAddr = 0;
      if (visible && sl < 239) {
        this.evalSprites(sl);
      } else if (!visible) {
        // pre-render: no sprites for line 0
        this.spColor.fill(0);
      }
    } else if (d >= 258 && d <= 320) {
      this.oamAddr = 0;
    } else if (d >= 321 && d <= 336) {
      this.shiftBg();
      const ph = (d - 1) & 7;
      if (ph === 0 && d === 329) this.reloadShifters();
      this.fetchStep(ph);
    } else if (d === 337) {
      this.shiftBg();
      this.reloadShifters();
      this.ntByte = this.ppuRead(0x2000 | (this.v & 0x0fff));
    } else if (d === 339) {
      this.ntByte = this.ppuRead(0x2000 | (this.v & 0x0fff));
    }
  }

  renderPixel(sl, x) {
    const mask = this.mask;
    let bgPix = 0;
    let bgPal = 0;
    if ((mask & 0x08) && (x >= 8 || (mask & 0x02))) {
      const shift = 15 - this.x;
      bgPix = ((this.bgShiftLo >> shift) & 1) | (((this.bgShiftHi >> shift) & 1) << 1);
      if (bgPix !== 0) {
        const aShift = 7 - this.x;
        bgPal = ((this.atShiftLo >> aShift) & 1) | (((this.atShiftHi >> aShift) & 1) << 1);
      }
    }

    let spPix = 0;
    let spPal = 0;
    let spFlags = 0;
    if ((mask & 0x10) && (x >= 8 || (mask & 0x04)) && sl !== 0) {
      spPix = this.spColor[x];
      spPal = this.spPal[x];
      spFlags = this.spFlags[x];
    }

    let palIndex;
    if (spPix !== 0 && (bgPix === 0 || (spFlags & 1) === 0)) {
      palIndex = 0x10 + spPal * 4 + spPix;
    } else if (bgPix !== 0) {
      palIndex = bgPal * 4 + bgPix;
    } else {
      palIndex = 0;
    }

    // sprite-0 hit: opaque sprite-0 pixel over opaque bg pixel, x<255
    if (spPix !== 0 && bgPix !== 0 && (spFlags & 2) !== 0 && x !== 255) {
      this.status |= 0x40;
    }

    let color = this.paletteRam[palIndex === 0 ? 0 : palIndex];
    color &= (mask & 0x01) ? 0x30 : 0x3f;
    this.framebuffer[sl * 256 + x] = RGB_LUT[((mask & 0xe0) << 1) | color];
  }

  // ── Save state ─────────────────────────────────────────────────────────────
  serialize() {
    return {
      chr: Array.from(this.chr),
      vram: Array.from(this.vram),
      paletteRam: Array.from(this.paletteRam),
      oam: Array.from(this.oam),
      mirror: this.mirror,
      ctrl: this.ctrl, mask: this.mask, status: this.status,
      oamAddr: this.oamAddr,
      v: this.v, t: this.t, x: this.x, w: this.w,
      readBuffer: this.readBuffer,
      scanline: this.scanline, dot: this.dot,
      frame: this.frame, oddFrame: this.oddFrame,
    };
  }

  deserialize(s) {
    this.chr.set(s.chr);
    this.vram.set(s.vram);
    this.paletteRam.set(s.paletteRam);
    this.oam.set(s.oam);
    this.mirror = s.mirror;
    this.ctrl = s.ctrl; this.mask = s.mask; this.status = s.status;
    this.oamAddr = s.oamAddr;
    this.v = s.v; this.t = s.t; this.x = s.x; this.w = s.w;
    this.readBuffer = s.readBuffer;
    this.scanline = s.scanline; this.dot = s.dot;
    this.frame = s.frame; this.oddFrame = s.oddFrame;
    this.spColor.fill(0);
  }
}
