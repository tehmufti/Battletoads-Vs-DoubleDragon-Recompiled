/*
 * apu.js — cycle-driven NES APU (2A03).
 *
 * Ticked once per CPU cycle by the machine. Implements pulse 1/2 (envelope,
 * sweep with the pulse-1 negate quirk, duty sequencer), triangle (linear
 * counter), noise (15-bit LFSR), DMC (sample DMA with CPU stall, IRQ, and
 * direct $4011 PCM loads — Battletoads&DD plays drum/voice samples this way),
 * the frame counter (4/5-step, IRQ), and $4015 status.
 *
 * Audio is produced by accumulating the non-linear mixer output every CPU
 * cycle and emitting box-filtered samples at the requested rate into a ring
 * buffer. Pass sampleRate = 0 for headless use (no sample generation).
 */

const LENGTH_TABLE = [
  10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
  12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
];

const DUTY_TABLE = [
  0b01000000, 0b01100000, 0b01111000, 0b10011111,
];

const NOISE_PERIODS = [4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068];

const DMC_RATES = [428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54];

const TRIANGLE_SEQ = [
  15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
];

// Non-linear mixer lookup tables
const PULSE_TABLE = new Float32Array(31);
for (let i = 1; i < 31; i += 1) PULSE_TABLE[i] = 95.52 / (8128.0 / i + 100);
const TND_TABLE = new Float32Array(203);
for (let i = 1; i < 203; i += 1) TND_TABLE[i] = 163.67 / (24329.0 / i + 100);

class Envelope {
  constructor() { this.start = false; this.divider = 0; this.decay = 0; this.loop = false; this.constant = false; this.volume = 0; }
  clock() {
    if (this.start) {
      this.start = false;
      this.decay = 15;
      this.divider = this.volume;
    } else if (this.divider === 0) {
      this.divider = this.volume;
      if (this.decay > 0) this.decay -= 1;
      else if (this.loop) this.decay = 15;
    } else {
      this.divider -= 1;
    }
  }
  output() { return this.constant ? this.volume : this.decay; }
}

class Pulse {
  constructor(channel) {
    this.channel = channel; // 1 or 2 (sweep negate behavior differs)
    this.enabled = false;
    this.duty = 0; this.seqPos = 0;
    this.timer = 0; this.period = 0;
    this.length = 0; this.lengthHalt = false;
    this.env = new Envelope();
    this.sweepEnabled = false; this.sweepPeriod = 0; this.sweepNegate = false;
    this.sweepShift = 0; this.sweepDivider = 0; this.sweepReload = false;
  }
  sweepTarget() {
    const change = this.period >> this.sweepShift;
    if (this.sweepNegate) {
      return this.channel === 1 ? this.period - change - 1 : this.period - change;
    }
    return this.period + change;
  }
  muted() {
    return this.period < 8 || this.sweepTarget() > 0x7ff;
  }
  clockSweep() {
    if (this.sweepDivider === 0 && this.sweepEnabled && this.sweepShift !== 0 && !this.muted()) {
      const t = this.sweepTarget();
      if (t >= 0) this.period = t & 0x7ff;
    }
    if (this.sweepDivider === 0 || this.sweepReload) {
      this.sweepDivider = this.sweepPeriod;
      this.sweepReload = false;
    } else {
      this.sweepDivider -= 1;
    }
  }
  clockLength() { if (!this.lengthHalt && this.length > 0) this.length -= 1; }
  tick2() { // called every 2 CPU cycles (APU cycle)
    if (this.timer === 0) {
      this.timer = this.period;
      this.seqPos = (this.seqPos + 1) & 7;
    } else {
      this.timer -= 1;
    }
  }
  output() {
    if (!this.enabled || this.length === 0 || this.muted()) return 0;
    const bit = (DUTY_TABLE[this.duty] >> (7 - this.seqPos)) & 1;
    return bit ? this.env.output() : 0;
  }
}

