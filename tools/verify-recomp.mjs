/*
 * verify-recomp.mjs — frame-exact verification of the C recompilation
 * against the JS reference core.
 *
 * Generates a deterministic input tape (boot Start presses → gameplay with
 * walking, punching, jumping), runs the JS core over it emitting per-frame
 * FNV-1a hashes of framebuffer bytes + system RAM, runs the C headless build
 * over the same tape, and diffs the streams. Any divergence prints the first
 * differing frame; success means the two implementations are frame-exact.
 *
 * Usage: node tools/recomp/verify-recomp.mjs [frames] [--exe path]
 */
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { Nes } from "../ref-core/nes.js";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const args = process.argv.slice(2);
const frames = Number(args.find((a) => /^\d+$/.test(a)) ?? "3000");
const exeIdx = args.indexOf("--exe");
const exe = exeIdx >= 0 ? args[exeIdx + 1] : path.join(root, "build", "recomp-headless.exe");
const romPath = path.join(root, "Battletoads Double Dragon (U).nes");

// ── the tape: boot to gameplay, then fight-ish inputs ────────────────────────
const events = [[1, 0]];
for (const f of [700, 950, 1200, 1450, 1650]) {
  events.push([f, 8], [f + 8, 0]);   // Start taps
}
events.push([1750, 0x80]);            // hold RIGHT into the level
for (let f = 2000; f < 2900; f += 90) {
  events.push([f, 0x82], [f + 6, 0x80]);        // B punches while walking
  events.push([f + 40, 0x81], [f + 46, 0x80]);  // A taps (jump)
}
events.push([2900, 0x20], [2960, 0x90], [3020, 0x80]); // depth moves
const tapeDir = path.join(root, "build");
fs.mkdirSync(tapeDir, { recursive: true });
const tapePath = path.join(tapeDir, "verify-tape.txt");
fs.writeFileSync(tapePath, events.map(([f, m]) => `${f} ${m}`).join("\n") + "\n");

// ── JS reference run ─────────────────────────────────────────────────────────
function fnv1a(bytes, h) {
  for (let i = 0; i < bytes.length; i += 1) {
    h ^= bytes[i];
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h >>> 0;
}

console.log(`JS reference: ${frames} frames…`);
const nes = new Nes(fs.readFileSync(romPath), { sampleRate: 0 });
const jsLines = [];
let pad = 0;
let pos = 0;
const t0 = Date.now();
for (let fr = 1; fr <= frames; fr += 1) {
  while (pos < events.length && events[pos][0] <= fr) {
    pad = events[pos][1];
    pos += 1;
  }
  nes.setButtons(0, pad);
  nes.runFrame();
  const fb = nes.ppu.framebuffer;
  const fbBytes = new Uint8Array(fb.buffer, fb.byteOffset, fb.byteLength);
  const fbHash = fnv1a(fbBytes, 2166136261 >>> 0);
  const ramHash = fnv1a(nes.ram, 2166136261 >>> 0);
  jsLines.push(`${fr} ${fbHash.toString(16).padStart(8, "0")} ${ramHash.toString(16).padStart(8, "0")}`);
}
console.log(`JS done in ${((Date.now() - t0) / 1000).toFixed(1)}s`);

// ── C run ────────────────────────────────────────────────────────────────────
console.log(`C build: ${path.relative(root, exe)}…`);
const t1 = Date.now();
const out = execFileSync(exe, [romPath, String(frames), tapePath],
  { maxBuffer: 1 << 26, encoding: "utf8" });
console.log(`C done in ${((Date.now() - t1) / 1000).toFixed(1)}s`);
const cLines = out.trim().split(/\r?\n/);

// ── diff ─────────────────────────────────────────────────────────────────────
if (cLines.length !== jsLines.length) {
  console.log(`LINE COUNT MISMATCH: js ${jsLines.length} vs c ${cLines.length}`);
}
let bad = 0;
for (let i = 0; i < Math.min(jsLines.length, cLines.length); i += 1) {
  if (jsLines[i] !== cLines[i]) {
    if (bad === 0) {
      console.log(`FIRST DIVERGENCE at frame ${i + 1}:`);
      console.log(`  js: ${jsLines[i]}`);
      console.log(`  c : ${cLines[i]}`);
    }
    bad += 1;
  }
}
if (bad === 0) {
  console.log(`VERIFIED: ${frames} frames FRAME-EXACT (framebuffer + RAM hashes identical)`);
} else {
  console.log(`FAILED: ${bad} of ${frames} frames diverge`);
  process.exit(1);
}
