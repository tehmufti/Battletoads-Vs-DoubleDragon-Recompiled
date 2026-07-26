/*
 * headless.c — verification harness for the recompilation.
 *
 * Loads the user's ROM from disk, plays an input tape (text lines
 * "<frame> <padmask>", meaning pad0 = padmask from that frame onward), and
 * prints one line per frame: "<frame> <fbhash> <ramhash>" where the hashes
 * are FNV-1a 32-bit over the framebuffer bytes (little-endian) and the 2KB
 * system RAM. tools/recomp/verify-recomp.mjs produces the same lines from
 * the JS core and diffs the two streams — frame-exact or fail.
 *
 * Usage: recomp-headless <rom.nes> <frames> [tape.txt] [--bench]
 *        recomp-headless <rom.nes> <frames> [tape.txt] --state-test
 *          (save/load determinism proof: run, save, run K frames hashing,
 *           load, re-run K frames hashing — the two hash streams must match)
 */
#include "nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t h) {
    for (size_t i = 0; i < len; i += 1) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

typedef struct { int frame; int mask; } TapeEvent;

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom.nes> <frames> [tape.txt] [--bench]\n", argv[0]);
        return 2;
    }
    const char *rom_path = argv[1];
    int frames = atoi(argv[2]);
    const char *tape_path = NULL;
    int bench = 0, state_test = 0;
    for (int i = 3; i < argc; i += 1) {
        if (strcmp(argv[i], "--bench") == 0) bench = 1;
        else if (strcmp(argv[i], "--state-test") == 0) state_test = 1;
        else tape_path = argv[i];
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "cannot open rom: %s\n", rom_path); return 2; }
    fseek(f, 0, SEEK_END);
    long rom_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc((size_t)rom_len);
    if (fread(rom, 1, (size_t)rom_len, f) != (size_t)rom_len) { fclose(f); return 2; }
    fclose(f);

    TapeEvent *tape = NULL;
    int tape_len = 0;
    if (tape_path) {
        FILE *tf = fopen(tape_path, "r");
        if (!tf) { fprintf(stderr, "cannot open tape: %s\n", tape_path); return 2; }
        int cap = 256;
        tape = (TapeEvent *)malloc(sizeof(TapeEvent) * cap);
        int fr, mask;
        while (fscanf(tf, "%d %d", &fr, &mask) == 2) {
            if (tape_len == cap) { cap *= 2; tape = (TapeEvent *)realloc(tape, sizeof(TapeEvent) * cap); }
            tape[tape_len].frame = fr;
            tape[tape_len].mask = mask;
            tape_len += 1;
        }
        fclose(tf);
    }

    int rc = nes_init(rom, (size_t)rom_len, 0);
    free(rom);
    if (rc != 0) { fprintf(stderr, "nes_init failed: %d\n", rc); return 2; }

    if (state_test) {
        /* determinism proof for save states: warm up, save, hash K frames,
         * load, hash the same K frames again — streams must be identical */
        const int K = 240;
        int tape_pos = 0, pad = 0;
        for (int fr = 1; fr <= frames; fr += 1) {
            while (tape_pos < tape_len && tape[tape_pos].frame <= fr) {
                pad = tape[tape_pos].mask;
                tape_pos += 1;
            }
            nes_set_buttons(0, (uint8_t)pad);
            nes_run_frame();
        }
        const char *sp = "recomp-state-test.sav";
        if (nes_save_state(sp) != 0) { fprintf(stderr, "state save failed\n"); return 1; }
        uint32_t h1[240], h2[240];
        for (int i = 0; i < K; i += 1) {
            nes_set_buttons(0, (uint8_t)((i & 8) ? 0x82 : 0x80));
            nes_run_frame();
            h1[i] = fnv1a((const uint8_t *)PPU.framebuffer, sizeof PPU.framebuffer,
                          fnv1a(M.ram, sizeof M.ram, 2166136261u));
        }
        if (nes_load_state(sp) != 0) { fprintf(stderr, "state load failed\n"); return 1; }
        for (int i = 0; i < K; i += 1) {
            nes_set_buttons(0, (uint8_t)((i & 8) ? 0x82 : 0x80));
            nes_run_frame();
            h2[i] = fnv1a((const uint8_t *)PPU.framebuffer, sizeof PPU.framebuffer,
                          fnv1a(M.ram, sizeof M.ram, 2166136261u));
        }
        remove(sp);
        for (int i = 0; i < K; i += 1) {
            if (h1[i] != h2[i]) {
                fprintf(stderr, "STATE TEST FAILED at +%d: %08x vs %08x\n", i, h1[i], h2[i]);
                return 1;
            }
        }
        fprintf(stderr, "STATE TEST PASSED: save/load deterministic over %d frames\n", K);
        return 0;
    }

    clock_t t0 = clock();
    int tape_pos = 0;
    int pad = 0;
    for (int fr = 1; fr <= frames; fr += 1) {
        while (tape_pos < tape_len && tape[tape_pos].frame <= fr) {
            pad = tape[tape_pos].mask;
            tape_pos += 1;
        }
        nes_set_buttons(0, (uint8_t)pad);
        nes_run_frame();
        if (!bench) {
            uint32_t fb = fnv1a((const uint8_t *)PPU.framebuffer, sizeof PPU.framebuffer, 2166136261u);
            uint32_t ram = fnv1a(M.ram, sizeof M.ram, 2166136261u);
            printf("%d %08x %08x\n", fr, fb, ram);
        }
    }
    if (bench) {
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        fprintf(stderr, "bench: %d frames in %.3fs = %.1f fps (%.1fx realtime)%s\n",
                frames, secs, frames / secs, frames / secs / 60.0988,
                recomp_enabled ? " [recomp]" : " [interp]");
        fprintf(stderr, "execution mix: %llu native / %llu interpreted (%.2f%% native)\n",
                (unsigned long long)M.native_instr, (unsigned long long)M.interp_instr,
                100.0 * (double)M.native_instr / (double)(M.native_instr + M.interp_instr + 1));
    }
    return 0;
}
