/*
 * recomp_stub.c — interpreter-only build: no generated code linked.
 * Stage B builds link recomp/gen/recomp_dispatch.c instead of this file.
 */
#include "nes.h"

int recomp_enabled = 0;
int recomp_run(void) { return 0; }