class Triangle {
  constructor() {
    this.enabled = false;
    this.timer = 0; this.period = 0;
    this.length = 0; this.lengthHalt = false; // halt == control flag
    this.linear = 0; this.linearReload = 0; this.linearReloadFlag = false;
    this.seqPos = 0;
  }
  clockLinear() {
    if (this.linearReloadFlag) this.linear = this.linearReload;
    else if (this.linear > 0) this.linear -= 1;
    if (!this.lengthHalt) this.linearReloadFlag = false;
  }
  clockLength() { if (!this.lengthHalt && this.length > 0) this.length -= 1; }
  tick() { // every CPU cycle
    if (this.timer === 0) {
      this.timer = this.period;
      if (this.length > 0 && this.linear > 0 && this.period >= 2) {
        this.seqPos = (this.seqPos + 1) & 31;
      }
    } else {
      this.timer -= 1;
    }
  }
  output() {
    // The sequencer freezes when silenced but keeps outputting its last value.
    // period < 2 produces an ultrasonic tone — emit the average to avoid aliasing.
    return this.period < 2 ? 7 : TRIANGLE_SEQ[this.seqPos];
  }
}

class Noise {
  constructor() {
    this.enabled = false;
    this.timer = 0; this.period = NOISE_PERIODS[0];
    this.length = 0; this.lengthHalt = false;
    this.mode = false;
    this.shift = 1;
    this.env = new Envelope();
  }
  clockLength() { if (!this.lengthHalt && this.length > 0) this.length -= 1; }
  tick() {
    if (this.timer === 0) {
      this.timer = this.period - 1;
      const tap = this.mode ? 6 : 1;
      const fb = (this.shift ^ (this.shift >> tap)) & 1;
      this.shift = (this.shift >> 1) | (fb << 14);
    } else {
      this.timer -= 1;
    }
  }
  output() {
    if (!this.enabled || this.length === 0 || (this.shift & 1)) return 0;
    return this.env.output();
  }
}

class Dmc {
  constructor(apu) {
    this.apu = apu;
    this.enabled = false;
    this.irqEnabled = false; this.loop = false;
    this.rate = DMC_RATES[0]; this.timer = 0;
    this.level = 0;
    this.sampleAddr = 0xc000; this.sampleLength = 1;
    this.currentAddr = 0xc000; this.bytesRemaining = 0;
    this.shift = 0; this.bitsRemaining = 8;
    this.silence = true;
    this.buffer = -1; // -1 = empty
    this.irq = false;
  }
  restart() {
    this.currentAddr = this.sampleAddr;
    this.bytesRemaining = this.sampleLength;
  }
  maybeFetch() {
    if (this.buffer >= 0 || this.bytesRemaining === 0) return;
    // DMA fetch: CPU is stalled ~4 cycles
    this.buffer = this.apu.m.dmcFetch(this.currentAddr);
    this.currentAddr = this.currentAddr === 0xffff ? 0x8000 : this.currentAddr + 1;
    this.bytesRemaining -= 1;
    if (this.bytesRemaining === 0) {
      if (this.loop) {
        this.restart();
      } else if (this.irqEnabled) {
        this.irq = true;
        this.apu.updateIrq();
      }
    }
  }
  tick() {
    if (this.timer === 0) {
      this.timer = this.rate - 1;
      if (!this.silence) {
        if (this.shift & 1) {
          if (this.level <= 125) this.level += 2;
        } else if (this.level >= 2) {
          this.level -= 2;
        }
      }
      this.shift >>= 1;
      this.bitsRemaining -= 1;
      if (this.bitsRemaining === 0) {
        this.bitsRemaining = 8;
        if (this.buffer < 0) {
          this.silence = true;
        } else {
          this.silence = false;
          this.shift = this.buffer;
          this.buffer = -1;
          this.maybeFetch();
        }
      }
    } else {
      this.timer -= 1;
    }
  }
}

export class Apu {
  constructor(machine, sampleRate = 44100) {
    this.m = machine;
    this.pulse1 = new Pulse(1);
    this.pulse2 = new Pulse(2);
    this.triangle = new Triangle();
    this.noise = new Noise();
    this.dmc = new Dmc(this);

    this.frameCounterCycle = 0;
    this.frameMode5 = false;
    this.frameIrqInhibit = false;
    this.frameIrq = false;
    this.oddCycle = false;
    this.pendingFrameWrite = -1;
    this.pendingFrameDelay = 0;

    // sampling
    this.sampleRate = sampleRate;
    this.cyclesPerSample = sampleRate > 0 ? 1789773 / sampleRate : 0;
    this.sampleCounter = 0;
    this.sampleAcc = 0;
    this.sampleAccCount = 0;
    this.ring = new Float32Array(1 << 15);
    this.ringHead = 0; // write
    this.ringTail = 0; // read
    this.hpLast = 0; this.hpOut = 0;
  }

