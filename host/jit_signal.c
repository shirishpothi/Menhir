/* Android/Linux crash diagnostics. Never inspect guest pointers in a handler.
   Unrelated faults go directly to the handler installed by ART/bionic. */
#include "jit_signal.h"
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <ucontext.h>
static const int signals[] = {SIGILL, SIGBUS, SIGSEGV};
static struct sigaction previous[3];
static HvMachine *machine;
static void fault(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    uintptr_t pc = uc->uc_mcontext.pc;
    uintptr_t base = (uintptr_t)machine->jit.base;
    if (pc >= base && pc - base < machine->jit.capacity * 4) {
        static const char message[] = "hengevm: host fault in translated code\n";
        (void)write(STDERR_FILENO, message, sizeof message - 1);
    }
    unsigned i = sig == SIGILL ? 0 : sig == SIGBUS ? 1 : 2;
    struct sigaction *old = &previous[i];
    if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        /* Restore disposition and retry the faulting instruction. */
        sigaction(sig, old, NULL);
        return;
    }
    if (old->sa_flags & SA_RESETHAND) {
        struct sigaction def = {.sa_handler = SIG_DFL};
        sigaction(sig, &def, NULL);
    }
    if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(sig, info, context);
    else old->sa_handler(sig);
}
int hv_jit_signals_install(HvMachine *m) {
    if (machine) return -1;
    machine = m;
    for (unsigned i = 0; i < 3; i++) {
        if (sigaction(signals[i], NULL, &previous[i])) goto fail;
        struct sigaction action = previous[i];
        action.sa_flags = (previous[i].sa_flags & (SA_ONSTACK | SA_NODEFER | SA_RESTART)) | SA_SIGINFO;
        action.sa_sigaction = fault;
        if (sigaction(signals[i], &action, NULL)) {
fail:
            while (i) { --i; sigaction(signals[i], &previous[i], NULL); }
            machine = NULL;
            return -1;
        }
    }
    return 0;
}
void hv_jit_signals_remove(void) {
    if (!machine) return;
    for (unsigned i = 0; i < 3; i++) sigaction(signals[i], &previous[i], NULL);
    machine = NULL;
}
