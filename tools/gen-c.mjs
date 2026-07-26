/*
 * gen-c.mjs — the static recompiler: 6502 → C.
 *
 * Reads the ROM plus generated/decomp-coverage.json (one byte per PRG byte;
 * nonzero = instruction start, collected by runtime tracing + static
 * traversal) and emits recomp/gen/bank0..7.c, one dispatch function per
 * 32KB AxROM bank, plus recomp_dispatch.c.
 *
 * Translation contract (what makes the output cycle-accurate): every emitted
 * instruction performs the EXACT same bus-access sequence as the interpreter
 * in recomp/runtime/cpu.c — opcode fetch, operand fetches, dummy reads,
 * RMW double-writes — via the ticked RD/WR macros. The C compiler then
 * removes the decode overhead the interpreter pays per instruction.
 *
 * Control flow:
 *  - branches/JMP/JSR with in-bank covered targets become direct `goto`s
 *  - RTS/RTI/JMP($) re-enter through the bank's address switch
 *  - stores that can hit $8000+ re-check the mapped bank (AxROM bank switch)
 *  - unknown/unofficial opcodes and RAM execution fall back to the
 *    interpreter (no case emitted → dispatcher returns)
 *  - every instruction polls NMI/IRQ/frame-done first, same as cpu_step()
 *
 * Usage: node tools/recomp/gen-c.mjs
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const rom = fs.readFileSync(path.join(root, "Battletoads Double Dragon (U).nes"));
const prg = rom.subarray(16, 16 + 256 * 1024);
const covJson = JSON.parse(fs.readFileSync(path.join(root, "data", "decomp-coverage.json"), "utf8"));
const cov = Buffer.from(covJson.bitmap, "base64");
const outDir = path.join(root, "gen");
fs.mkdirSync(outDir, { recursive: true });

/* ── opcode table: official set, [kind, mode] ─────────────────────────────── */
/* modes: imp acc imm zp zpx zpy abs abx aby izx izy ind rel
 * abx/aby/izy split into read (r) and write (w) variants per kind. */