  updateIrq() {
    this.m.setIrq(0, this.frameIrq);
    this.m.setIrq(1, this.dmc.irq);
  }

  // ── Frame counter ──────────────────────────────────────────────────────────
  clockQuarter() {
    this.pulse1.env.clock();
    this.pulse2.env.clock();
    this.noise.env.clock();
    this.triangle.clockLinear();
  }

  clockHalf() {
    this.clockQuarter();
    this.pulse1.clockLength(); this.pulse1.clockSweep();
    this.pulse2.clockLength(); this.pulse2.clockSweep();
    this.triangle.clockLength();
    this.noise.clockLength();
  }

  tickFrameCounter() {
    if (this.pendingFrameDelay > 0) {
      this.pendingFrameDelay -= 1;
      if (this.pendingFrameDelay === 0) {
        const value = this.pendingFrameWrite;
        this.pendingFrameWrite = -1;
        this.frameMode5 = (value & 0x80) !== 0;
        this.frameIrqInhibit = (value & 0x40) !== 0;
        if (this.frameIrqInhibit) { this.frameIrq = false; this.updateIrq(); }
        this.frameCounterCycle = 0;
        if (this.frameMode5) this.clockHalf();
        return;
      }
    }
    const c = this.frameCounterCycle;
    if (!this.frameMode5) {
      if (c === 7457) this.clockQuarter();
      else if (c === 14913) this.clockHalf();
      else if (c === 22371) this.clockQuarter();
      else if (c === 29829) {
        this.clockHalf();
        if (!this.frameIrqInhibit) { this.frameIrq = true; this.updateIrq(); }
      }
      this.frameCounterCycle = c >= 29829 ? 0 : c + 1;
    } else {
      if (c === 7457) this.clockQuarter();
      else if (c === 14913) this.clockHalf();
      else if (c === 22371) this.clockQuarter();
      else if (c === 37281) this.clockHalf();
      this.frameCounterCycle = c >= 37281 ? 0 : c + 1;
    }
  }

  // ── Tick: one CPU cycle ───────────────────────────────────────────────────
  tick() {
    this.tickFrameCounter();
    this.triangle.tick();
    this.noise.tick();
    this.dmc.tick();
    this.oddCycle = !this.oddCycle;
    if (this.oddCycle) {
      this.pulse1.tick2();
      this.pulse2.tick2();
    }

    if (this.cyclesPerSample > 0) {
      const mix =
        PULSE_TABLE[this.pulse1.output() + this.pulse2.output()] +
        TND_TABLE[3 * this.triangle.output() + 2 * this.noise.output() + this.dmc.level];
      this.sampleAcc += mix;
      this.sampleAccCount += 1;
      this.sampleCounter += 1;
      if (this.sampleCounter >= this.cyclesPerSample) {
        this.sampleCounter -= this.cyclesPerSample;
        const raw = this.sampleAcc / this.sampleAccCount;
        this.sampleAcc = 0; this.sampleAccCount = 0;
        // one-pole high-pass (~37 Hz at 44.1k) to remove DC
        const out = raw - this.hpLast + 0.9947 * this.hpOut;
        this.hpLast = raw; this.hpOut = out;
        const head = this.ringHead;
        const next = (head + 1) & (this.ring.length - 1);
        if (next !== this.ringTail) {
          this.ring[head] = out;
          this.ringHead = next;
        } // else: overflow, drop sample
      }
    }
  }

  availableSamples() {
    return (this.ringHead - this.ringTail) & (this.ring.length - 1);
  }

  readSamples(out, count) {
    let tail = this.ringTail;
    const mask = this.ring.length - 1;
    const avail = (this.ringHead - tail) & mask;
    const n = Math.min(count, avail);
    for (let i = 0; i < n; i += 1) {
      out[i] = this.ring[tail];
      tail = (tail + 1) & mask;
    }
    this.ringTail = tail;
    return n;
  }

