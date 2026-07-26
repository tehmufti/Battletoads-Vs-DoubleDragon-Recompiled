# Battletoads & Double Dragon — Static Recompilation (NES → C)

This repository turns *Battletoads & Double Dragon (U)* into a **native
Windows program built from C source** — not an emulator interpreting opcodes
at its core. A generator translates the ROM's 6502 code into C (one function
per AxROM bank), which links against a small hand-written runtime (CPU
fallback interpreter, dot-accurate PPU, full APU with DMC, bus/mapper) and a
pure Win32 frontend. **96%+ of executed instructions run as recompiled
native code**, verified frame-exact against a reference emulator.

**No ROM, no game code, and no binaries are included.** You generate
everything locally from your own legally-obtained copy — the same posture as
the N64 recompilation projects. The repo contains only original code plus a
code/data address map (metadata).

## Requirements

- Windows 10/11
- Visual Studio 2022 (any edition, with the "Desktop development with C++"
  workload) — the build script finds it via `vswhere`
- Node.js 18+ (runs the generator and the verification harness)
- Your own `Battletoads Double Dragon (U).nes` (iNES, mapper 7 / AxROM,
  256 KB PRG)

## Quick start

```
1. copy your ROM into this folder as:  Battletoads Double Dragon (U).nes
2. npm run gen        → translates the ROM's code into gen/bank0..7.c
3. npm run build      → compiles Battletoads-Recomp.exe
4. run Battletoads-Recomp.exe (keep it next to the ROM)
```

## The exe

- **START GAME**
- **SPRITE LIMIT [X]** — ticked = the authentic 8-sprites-per-scanline
  hardware limit (with its flicker); untick to render every sprite
  (flicker-free). The sprite-overflow status flag is still raised either
  way, so game logic is unaffected.
- **CONTROLS** — remap every Player-1 key, including **save state** and
  **load state** (defaults: F5 / F7, single slot, stored next to the exe).
  Player 2 is fixed on WASD + F/G. Settings persist in
  `recomp-settings.txt`.

## Verifying your build

```
npm run build:headless
npm run verify
```

`verify` replays a scripted boot-to-gameplay input tape on both the bundled
JS reference core (`ref-core/`) and your C build, hashing the framebuffer
and system RAM **every frame** — the run passes only if thousands of frames
are bit-identical. Save-state determinism has its own proof:

```
build\recomp-headless.exe "Battletoads Double Dragon (U).nes" 2200 build\verify-tape.txt --state-test
```

## How it works

- `data/decomp-coverage.json` — one byte per PRG byte marking instruction
  starts (collected upstream by runtime tracing + static traversal of the
  game; ~22k instructions across 8 banks). This is what separates code from
  data. Addresses only — no ROM content.
- `tools/gen-c.mjs` — the recompiler. Each covered instruction becomes C
  that performs the **exact same bus-access sequence** as the interpreter
  (opcode fetch, operand fetches, dummy reads, RMW double-writes), so cycle
  accuracy is preserved — the master clock advances 3 PPU dots + 1 APU
  cycle per bus access, which is why mid-frame raster tricks (the HUD
  split, CHR streaming) render correctly. Branches/JMP/JSR with known
  targets become direct `goto`s; RTS/indirect jumps re-enter through a
  per-bank address switch; stores that can hit $8000+ re-check the AxROM
  bank mapping.
- `runtime/` — the machine. Anything not covered (RAM-executed stubs, rare
  paths) falls back to the built-in cycle-accurate interpreter seamlessly.
- `runtime/state.c` — save states as frame-boundary struct snapshots
  (build-specific files).
- The exe loads the ROM at startup for all data (graphics, tables, music) —
  which also means data-only ROM hacks apply without regenerating; code
  edits need `npm run gen` again.

## Modding

The generated `gen/bankN.c` files are readable C with address labels
(`L_9291:` = the routine at $9291), and the whole machine is four plain C
structs (`CPU`, `PPU`, `APU`, `M` — RAM is `M.ram`). Breakpoints, watches,
one-line cheats (`M.ram[0x5d1] = 3;` = infinite lives), or whole new C
systems are all on the table. Note that `npm run gen` overwrites `gen/`,
so keep hand modifications in separate files or patches.

## Legal

This project ships no copyrighted material: no ROM, no game data, no
generated code, no built binaries (all are `.gitignore`d). The coverage map
is address metadata. Everything you generate from your own cartridge dump
stays on your machine. This is a fan preservation/interoperability project,
unaffiliated with the rights holders; don't distribute your generated
output or your ROM.

