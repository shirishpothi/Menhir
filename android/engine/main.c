/* Small headless runner. The full app owns restore, networking and audio. */
#include "dispatch.h"
#include "jit_signal.h"
#include "vmapple.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static HvMachine m;
static HvCpu cpu[HV_MAX_CPUS];
static HvVmapple board;
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--installer"))) { fprintf(stderr, "usage: hengevm BOOT_ROM [--installer]\n"); return 2; }
    if (hv_machine_init(&m, 64 << 20)) return 1;
    m.log = 1;
    HvCfgParams cfg = {.nr_cpus = 1};
    if (argc == 3) cfg.run_installer1 = cfg.run_installer2 = 1;
    const uint8_t mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    hv_cpu_reset(cpu, &m);
    if (hv_vmapple_create(&board, &m, cpu, 512ull << 20, NULL, NULL, &cfg, mac)) return 1;
    if (hv_vmapple_load_rom(&board, argv[1])) return 1;
    hv_vmapple_reset_cpu(&board);
    fprintf(stderr, "hengevm: host page size %ld, guest counter 24000000 Hz\n", sysconf(_SC_PAGESIZE));
    if (hv_jit_signals_install(&m)) return 1;
    int result = hv_run(cpu);
    hv_jit_signals_remove();
    fprintf(stderr, "hengevm: exit %d pc %llx\n", result, (unsigned long long)cpu->pc);
    hv_vmapple_destroy(&board);
    hv_machine_free(&m);
    return result == HV_EXIT_HALT ? 0 : 1;
}