  // ── Registers ──────────────────────────────────────────────────────────────
  write(addr, value) {
    switch (addr) {
      case 0x4000: case 0x4004: {
        const p = addr === 0x4000 ? this.pulse1 : this.pulse2;
        p.duty = (value >> 6) & 3;
        p.lengthHalt = (value & 0x20) !== 0;
        p.env.loop = p.lengthHalt;
        p.env.constant = (value & 0x10) !== 0;
        p.env.volume = value & 0x0f;
        break;
      }
      case 0x4001: case 0x4005: {
        const p = addr === 0x4001 ? this.pulse1 : this.pulse2;
        p.sweepEnabled = (value & 0x80) !== 0;
        p.sweepPeriod = (value >> 4) & 7;
        p.sweepNegate = (value & 0x08) !== 0;
        p.sweepShift = value & 7;
        p.sweepReload = true;
        break;
      }
      case 0x4002: case 0x4006: {
        const p = addr === 0x4002 ? this.pulse1 : this.pulse2;
        p.period = (p.period & 0x700) | value;
        break;
      }
      case 0x4003: case 0x4007: {
        const p = addr === 0x4003 ? this.pulse1 : this.pulse2;
        p.period = (p.period & 0x0ff) | ((value & 7) << 8);
        if (p.enabled) p.length = LENGTH_TABLE[value >> 3];
        p.seqPos = 0;
        p.env.start = true;
        break;
      }
      case 0x4008:
        this.triangle.lengthHalt = (value & 0x80) !== 0;
        this.triangle.linearReload = value & 0x7f;
        break;
      case 0x400a:
        this.triangle.period = (this.triangle.period & 0x700) | value;
        break;
      case 0x400b:
        this.triangle.period = (this.triangle.period & 0x0ff) | ((value & 7) << 8);
        if (this.triangle.enabled) this.triangle.length = LENGTH_TABLE[value >> 3];
        this.triangle.linearReloadFlag = true;
        break;
      case 0x400c:
        this.noise.lengthHalt = (value & 0x20) !== 0;
        this.noise.env.loop = this.noise.lengthHalt;
        this.noise.env.constant = (value & 0x10) !== 0;
        this.noise.env.volume = value & 0x0f;
        break;
      case 0x400e:
        this.noise.mode = (value & 0x80) !== 0;
        this.noise.period = NOISE_PERIODS[value & 0x0f];
        break;
      case 0x400f:
        if (this.noise.enabled) this.noise.length = LENGTH_TABLE[value >> 3];
        this.noise.env.start = true;
        break;
      case 0x4010:
        this.dmc.irqEnabled = (value & 0x80) !== 0;
        this.dmc.loop = (value & 0x40) !== 0;
        this.dmc.rate = DMC_RATES[value & 0x0f];
        if (!this.dmc.irqEnabled) { this.dmc.irq = false; this.updateIrq(); }
        break;
      case 0x4011:
        this.dmc.level = value & 0x7f;
        break;
      case 0x4012:
        this.dmc.sampleAddr = 0xc000 | (value << 6);
        break;
      case 0x4013:
        this.dmc.sampleLength = (value << 4) | 1;
        break;
      case 0x4015: {
        this.pulse1.enabled = (value & 0x01) !== 0;
        this.pulse2.enabled = (value & 0x02) !== 0;
        this.triangle.enabled = (value & 0x04) !== 0;
        this.noise.enabled = (value & 0x08) !== 0;
        if (!this.pulse1.enabled) this.pulse1.length = 0;
        if (!this.pulse2.enabled) this.pulse2.length = 0;
        if (!this.triangle.enabled) this.triangle.length = 0;
        if (!this.noise.enabled) this.noise.length = 0;
        const dmcEnable = (value & 0x10) !== 0;
        this.dmc.enabled = dmcEnable;
        if (dmcEnable) {
          if (this.dmc.bytesRemaining === 0) {
            this.dmc.restart();
          }
          this.dmc.maybeFetch();
        } else {
          this.dmc.bytesRemaining = 0;
        }
        this.dmc.irq = false;
        this.updateIrq();
        break;
      }
      case 0x4017:
        // takes effect after a 3-4 cycle delay
        this.pendingFrameWrite = value;
        this.pendingFrameDelay = this.oddCycle ? 4 : 3;
        break;
      default:
        break;
    }
  }

  read4015() {
    let v = 0;
    if (this.pulse1.length > 0) v |= 0x01;
    if (this.pulse2.length > 0) v |= 0x02;
    if (this.triangle.length > 0) v |= 0x04;
    if (this.noise.length > 0) v |= 0x08;
    if (this.dmc.bytesRemaining > 0) v |= 0x10;
    if (this.frameIrq) v |= 0x40;
    if (this.dmc.irq) v |= 0x80;
    this.frameIrq = false;
    this.updateIrq();
    return v;
  }

