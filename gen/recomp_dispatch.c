/* generated dispatcher */
#include "../runtime/nes.h"
int recomp_enabled = 1;
int bank0_run(void);
int bank1_run(void);
int bank2_run(void);
int bank3_run(void);
int bank4_run(void);
int bank5_run(void);
int bank6_run(void);
int bank7_run(void);
int recomp_run(void) {
  for (;;) {
    if (CPU.jammed || CPU.nmi_pending || (M.irq_line != 0 && (CPU.p & FI) == 0) || M.frame_done) return 0;
    if (CPU.pc < 0x8000) return 0;
    int r;
    switch (M.prg_bank & 7) {
    case 0: r = bank0_run(); break;
    case 1: r = bank1_run(); break;
    case 2: r = bank2_run(); break;
    case 3: r = bank3_run(); break;
    case 4: r = bank4_run(); break;
    case 5: r = bank5_run(); break;
    case 6: r = bank6_run(); break;
    case 7: r = bank7_run(); break;
    default: return 0;
    }
    if (r == 0) return 0; /* pc has no native block: one interpreter step */
  }
}
