#include "platform.h"
#include "jit_signal.h"
#include "dispatch.h"
#include <signal.h>
#include <stdlib.h>
static volatile sig_atomic_t chained;
static HvCpu cpu;
static void prior(int sig) { (void)sig; chained++; }
int main(void) {
    uint64_t a = hv_host_counter();
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
    uint64_t dt = hv_host_counter() - a;
    if (dt < 240000 || dt > 24000000) return 1;
    static HvMachine m;
    struct sigaction old, action = {.sa_handler = prior};
    sigemptyset(&action.sa_mask);
    sigaction(SIGILL, &action, &old);
    if (hv_jit_signals_install(&m)) return 1;
    raise(SIGILL);
    hv_jit_signals_remove();
    sigaction(SIGILL, &old, NULL);
    if (chained != 1) return 1;
    if (hv_machine_init(&m, 1 << 20)) return 1;
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code) return 1;
    /* mov x18,#85; msr tpidr_el0,x18; mrs x1,tpidr_el0;
       hvc #0 (PSCI SYSTEM_OFF supplied in x0). */
    code[0] = 0xd2800ab2;
    code[1] = 0xd51bd052;
    code[2] = 0xd53bd041;
    code[3] = 0xd4000002;
    hv_machine_add_ram(&m, 0x10000, 16384, code, 1);
    hv_cpu_reset(&cpu, &m);
    cpu.pc = 0x10000;
    cpu.x[0] = 0x84000008;
    uint64_t tp_before, tp_after, x18_before, x18_after;
    __asm__ volatile("mrs %0, tpidr_el0; mov %1, x18" : "=r"(tp_before), "=r"(x18_before));
    int reason = hv_run(&cpu);
    __asm__ volatile("mrs %0, tpidr_el0; mov %1, x18" : "=r"(tp_after), "=r"(x18_after));
    if (reason != HV_EXIT_HALT || cpu.x[1] != 85 || cpu.x[18] != 85 ||
        tp_before != tp_after || x18_before != x18_after) return 1;
    hv_machine_free(&m);
    free(code);
    puts("24 MHz counter, prior signal handler, host TPIDR_EL0/x18: passed");
    return 0;
}