  // ── Save state ─────────────────────────────────────────────────────────────
  serialize() {
    const env = (e) => ({ start: e.start, divider: e.divider, decay: e.decay, loop: e.loop, constant: e.constant, volume: e.volume });
    const pulse = (p) => ({
      enabled: p.enabled, duty: p.duty, seqPos: p.seqPos, timer: p.timer, period: p.period,
      length: p.length, lengthHalt: p.lengthHalt, env: env(p.env),
      sweepEnabled: p.sweepEnabled, sweepPeriod: p.sweepPeriod, sweepNegate: p.sweepNegate,
      sweepShift: p.sweepShift, sweepDivider: p.sweepDivider, sweepReload: p.sweepReload,
    });
    return {
      pulse1: pulse(this.pulse1), pulse2: pulse(this.pulse2),
      triangle: {
        enabled: this.triangle.enabled, timer: this.triangle.timer, period: this.triangle.period,
        length: this.triangle.length, lengthHalt: this.triangle.lengthHalt,
        linear: this.triangle.linear, linearReload: this.triangle.linearReload,
        linearReloadFlag: this.triangle.linearReloadFlag, seqPos: this.triangle.seqPos,
      },
      noise: {
        enabled: this.noise.enabled, timer: this.noise.timer, period: this.noise.period,
        length: this.noise.length, lengthHalt: this.noise.lengthHalt, mode: this.noise.mode,
        shift: this.noise.shift, env: env(this.noise.env),
      },
      dmc: {
        enabled: this.dmc.enabled, irqEnabled: this.dmc.irqEnabled, loop: this.dmc.loop,
        rate: this.dmc.rate, timer: this.dmc.timer, level: this.dmc.level,
        sampleAddr: this.dmc.sampleAddr, sampleLength: this.dmc.sampleLength,
        currentAddr: this.dmc.currentAddr, bytesRemaining: this.dmc.bytesRemaining,
        shift: this.dmc.shift, bitsRemaining: this.dmc.bitsRemaining,
        silence: this.dmc.silence, buffer: this.dmc.buffer, irq: this.dmc.irq,
      },
      frameCounterCycle: this.frameCounterCycle, frameMode5: this.frameMode5,
      frameIrqInhibit: this.frameIrqInhibit, frameIrq: this.frameIrq,
      oddCycle: this.oddCycle,
      pendingFrameWrite: this.pendingFrameWrite, pendingFrameDelay: this.pendingFrameDelay,
    };
  }

  deserialize(s) {
    const env = (e, t) => { e.start = t.start; e.divider = t.divider; e.decay = t.decay; e.loop = t.loop; e.constant = t.constant; e.volume = t.volume; };
    const pulse = (p, t) => {
      p.enabled = t.enabled; p.duty = t.duty; p.seqPos = t.seqPos; p.timer = t.timer; p.period = t.period;
      p.length = t.length; p.lengthHalt = t.lengthHalt; env(p.env, t.env);
      p.sweepEnabled = t.sweepEnabled; p.sweepPeriod = t.sweepPeriod; p.sweepNegate = t.sweepNegate;
      p.sweepShift = t.sweepShift; p.sweepDivider = t.sweepDivider; p.sweepReload = t.sweepReload;
    };
    pulse(this.pulse1, s.pulse1); pulse(this.pulse2, s.pulse2);
    Object.assign(this.triangle, s.triangle);
    const n = s.noise; this.noise.enabled = n.enabled; this.noise.timer = n.timer; this.noise.period = n.period;
    this.noise.length = n.length; this.noise.lengthHalt = n.lengthHalt; this.noise.mode = n.mode;
    this.noise.shift = n.shift; env(this.noise.env, n.env);
    Object.assign(this.dmc, { apu: this }, s.dmc);
    this.dmc.apu = this;
    this.frameCounterCycle = s.frameCounterCycle; this.frameMode5 = s.frameMode5;
    this.frameIrqInhibit = s.frameIrqInhibit; this.frameIrq = s.frameIrq;
    this.oddCycle = s.oddCycle;
    this.pendingFrameWrite = s.pendingFrameWrite; this.pendingFrameDelay = s.pendingFrameDelay;
    this.updateIrq();
  }
}
