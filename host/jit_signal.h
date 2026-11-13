#ifndef MENHIR_JIT_SIGNAL_H
#define MENHIR_JIT_SIGNAL_H
#include "machine.h"
/* Install once before starting CPUs; remove only after joining every CPU. */
int hv_jit_signals_install(HvMachine *machine);
void hv_jit_signals_remove(void);
#endif
