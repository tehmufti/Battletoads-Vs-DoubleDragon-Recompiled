/*
 * state.c — save states for the recompilation.
 *
 * Raw struct dumps of the four machine components, taken at a frame
 * boundary. The PRG pointer/length are preserved across load (the ROM is
 * immutable and owned by the running process). A header with struct sizes
 * rejects states from a different build (layout is compiler-dependent —
 * states are per-build files, documented in the README).
 */
#include "nes.h"
#include <stdio.h>
#include <string.h>

#define STATE_MAGIC 0x42544444u /* "BTDD" */
#define STATE_VERSION 1u

typedef struct {
    uint32_t magic, version;
    uint32_t cpu_size, ppu_size, apu_size, machine_size;
} StateHeader;

int nes_save_state(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    StateHeader h = {
        STATE_MAGIC, STATE_VERSION,
        (uint32_t)sizeof(Cpu), (uint32_t)sizeof(Ppu),
        (uint32_t)sizeof(Apu), (uint32_t)sizeof(Machine),
    };
    Machine m = M;
    m.prg = NULL; /* never serialize the ROM pointer */
    int ok = fwrite(&h, sizeof h, 1, f) == 1 &&
             fwrite(&CPU, sizeof CPU, 1, f) == 1 &&
             fwrite(&PPU, sizeof PPU, 1, f) == 1 &&
             fwrite(&APU, sizeof APU, 1, f) == 1 &&
             fwrite(&m, sizeof m, 1, f) == 1;
    fclose(f);
    return ok ? 0 : -2;
}

int nes_load_state(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    StateHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != STATE_MAGIC ||
        h.version != STATE_VERSION ||
        h.cpu_size != sizeof(Cpu) || h.ppu_size != sizeof(Ppu) ||
        h.apu_size != sizeof(Apu) || h.machine_size != sizeof(Machine)) {
        fclose(f);
        return -2; /* different build or not a state file */
    }
    uint8_t *prg = M.prg;
    size_t prg_len = M.prg_len;
    Machine m;
    int ok = fread(&CPU, sizeof CPU, 1, f) == 1 &&
             fread(&PPU, sizeof PPU, 1, f) == 1 &&
             fread(&APU, sizeof APU, 1, f) == 1 &&
             fread(&m, sizeof m, 1, f) == 1;
    fclose(f);
    if (!ok) return -3;
    M = m;
    M.prg = prg;
    M.prg_len = prg_len;
    return 0;
}
