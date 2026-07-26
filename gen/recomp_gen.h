/* generated */
#ifndef RECOMP_GEN_H
#define RECOMP_GEN_H
#include "../runtime/nes.h"
#define RD(a) (CPU.cycles += 1, bus_rd((uint16_t)(a)))
#define WR(a, v) (CPU.cycles += 1, bus_wr((uint16_t)(a), (uint8_t)(v)))
#define IPOLL M.native_instr += 1; if (CPU.nmi_pending || (M.irq_line != 0 && (CPU.p & FI) == 0) || M.frame_done) return 1
#endif