const T = {};
function def(op, kind, mode) { T[op] = { kind, mode }; }
/* loads */
def(0xa9, "LDA", "imm"); def(0xa5, "LDA", "zp"); def(0xb5, "LDA", "zpx");
def(0xad, "LDA", "abs"); def(0xbd, "LDA", "abxr"); def(0xb9, "LDA", "abyr");
def(0xa1, "LDA", "izx"); def(0xb1, "LDA", "izyr");
def(0xa2, "LDX", "imm"); def(0xa6, "LDX", "zp"); def(0xb6, "LDX", "zpy");
def(0xae, "LDX", "abs"); def(0xbe, "LDX", "abyr");
def(0xa0, "LDY", "imm"); def(0xa4, "LDY", "zp"); def(0xb4, "LDY", "zpx");
def(0xac, "LDY", "abs"); def(0xbc, "LDY", "abxr");
/* stores */
def(0x85, "STA", "zp"); def(0x95, "STA", "zpx"); def(0x8d, "STA", "abs");
def(0x9d, "STA", "abxw"); def(0x99, "STA", "abyw"); def(0x81, "STA", "izx"); def(0x91, "STA", "izyw");
def(0x86, "STX", "zp"); def(0x96, "STX", "zpy"); def(0x8e, "STX", "abs");
def(0x84, "STY", "zp"); def(0x94, "STY", "zpx"); def(0x8c, "STY", "abs");
/* transfers */
def(0xaa, "TAX", "imp"); def(0xa8, "TAY", "imp"); def(0x8a, "TXA", "imp");
def(0x98, "TYA", "imp"); def(0xba, "TSX", "imp"); def(0x9a, "TXS", "imp");
/* alu */
for (const [base, kind] of [[0x69, "ADC"], [0xe9, "SBC"], [0x29, "AND"], [0x09, "ORA"], [0x49, "EOR"], [0xc9, "CMP"]]) {
  const modes = kind === "CMP" || kind === "ADC" || kind === "SBC" || kind === "AND" || kind === "ORA" || kind === "EOR"
    ? null : null;
  void modes; void base;
}
def(0x69, "ADC", "imm"); def(0x65, "ADC", "zp"); def(0x75, "ADC", "zpx");
def(0x6d, "ADC", "abs"); def(0x7d, "ADC", "abxr"); def(0x79, "ADC", "abyr");
def(0x61, "ADC", "izx"); def(0x71, "ADC", "izyr");
def(0xe9, "SBC", "imm"); def(0xe5, "SBC", "zp"); def(0xf5, "SBC", "zpx");
def(0xed, "SBC", "abs"); def(0xfd, "SBC", "abxr"); def(0xf9, "SBC", "abyr");
def(0xe1, "SBC", "izx"); def(0xf1, "SBC", "izyr");
def(0x29, "AND", "imm"); def(0x25, "AND", "zp"); def(0x35, "AND", "zpx");
def(0x2d, "AND", "abs"); def(0x3d, "AND", "abxr"); def(0x39, "AND", "abyr");
def(0x21, "AND", "izx"); def(0x31, "AND", "izyr");
def(0x09, "ORA", "imm"); def(0x05, "ORA", "zp"); def(0x15, "ORA", "zpx");
def(0x0d, "ORA", "abs"); def(0x1d, "ORA", "abxr"); def(0x19, "ORA", "abyr");
def(0x01, "ORA", "izx"); def(0x11, "ORA", "izyr");
def(0x49, "EOR", "imm"); def(0x45, "EOR", "zp"); def(0x55, "EOR", "zpx");
def(0x4d, "EOR", "abs"); def(0x5d, "EOR", "abxr"); def(0x59, "EOR", "abyr");
def(0x41, "EOR", "izx"); def(0x51, "EOR", "izyr");
def(0xc9, "CMP", "imm"); def(0xc5, "CMP", "zp"); def(0xd5, "CMP", "zpx");
def(0xcd, "CMP", "abs"); def(0xdd, "CMP", "abxr"); def(0xd9, "CMP", "abyr");
def(0xc1, "CMP", "izx"); def(0xd1, "CMP", "izyr");
def(0xe0, "CPX", "imm"); def(0xe4, "CPX", "zp"); def(0xec, "CPX", "abs");
def(0xc0, "CPY", "imm"); def(0xc4, "CPY", "zp"); def(0xcc, "CPY", "abs");
def(0x24, "BIT", "zp"); def(0x2c, "BIT", "abs");
/* shifts / rmw */
def(0x0a, "ASLA", "acc"); def(0x06, "ASL", "zp"); def(0x16, "ASL", "zpx");
def(0x0e, "ASL", "abs"); def(0x1e, "ASL", "abxw");
def(0x4a, "LSRA", "acc"); def(0x46, "LSR", "zp"); def(0x56, "LSR", "zpx");
def(0x4e, "LSR", "abs"); def(0x5e, "LSR", "abxw");
def(0x2a, "ROLA", "acc"); def(0x26, "ROL", "zp"); def(0x36, "ROL", "zpx");
def(0x2e, "ROL", "abs"); def(0x3e, "ROL", "abxw");
def(0x6a, "RORA", "acc"); def(0x66, "ROR", "zp"); def(0x76, "ROR", "zpx");
def(0x6e, "ROR", "abs"); def(0x7e, "ROR", "abxw");
def(0xe6, "INC", "zp"); def(0xf6, "INC", "zpx"); def(0xee, "INC", "abs"); def(0xfe, "INC", "abxw");
def(0xc6, "DEC", "zp"); def(0xd6, "DEC", "zpx"); def(0xce, "DEC", "abs"); def(0xde, "DEC", "abxw");
def(0xe8, "INX", "imp"); def(0xc8, "INY", "imp"); def(0xca, "DEX", "imp"); def(0x88, "DEY", "imp");
/* flags */
def(0x18, "CLC", "imp"); def(0x38, "SEC", "imp"); def(0x58, "CLI", "imp"); def(0x78, "SEI", "imp");
def(0xb8, "CLV", "imp"); def(0xd8, "CLD", "imp"); def(0xf8, "SED", "imp");
/* flow */
def(0x4c, "JMP", "abs"); def(0x6c, "JMPI", "ind"); def(0x20, "JSR", "abs");
def(0x60, "RTS", "imp"); def(0x40, "RTI", "imp");
def(0x10, "BPL", "rel"); def(0x30, "BMI", "rel"); def(0x50, "BVC", "rel"); def(0x70, "BVS", "rel");
def(0x90, "BCC", "rel"); def(0xb0, "BCS", "rel"); def(0xd0, "BNE", "rel"); def(0xf0, "BEQ", "rel");
/* stack */
def(0x48, "PHA", "imp"); def(0x08, "PHP", "imp"); def(0x68, "PLA", "imp"); def(0x28, "PLP", "imp");
def(0xea, "NOP", "imp");

