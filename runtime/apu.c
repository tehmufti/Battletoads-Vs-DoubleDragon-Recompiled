/*
 * apu.c — cycle-driven NES APU (2A03), ported 1:1 from src/nes/apu.js.
 * Pulse 1/2 (envelope, sweep with pulse-1 negate quirk), triangle, noise
 * LFSR, DMC with CPU-stall sample DMA and $4011 PCM, frame counter with
 * write delay, $4015 status. Box-filtered float samples into a ring buffer;
 * sample_rate = 0 for headless (no sample generation).
 */
#include "nes.h"

Apu APU;

static const uint8_t LENGTH_TABLE[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t DUTY_TABLE[4] = { 0x40, 0x60, 0x78, 0x9f };
static const uint16_t NOISE_PERIODS[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t DMC_RATES[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
static const uint8_t TRIANGLE_SEQ[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static float PULSE_TABLE[31];
static float TND_TABLE[203];

/* ── Envelope ────────────────────────────────────────────────────────────── */
static void env_clock(Envelope *e) {
    if (e->start) {
        e->start = false;
        e->decay = 15;
        e->divider = e->volume;
    } else if (e->divider == 0) {
        e->divider = e->volume;
        if (e->decay > 0) e->decay -= 1;
        else if (e->loop) e->decay = 15;
    } else {
        e->divider -= 1;
    }
}
static inline uint8_t env_output(const Envelope *e) { return e->constant ? e->volume : e->decay; }

/* ── Pulse ───────────────────────────────────────────────────────────────── */
static int pulse_sweep_target(const Pulse *p) {
    int change = p->period >> p->sweep_shift;
    if (p->sweep_negate) {
        return p->channel == 1 ? p->period - change - 1 : p->period - change;
    }
    return p->period + change;
}
static bool pulse_muted(const Pulse *p) {
    return p->period < 8 || pulse_sweep_target(p) > 0x7ff;
}
static void pulse_clock_sweep(Pulse *p) {
    if (p->sweep_divider == 0 && p->sweep_enabled && p->sweep_shift != 0 && !pulse_muted(p)) {
        int t = pulse_sweep_target(p);
        if (t >= 0) p->period = (uint16_t)(t & 0x7ff);
    }
    if (p->sweep_divider == 0 || p->sweep_reload) {
        p->sweep_divider = p->sweep_period;
        p->sweep_reload = false;
    } else {
        p->sweep_divider -= 1;
    }
}
static void pulse_clock_length(Pulse *p) { if (!p->length_halt && p->length > 0) p->length -= 1; }
static void pulse_tick2(Pulse *p) {
    if (p->timer == 0) {
        p->timer = p->period;
        p->seq_pos = (p->seq_pos + 1) & 7;
    } else {
        p->timer -= 1;
    }
}
static uint8_t pulse_output(const Pulse *p) {
    if (!p->enabled || p->length == 0 || pulse_muted(p)) return 0;
    int bit = (DUTY_TABLE[p->duty] >> (7 - p->seq_pos)) & 1;
    return bit ? env_output(&p->env) : 0;
}

/* ── Triangle ────────────────────────────────────────────────────────────── */
static void tri_clock_linear(Triangle *t) {
    if (t->linear_reload_flag) t->linear = t->linear_reload;
    else if (t->linear > 0) t->linear -= 1;
    if (!t->length_halt) t->linear_reload_flag = false;
}
static void tri_clock_length(Triangle *t) { if (!t->length_halt && t->length > 0) t->length -= 1; }
static void tri_tick(Triangle *t) {
    if (t->timer == 0) {
        t->timer = t->period;
        if (t->length > 0 && t->linear > 0 && t->period >= 2) {
            t->seq_pos = (t->seq_pos + 1) & 31;
        }
    } else {
        t->timer -= 1;
    }
}
static uint8_t tri_output(const Triangle *t) {
    return t->period < 2 ? 7 : TRIANGLE_SEQ[t->seq_pos];
}

/* ── Noise ───────────────────────────────────────────────────────────────── */
static void noise_clock_length(Noise *n) { if (!n->length_halt && n->length > 0) n->length -= 1; }
static void noise_tick(Noise *n) {
    if (n->timer == 0) {
        n->timer = n->period - 1;
        int tap = n->mode ? 6 : 1;
        uint16_t fb = (uint16_t)((n->shift ^ (n->shift >> tap)) & 1);
        n->shift = (uint16_t)((n->shift >> 1) | (fb << 14));
    } else {
        n->timer -= 1;
    }
}
static uint8_t noise_output(const Noise *n) {
    if (!n->enabled || n->length == 0 || (n->shift & 1)) return 0;
    return env_output(&n->env);
}

/* ── DMC ─────────────────────────────────────────────────────────────────── */
static void apu_update_irq(void) {
    set_irq(0, APU.frame_irq);
    set_irq(1, APU.dmc.irq);
}
static void dmc_restart(Dmc *d) {
    d->current_addr = d->sample_addr;
    d->bytes_remaining = d->sample_length;
}
static void dmc_maybe_fetch(Dmc *d) {
    if (d->buffer >= 0 || d->bytes_remaining == 0) return;
    d->buffer = dmc_fetch(d->current_addr);
    d->current_addr = d->current_addr == 0xffff ? 0x8000 : d->current_addr + 1;
    d->bytes_remaining -= 1;
    if (d->bytes_remaining == 0) {
        if (d->loop) {
            dmc_restart(d);
        } else if (d->irq_enabled) {
            d->irq = true;
            apu_update_irq();
        }
    }
}
static void dmc_tick(Dmc *d) {
    if (d->timer == 0) {
        d->timer = d->rate - 1;
        if (!d->silence) {
            if (d->shift & 1) {
                if (d->level <= 125) d->level += 2;
            } else if (d->level >= 2) {
                d->level -= 2;
            }
        }
        d->shift >>= 1;
        d->bits_remaining -= 1;
        if (d->bits_remaining == 0) {
            d->bits_remaining = 8;
            if (d->buffer < 0) {
                d->silence = true;
            } else {
                d->silence = false;
                d->shift = (uint8_t)d->buffer;
                d->buffer = -1;
                dmc_maybe_fetch(d);
            }
        }
    } else {
        d->timer -= 1;
    }
}

/* ── Frame counter ───────────────────────────────────────────────────────── */
static void clock_quarter(void) {
    env_clock(&APU.pulse1.env);
    env_clock(&APU.pulse2.env);
    env_clock(&APU.noise.env);
    tri_clock_linear(&APU.triangle);
}
static void clock_half(void) {
    clock_quarter();
    pulse_clock_length(&APU.pulse1); pulse_clock_sweep(&APU.pulse1);
    pulse_clock_length(&APU.pulse2); pulse_clock_sweep(&APU.pulse2);
    tri_clock_length(&APU.triangle);
    noise_clock_length(&APU.noise);
}
static void tick_frame_counter(void) {
    if (APU.pending_frame_delay > 0) {
        APU.pending_frame_delay -= 1;
        if (APU.pending_frame_delay == 0) {
            int value = APU.pending_frame_write;
            APU.pending_frame_write = -1;
            APU.frame_mode5 = (value & 0x80) != 0;
            APU.frame_irq_inhibit = (value & 0x40) != 0;
            if (APU.frame_irq_inhibit) { APU.frame_irq = false; apu_update_irq(); }
            APU.frame_counter_cycle = 0;
            if (APU.frame_mode5) clock_half();
            return;
        }
    }
    int c = APU.frame_counter_cycle;
    if (!APU.frame_mode5) {
        if (c == 7457) clock_quarter();
        else if (c == 14913) clock_half();
        else if (c == 22371) clock_quarter();
        else if (c == 29829) {
            clock_half();
            if (!APU.frame_irq_inhibit) { APU.frame_irq = true; apu_update_irq(); }
        }
        APU.frame_counter_cycle = c >= 29829 ? 0 : c + 1;
    } else {
        if (c == 7457) clock_quarter();
        else if (c == 14913) clock_half();
        else if (c == 22371) clock_quarter();
        else if (c == 37281) clock_half();
        APU.frame_counter_cycle = c >= 37281 ? 0 : c + 1;
    }
}

/* ── Init / tick ─────────────────────────────────────────────────────────── */
void apu_init(double sample_rate) {
    for (int i = 1; i < 31; i += 1) PULSE_TABLE[i] = (float)(95.52 / (8128.0 / i + 100));
    for (int i = 1; i < 203; i += 1) TND_TABLE[i] = (float)(163.67 / (24329.0 / i + 100));
    APU.pulse1.channel = 1;
    APU.pulse2.channel = 2;
    APU.noise.period = NOISE_PERIODS[0];
    APU.noise.shift = 1;
    APU.dmc.rate = DMC_RATES[0];
    APU.dmc.sample_addr = 0xc000; APU.dmc.sample_length = 1;
    APU.dmc.current_addr = 0xc000;
    APU.dmc.bits_remaining = 8;
    APU.dmc.silence = true;
    APU.dmc.buffer = -1;
    APU.pending_frame_write = -1;
    APU.sample_rate = sample_rate;
    APU.cycles_per_sample = sample_rate > 0 ? 1789773.0 / sample_rate : 0;
}

void apu_tick(void) {
    tick_frame_counter();
    tri_tick(&APU.triangle);
    noise_tick(&APU.noise);
    dmc_tick(&APU.dmc);
    APU.odd_cycle = !APU.odd_cycle;
    if (APU.odd_cycle) {
        pulse_tick2(&APU.pulse1);
        pulse_tick2(&APU.pulse2);
    }

    if (APU.cycles_per_sample > 0) {
        float mix =
            PULSE_TABLE[pulse_output(&APU.pulse1) + pulse_output(&APU.pulse2)] +
            TND_TABLE[3 * tri_output(&APU.triangle) + 2 * noise_output(&APU.noise) + APU.dmc.level];
        APU.sample_acc += mix;
        APU.sample_acc_count += 1;
        APU.sample_counter += 1;
        if (APU.sample_counter >= APU.cycles_per_sample) {
            APU.sample_counter -= APU.cycles_per_sample;
            double raw = APU.sample_acc / APU.sample_acc_count;
            APU.sample_acc = 0; APU.sample_acc_count = 0;
            double out = raw - APU.hp_last + 0.9947 * APU.hp_out;
            APU.hp_last = raw; APU.hp_out = out;
            int head = APU.ring_head;
            int next = (head + 1) & (APU_RING_LEN - 1);
            if (next != APU.ring_tail) {
                APU.ring[head] = (float)out;
                APU.ring_head = next;
            }
        }
    }
}

int apu_available_samples(void) {
    return (APU.ring_head - APU.ring_tail) & (APU_RING_LEN - 1);
}

int apu_read_samples(float *out, int count) {
    int tail = APU.ring_tail;
    int avail = (APU.ring_head - tail) & (APU_RING_LEN - 1);
    int n = count < avail ? count : avail;
    for (int i = 0; i < n; i += 1) {
        out[i] = APU.ring[tail];
        tail = (tail + 1) & (APU_RING_LEN - 1);
    }
    APU.ring_tail = tail;
    return n;
}

/* ── Registers ───────────────────────────────────────────────────────────── */
void apu_write(uint16_t addr, uint8_t value) {
    switch (addr) {
    case 0x4000: case 0x4004: {
        Pulse *p = addr == 0x4000 ? &APU.pulse1 : &APU.pulse2;
        p->duty = (value >> 6) & 3;
        p->length_halt = (value & 0x20) != 0;
        p->env.loop = p->length_halt;
        p->env.constant = (value & 0x10) != 0;
        p->env.volume = value & 0x0f;
        break;
    }
    case 0x4001: case 0x4005: {
        Pulse *p = addr == 0x4001 ? &APU.pulse1 : &APU.pulse2;
        p->sweep_enabled = (value & 0x80) != 0;
        p->sweep_period = (value >> 4) & 7;
        p->sweep_negate = (value & 0x08) != 0;
        p->sweep_shift = value & 7;
        p->sweep_reload = true;
        break;
    }
    case 0x4002: case 0x4006: {
        Pulse *p = addr == 0x4002 ? &APU.pulse1 : &APU.pulse2;
        p->period = (uint16_t)((p->period & 0x700) | value);
        break;
    }
    case 0x4003: case 0x4007: {
        Pulse *p = addr == 0x4003 ? &APU.pulse1 : &APU.pulse2;
        p->period = (uint16_t)((p->period & 0x0ff) | ((value & 7) << 8));
        if (p->enabled) p->length = LENGTH_TABLE[value >> 3];
        p->seq_pos = 0;
        p->env.start = true;
        break;
    }
    case 0x4008:
        APU.triangle.length_halt = (value & 0x80) != 0;
        APU.triangle.linear_reload = value & 0x7f;
        break;
    case 0x400a:
        APU.triangle.period = (uint16_t)((APU.triangle.period & 0x700) | value);
        break;
    case 0x400b:
        APU.triangle.period = (uint16_t)((APU.triangle.period & 0x0ff) | ((value & 7) << 8));
        if (APU.triangle.enabled) APU.triangle.length = LENGTH_TABLE[value >> 3];
        APU.triangle.linear_reload_flag = true;
        break;
    case 0x400c:
        APU.noise.length_halt = (value & 0x20) != 0;
        APU.noise.env.loop = APU.noise.length_halt;
        APU.noise.env.constant = (value & 0x10) != 0;
        APU.noise.env.volume = value & 0x0f;
        break;
    case 0x400e:
        APU.noise.mode = (value & 0x80) != 0;
        APU.noise.period = NOISE_PERIODS[value & 0x0f];
        break;
    case 0x400f:
        if (APU.noise.enabled) APU.noise.length = LENGTH_TABLE[value >> 3];
        APU.noise.env.start = true;
        break;
    case 0x4010:
        APU.dmc.irq_enabled = (value & 0x80) != 0;
        APU.dmc.loop = (value & 0x40) != 0;
        APU.dmc.rate = DMC_RATES[value & 0x0f];
        if (!APU.dmc.irq_enabled) { APU.dmc.irq = false; apu_update_irq(); }
        break;
    case 0x4011:
        APU.dmc.level = value & 0x7f;
        break;
    case 0x4012:
        APU.dmc.sample_addr = (uint16_t)(0xc000 | (value << 6));
        break;
    case 0x4013:
        APU.dmc.sample_length = (uint16_t)((value << 4) | 1);
        break;
    case 0x4015: {
        APU.pulse1.enabled = (value & 0x01) != 0;
        APU.pulse2.enabled = (value & 0x02) != 0;
        APU.triangle.enabled = (value & 0x04) != 0;
        APU.noise.enabled = (value & 0x08) != 0;
        if (!APU.pulse1.enabled) APU.pulse1.length = 0;
        if (!APU.pulse2.enabled) APU.pulse2.length = 0;
        if (!APU.triangle.enabled) APU.triangle.length = 0;
        if (!APU.noise.enabled) APU.noise.length = 0;
        bool dmc_enable = (value & 0x10) != 0;
        APU.dmc.enabled = dmc_enable;
        if (dmc_enable) {
            if (APU.dmc.bytes_remaining == 0) {
                dmc_restart(&APU.dmc);
            }
            dmc_maybe_fetch(&APU.dmc);
        } else {
            APU.dmc.bytes_remaining = 0;
        }
        APU.dmc.irq = false;
        apu_update_irq();
        break;
    }
    case 0x4017:
        APU.pending_frame_write = value;
        APU.pending_frame_delay = APU.odd_cycle ? 4 : 3;
        break;
    default:
        break;
    }
}

uint8_t apu_read_4015(void) {
    uint8_t v = 0;
    if (APU.pulse1.length > 0) v |= 0x01;
    if (APU.pulse2.length > 0) v |= 0x02;
    if (APU.triangle.length > 0) v |= 0x04;
    if (APU.noise.length > 0) v |= 0x08;
    if (APU.dmc.bytes_remaining > 0) v |= 0x10;
    if (APU.frame_irq) v |= 0x40;
    if (APU.dmc.irq) v |= 0x80;
    APU.frame_irq = false;
    apu_update_irq();
    return v;
}