const LEN = { imp: 1, acc: 1, imm: 2, zp: 2, zpx: 2, zpy: 2, abs: 3, abxr: 3, abxw: 3, abyr: 3, abyw: 3, izx: 2, izyr: 2, izyw: 2, ind: 3, rel: 2 };

/* ── emission helpers ─────────────────────────────────────────────────────── */
const hex = (v) => "0x" + v.toString(16);

/* operand-address emission: returns { code, ea, dynamic } — `code` computes
 * `ea` (a C expression or variable), replicating the interpreter's exact
 * cycle pattern for the mode. */
function emitAddr(mode, addr, bank) {
  const lo = prg[bank * 0x8000 + ((addr + 1) & 0x7fff)];
  const hi = prg[bank * 0x8000 + ((addr + 2) & 0x7fff)];
  const abs = lo | (hi << 8);
  switch (mode) {
    case "imm":
      return { code: `uint8_t v = RD(CPU.pc); CPU.pc += 1;`, val: "v" };
    case "zp":
      return { code: `uint16_t ea = RD(CPU.pc); CPU.pc += 1;`, ea: "ea", dynamic: false, stat: lo };
    case "zpx":
      return { code: `uint16_t z = RD(CPU.pc); CPU.pc += 1; RD(z); uint16_t ea = (uint8_t)(z + CPU.x);`, ea: "ea", dynamic: false };
    case "zpy":
      return { code: `uint16_t z = RD(CPU.pc); CPU.pc += 1; RD(z); uint16_t ea = (uint8_t)(z + CPU.y);`, ea: "ea", dynamic: false };
    case "abs":
      return {
        code: `uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1; uint16_t ea = (uint16_t)(lo | (hi << 8));`,
        ea: "ea", dynamic: false, stat: abs,
      };
    case "abxr":
      return {
        code: `uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1; uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.x; if ((int)lo + CPU.x > 0xff) RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    case "abxw":
      return {
        code: `uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1; uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.x; RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    case "abyr":
      return {
        code: `uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1; uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.y; if ((int)lo + CPU.y > 0xff) RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    case "abyw":
      return {
        code: `uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1; uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.y; RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    case "izx":
      return {
        code: `uint8_t z = RD(CPU.pc); CPU.pc += 1; RD(z); uint8_t lo = RD((uint8_t)(z + CPU.x)); uint8_t hi = RD((uint8_t)(z + CPU.x + 1)); uint16_t ea = (uint16_t)(lo | (hi << 8));`,
        ea: "ea", dynamic: true,
      };
    case "izyr":
      return {
        code: `uint8_t z = RD(CPU.pc); CPU.pc += 1; uint8_t lo = RD(z); uint8_t hi = RD((uint8_t)(z + 1)); uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.y; if ((int)lo + CPU.y > 0xff) RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    case "izyw":
      return {
        code: `uint8_t z = RD(CPU.pc); CPU.pc += 1; uint8_t lo = RD(z); uint8_t hi = RD((uint8_t)(z + 1)); uint16_t base = (uint16_t)(lo | (hi << 8)); uint16_t ea = base + CPU.y; RD((base & 0xff00) | (ea & 0xff));`,
        ea: "ea", dynamic: true,
      };
    default:
      throw new Error("bad mode " + mode);
  }
}

const RMW_FN = { ASL: "cpu_asl", LSR: "cpu_lsr", ROL: "cpu_rol", ROR: "cpu_ror" };

/* one instruction's body (no label, no IPOLL — caller adds those).
 * Returns { body, next } where next describes fallthrough:
 *   { kind: "fall", addr } | { kind: "stop" } (control transferred already) */
function emitInstr(bank, addr, labels) {
  const op = prg[bank * 0x8000 + (addr & 0x7fff)];
  const t = T[op];
  if (!t) return null; /* unsupported → interpreter */
  const len = LEN[t.mode];
  if ((addr & 0x7fff) + len > 0x8000) return null; /* crosses bank end */
  const nextAddr = (addr + len) & 0xffff;
  const L = [];
  const bankCheck = `if ((M.prg_bank & 7) != ${bank}) return 0;`;
  /* jump target helper: goto if the target is a label in this bank */
  const jumpTo = (target) =>
    labels.has(target) ? `goto L_${target.toString(16)};` : `return 0;`;

  L.push(`RD(CPU.pc); CPU.pc += 1;`); /* opcode fetch */

  const simpleLoad = { LDA: "CPU.a", LDX: "CPU.x", LDY: "CPU.y" };
  const simpleStore = { STA: "CPU.a", STX: "CPU.x", STY: "CPU.y" };
  const aluCall = { ADC: "cpu_adc", SBC: "cpu_sbc" };
  const aluBin = { AND: "&", ORA: "|", EOR: "^" };
  const cmpReg = { CMP: "CPU.a", CPX: "CPU.x", CPY: "CPU.y" };
  const branches = {
    BPL: "(CPU.p & FN) == 0", BMI: "(CPU.p & FN) != 0",
    BVC: "(CPU.p & FV) == 0", BVS: "(CPU.p & FV) != 0",
    BCC: "(CPU.p & FC) == 0", BCS: "(CPU.p & FC) != 0",
    BNE: "(CPU.p & FZ) == 0", BEQ: "(CPU.p & FZ) != 0",
  };

  const k = t.kind;
  if (k in simpleLoad) {
    if (t.mode === "imm") {
      const a = emitAddr("imm", addr, bank);
      L.push(a.code, `${simpleLoad[k]} = cpu_set_zn(v);`);
    } else {
      const a = emitAddr(t.mode, addr, bank);
      L.push(a.code, `${simpleLoad[k]} = cpu_set_zn(RD(${a.ea}));`);
    }
  } else if (k in simpleStore) {
    const a = emitAddr(t.mode, addr, bank);
    L.push(a.code, `WR(${a.ea}, ${simpleStore[k]});`);
    /* AxROM: a store that lands in $8000+ switches the mapped bank */
    if (a.dynamic) L.push(`if (ea >= 0x8000) { ${bankCheck} }`);
    else if (a.stat !== undefined && a.stat >= 0x8000) L.push(bankCheck);
  } else if (k in aluCall) {
    const a = emitAddr(t.mode, addr, bank);
    L.push(a.code, t.mode === "imm" ? `${aluCall[k]}(v);` : `${aluCall[k]}(RD(${a.ea}));`);
  } else if (k in aluBin) {
    const a = emitAddr(t.mode, addr, bank);
    const src = t.mode === "imm" ? "v" : `RD(${a.ea})`;
    L.push(a.code, `CPU.a = cpu_set_zn(CPU.a ${aluBin[k]} ${src});`);
  } else if (k in cmpReg) {
    const a = emitAddr(t.mode, addr, bank);
    const src = t.mode === "imm" ? "v" : `RD(${a.ea})`;
    L.push(a.code, `cpu_cmp(${cmpReg[k]}, ${src});`);
  } else if (k === "BIT") {
    const a = emitAddr(t.mode, addr, bank);
    L.push(a.code, `cpu_bit(RD(${a.ea}));`);
  } else if (k === "ASLA" || k === "LSRA" || k === "ROLA" || k === "RORA") {
    const fn = RMW_FN[k.slice(0, 3)];
    L.push(`RD(CPU.pc); CPU.a = ${fn}(CPU.a);`);
  } else if (k in RMW_FN || k === "INC" || k === "DEC") {
    const a = emitAddr(t.mode, addr, bank);
    const fnExpr = k === "INC" ? `cpu_set_zn((uint8_t)(m0 + 1))`
      : k === "DEC" ? `cpu_set_zn((uint8_t)(m0 - 1))`
      : `${RMW_FN[k]}(m0)`;
    L.push(a.code,
      `uint8_t m0 = RD(${a.ea}); WR(${a.ea}, m0); uint8_t m1 = ${fnExpr}; WR(${a.ea}, m1);`);
    if (a.dynamic) L.push(`if (ea >= 0x8000) { ${bankCheck} }`);
    else if (a.stat !== undefined && a.stat >= 0x8000) L.push(bankCheck);
  } else if (k === "TAX") { L.push(`RD(CPU.pc); CPU.x = cpu_set_zn(CPU.a);`);
  } else if (k === "TAY") { L.push(`RD(CPU.pc); CPU.y = cpu_set_zn(CPU.a);`);
  } else if (k === "TXA") { L.push(`RD(CPU.pc); CPU.a = cpu_set_zn(CPU.x);`);
  } else if (k === "TYA") { L.push(`RD(CPU.pc); CPU.a = cpu_set_zn(CPU.y);`);
  } else if (k === "TSX") { L.push(`RD(CPU.pc); CPU.x = cpu_set_zn(CPU.sp);`);
  } else if (k === "TXS") { L.push(`RD(CPU.pc); CPU.sp = CPU.x;`);
  } else if (k === "INX") { L.push(`RD(CPU.pc); CPU.x = cpu_set_zn((uint8_t)(CPU.x + 1));`);
  } else if (k === "INY") { L.push(`RD(CPU.pc); CPU.y = cpu_set_zn((uint8_t)(CPU.y + 1));`);
  } else if (k === "DEX") { L.push(`RD(CPU.pc); CPU.x = cpu_set_zn((uint8_t)(CPU.x - 1));`);
  } else if (k === "DEY") { L.push(`RD(CPU.pc); CPU.y = cpu_set_zn((uint8_t)(CPU.y - 1));`);
  } else if (k === "CLC") { L.push(`RD(CPU.pc); CPU.p &= (uint8_t)~FC;`);
  } else if (k === "SEC") { L.push(`RD(CPU.pc); CPU.p |= FC;`);
  } else if (k === "CLI") { L.push(`RD(CPU.pc); CPU.p &= (uint8_t)~FI;`);
  } else if (k === "SEI") { L.push(`RD(CPU.pc); CPU.p |= FI;`);
  } else if (k === "CLV") { L.push(`RD(CPU.pc); CPU.p &= (uint8_t)~FV;`);
  } else if (k === "CLD") { L.push(`RD(CPU.pc); CPU.p &= (uint8_t)~FD;`);
  } else if (k === "SED") { L.push(`RD(CPU.pc); CPU.p |= FD;`);
  } else if (k === "NOP") { L.push(`RD(CPU.pc);`);
  } else if (k === "PHA") { L.push(`RD(CPU.pc); cpu_push(CPU.a);`);
  } else if (k === "PHP") { L.push(`RD(CPU.pc); cpu_push((uint8_t)(CPU.p | FB | FU));`);
  } else if (k === "PLA") { L.push(`RD(CPU.pc); RD(0x100 | CPU.sp); CPU.a = cpu_set_zn(cpu_pull());`);
  } else if (k === "PLP") { L.push(`RD(CPU.pc); RD(0x100 | CPU.sp); CPU.p = (uint8_t)((cpu_pull() & ~FB) | FU);`);
  } else if (k in branches) {
    const off = prg[bank * 0x8000 + ((addr + 1) & 0x7fff)];
    const rel = off < 0x80 ? off : off - 0x100;
    const target = (addr + 2 + rel) & 0xffff;
    const crosses = ((addr + 2) & 0xff00) !== (target & 0xff00);
    L.push(`RD(CPU.pc); CPU.pc += 1;`);
    L.push(`if (${branches[k]}) {`);
    L.push(`  RD(CPU.pc);`);
    if (crosses) L.push(`  RD((CPU.pc & 0xff00) | ${hex(target & 0xff)});`);
    L.push(`  CPU.pc = ${hex(target)};`);
    L.push(`  ${jumpTo(target)}`);
    L.push(`}`);
    return { body: L, next: { kind: "fall", addr: nextAddr } };
  } else if (k === "JMP") {
    const lo = prg[bank * 0x8000 + ((addr + 1) & 0x7fff)];
    const hi2 = prg[bank * 0x8000 + ((addr + 2) & 0x7fff)];
    const target = lo | (hi2 << 8);
    L.push(`RD(CPU.pc); CPU.pc += 1; RD(CPU.pc); CPU.pc += 1;`);
    L.push(`CPU.pc = ${hex(target)};`);
    L.push(jumpTo(target));
    return { body: L, next: { kind: "stop" } };
  } else if (k === "JMPI") {
    L.push(`uint8_t lo = RD(CPU.pc); CPU.pc += 1; uint8_t hi = RD(CPU.pc); CPU.pc += 1;`);
    L.push(`uint16_t ptr = (uint16_t)(lo | (hi << 8));`);
    L.push(`uint8_t tlo = RD(ptr); uint8_t thi = RD((ptr & 0xff00) | ((ptr + 1) & 0xff));`);
    L.push(`CPU.pc = (uint16_t)(tlo | (thi << 8));`);
    L.push(`goto dispatch;`);
    return { body: L, next: { kind: "stop" } };
  } else if (k === "JSR") {
    const lo = prg[bank * 0x8000 + ((addr + 1) & 0x7fff)];
    const hi2 = prg[bank * 0x8000 + ((addr + 2) & 0x7fff)];
    const target = lo | (hi2 << 8);
    L.push(`RD(CPU.pc); CPU.pc += 1;`);                    /* fetch lo */
    L.push(`RD(0x100 | CPU.sp);`);                          /* internal */
    L.push(`cpu_push((uint8_t)(CPU.pc >> 8)); cpu_push((uint8_t)(CPU.pc & 0xff));`);
    L.push(`RD(CPU.pc);`);                                  /* fetch hi */
    L.push(`CPU.pc = ${hex(target)};`);
    L.push(jumpTo(target));
    return { body: L, next: { kind: "stop" } };
  } else if (k === "RTS") {
    L.push(`RD(CPU.pc); RD(0x100 | CPU.sp);`);
    L.push(`{ uint8_t lo = cpu_pull(); uint8_t hi = cpu_pull(); CPU.pc = (uint16_t)(lo | (hi << 8)); }`);
    L.push(`RD(CPU.pc); CPU.pc += 1;`);
    L.push(`goto dispatch;`);
    return { body: L, next: { kind: "stop" } };
  } else if (k === "RTI") {
    L.push(`RD(CPU.pc); RD(0x100 | CPU.sp);`);
    L.push(`CPU.p = (uint8_t)((cpu_pull() & ~FB) | FU);`);
    L.push(`{ uint8_t lo = cpu_pull(); uint8_t hi = cpu_pull(); CPU.pc = (uint16_t)(lo | (hi << 8)); }`);
    L.push(`goto dispatch;`);
    return { body: L, next: { kind: "stop" } };
  } else {
    return null;
  }
  return { body: L, next: { kind: "fall", addr: nextAddr } };
}

/* ── per-bank generation ──────────────────────────────────────────────────── */
let totalEmitted = 0;
let totalSkipped = 0;

for (let bank = 0; bank < 8; bank += 1) {
  const starts = [];
  for (let i = 0; i < 0x8000; i += 1) {
    if (cov[bank * 0x8000 + i]) starts.push(0x8000 + i);
  }
  /* first pass: which starts have supported emissions (labels) */
  const labels = new Set();
  for (const addr of starts) {
    const op = prg[bank * 0x8000 + (addr & 0x7fff)];
    const t = T[op];
    if (t && (addr & 0x7fff) + LEN[t.mode] <= 0x8000) labels.add(addr);
  }

  const out = [];
  out.push(`/* bank${bank}.c — generated by tools/recomp/gen-c.mjs; do not edit. */`);
  out.push(`#include "../runtime/nes.h"`);
  out.push(`#include "recomp_gen.h"`);
  out.push(``);
  out.push(`int bank${bank}_run(void) {`);
  out.push(`  goto dispatch;`);
  out.push(`dispatch:`);
  out.push(`  if (CPU.nmi_pending || (M.irq_line != 0 && (CPU.p & FI) == 0) || M.frame_done || CPU.jammed) return 1;`);
  out.push(`  if (CPU.pc < 0x8000 || (M.prg_bank & 7) != ${bank}) return 0;`);
  out.push(`  switch (CPU.pc) {`);
  for (const addr of labels) {
    out.push(`  case ${hex(addr)}: goto L_${addr.toString(16)};`);
  }
  out.push(`  default: return 0;`);
  out.push(`  }`);

  let emitted = 0;
  const sorted = [...labels].sort((a, b) => a - b);
  for (let i = 0; i < sorted.length; i += 1) {
    const addr = sorted[i];
    const r = emitInstr(bank, addr, labels);
    if (!r) { totalSkipped += 1; continue; }
    emitted += 1;
    out.push(`L_${addr.toString(16)}: IPOLL;`);
    out.push(`  {`);
    for (const line of r.body) out.push(`    ${line}`);
    out.push(`  }`);
    if (r.next.kind === "fall") {
      const nxt = r.next.addr;
      if (!(i + 1 < sorted.length && sorted[i + 1] === nxt)) {
        /* fallthrough target is not the next emitted label */
        out.push(labels.has(nxt) ? `  goto L_${nxt.toString(16)};` : `  return 0;`);
      }
    }
  }
  out.push(`}`);
  fs.writeFileSync(path.join(outDir, `bank${bank}.c`), out.join("\n") + "\n");
  console.log(`bank${bank}.c: ${emitted} instructions, ${labels.size} labels`);
  totalEmitted += emitted;
}

/* ── shared header + dispatcher ───────────────────────────────────────────── */
fs.writeFileSync(path.join(outDir, "recomp_gen.h"), `/* generated */
#ifndef RECOMP_GEN_H
#define RECOMP_GEN_H
#include "../runtime/nes.h"
#define RD(a) (CPU.cycles += 1, bus_rd((uint16_t)(a)))
#define WR(a, v) (CPU.cycles += 1, bus_wr((uint16_t)(a), (uint8_t)(v)))
#define IPOLL M.native_instr += 1; if (CPU.nmi_pending || (M.irq_line != 0 && (CPU.p & FI) == 0) || M.frame_done) return 1
#endif
`);

fs.writeFileSync(path.join(outDir, "recomp_dispatch.c"), `/* generated dispatcher */
#include "../runtime/nes.h"
int recomp_enabled = 1;
${Array.from({ length: 8 }, (_, b) => `int bank${b}_run(void);`).join("\n")}
int recomp_run(void) {
  for (;;) {
    if (CPU.jammed || CPU.nmi_pending || (M.irq_line != 0 && (CPU.p & FI) == 0) || M.frame_done) return 0;
    if (CPU.pc < 0x8000) return 0;
    int r;
    switch (M.prg_bank & 7) {
${Array.from({ length: 8 }, (_, b) => `    case ${b}: r = bank${b}_run(); break;`).join("\n")}
    default: return 0;
    }
    if (r == 0) return 0; /* pc has no native block: one interpreter step */
  }
}
`);

console.log(`total: ${totalEmitted} instructions recompiled, ${totalSkipped} left to the interpreter`);
