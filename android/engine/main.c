/* Headless Android machine. CPU workers block asynchronous signals while in JIT code. */
#include "dispatch.h"
#include "jit_signal.h"
#include "vmapple.h"
#include "net_slirp.h"
#include "audio_mac.h"
#include "install.h"
#include "usb_host.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static HvMachine m;
static HvCpu cpu[HV_MAX_CPUS];
static HvVmapple board;
static _Atomic int stopping;
static int net_on, ncpus = 1;
static void stop(int sig) {
    (void)sig;
    for (int i = 0; i < ncpus; i++) atomic_store(&cpu[i].halted, HV_HALT_HOST);
    hv_machine_kick_all(&m);
}
static void poll_board(void) {
#ifdef MENHIR_WITH_NET
    if (net_on) hv_net_poll();
#endif
}
static void *kick(void *unused) {
    (void)unused;
    while (!atomic_load(&stopping)) { usleep(1000); hv_machine_kick_all(&m); }
    return NULL;
}
static void *run_cpu(void *arg) {
    HvCpu *c = arg;
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536};
    if (!ss.ss_sp || sigaltstack(&ss, NULL)) abort();
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGILL); sigdelset(&set, SIGBUS); sigdelset(&set, SIGSEGV);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    int r = hv_run(c);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9;
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    fprintf(stderr, "hengevm: cpu %u elapsed %.3f s %.0f blocks/s peak RSS %ld KiB\n",
            c->index, elapsed, c->icount / elapsed, usage.ru_maxrss);
    fprintf(stderr, "hengevm: cpu %u exit %d blocks %llu pc %llx esr %llx far %llx elr %llx\n",
            c->index, r, (unsigned long long)c->icount, (unsigned long long)c->pc,
            (unsigned long long)c->esr_el1, (unsigned long long)c->far_el1, (unsigned long long)c->elr_el1);
    ss.ss_flags = SS_DISABLE; sigaltstack(&ss, NULL); free(ss.ss_sp);
    if (!c->index) stop(0);
    return (void *)(intptr_t)r;
}
static void progress(int step, double p, void *user) {
    (void)user;
    fprintf(stderr, "hengevm: install step %d %.1f%%\n", step, p * 100);
}
static int usage(void) {
    fprintf(stderr, "usage: hengevm --rom FILE [--disk FILE --aux FILE --aux-offset N] [--ecid N] [--seed N] [--ram MB] [--cpus N] [--log N] [--trace N] [--stats SECONDS] [--no-chain] [--installer | --install IPSW]\n");
    return 2;
}
int main(int argc, char **argv) {
    const char *rom = NULL, *disk_path = NULL, *aux_path = NULL, *ipsw = NULL;
    uint64_t ram_mb = 4096, ecid = 0, seed = 0, aux_offset = 16384;
    int installer = 0, log = 1, trace = 0, stats = 0, chain = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--installer")) { installer = 1; continue; }
        if (!strcmp(a, "--no-chain")) { chain = 0; continue; }
        if (!strcmp(a, "--no-pvg")) continue;
        if (a[0] != '-' && !rom) { rom = a; continue; }
        if (++i == argc) return usage();
        if (!strcmp(a, "--rom")) rom = argv[i];
        else if (!strcmp(a, "--disk")) disk_path = argv[i];
        else if (!strcmp(a, "--aux")) aux_path = argv[i];
        else if (!strcmp(a, "--install")) { ipsw = argv[i]; installer = 1; }
        else if (!strcmp(a, "--ecid")) ecid = strtoull(argv[i], NULL, 0);
        else if (!strcmp(a, "--seed")) seed = strtoull(argv[i], NULL, 0);
        else if (!strcmp(a, "--aux-offset")) aux_offset = strtoull(argv[i], NULL, 0);
        else if (!strcmp(a, "--ram")) ram_mb = strtoull(argv[i], NULL, 0);
        else if (!strcmp(a, "--cpus")) ncpus = atoi(argv[i]);
        else if (!strcmp(a, "--log")) log = atoi(argv[i]);
        else if (!strcmp(a, "--trace")) trace = atoi(argv[i]);
        else if (!strcmp(a, "--stats")) stats = atoi(argv[i]);
        else return usage();
    }
    if (!rom || ram_mb < 128 || ram_mb > 8192 || ncpus < 1 || ncpus > HV_MAX_CPUS ||
        (!!disk_path != !!aux_path) || (ipsw && (!disk_path || !ecid))) return usage();
#ifndef MENHIR_WITH_RESTORE
    if (ipsw) return usage();
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    HvDisk disk = {.fd = -1, .wfd = -1}, aux = {.fd = -1, .wfd = -1};
    if (disk_path && (hv_disk_open(&disk, disk_path, 0, 0) || hv_disk_open(&aux, aux_path, aux_offset, 0))) {
        perror("hengevm: disk"); return 1;
    }
    if (hv_machine_init(&m, 768u << 20)) return 1;
    m.log = log; m.trace = trace; m.stats_interval = stats; m.chain = chain;
    m.slow_mem = getenv("HV_SLOW_MEM") != NULL;
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = ecid, .seed = seed,
                      .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0, 1}};
    if (installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    if (hv_vmapple_create(&board, &m, cpu, ram_mb << 20, disk_path ? &disk : NULL,
                          aux_path ? &aux : NULL, &cfg, cfg.mac_en0)) return 1;
    if (hv_vmapple_load_rom(&board, rom)) return 1;
    hv_vmapple_reset_cpu(&board);
#ifdef MENHIR_WITH_NET
    if (!getenv("HV_NO_NET") && board.net) {
        if (hv_net_attach(&m, board.net)) return 1;
        net_on = 1;
    }
#endif
#ifdef MENHIR_WITH_AUDIO
    if (!getenv("HV_NO_SOUND") && board.snd) hv_audio_attach(board.snd);
#endif
    board.poll = poll_board;
    fprintf(stderr, "hengevm: host pages %ld RAM %llu MB CPUs %d\n", sysconf(_SC_PAGESIZE), (unsigned long long)ram_mb, ncpus);
    if (hv_jit_signals_install(&m)) return 1;
    signal(SIGPIPE, SIG_IGN);
#ifdef MENHIR_WITH_RESTORE
    if (ipsw) {
        char work[1024];
        if (snprintf(work, sizeof work, "%s.restore", disk_path) >= (int)sizeof work) return 1;
        if (mkdir(work, 0700) && access(work, W_OK)) return 1;
        if (hv_install_start(board.vusb, ipsw, work, ecid, progress, NULL)) return 1;
    }
#endif
    signal(SIGINT, stop); signal(SIGTERM, stop);
    pthread_t workers[HV_MAX_CPUS], kicker;
    pthread_attr_t attr;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&kicker, NULL, kick, NULL);
    for (int i = 0; i < ncpus; i++) if (pthread_create(&workers[i], &attr, run_cpu, &cpu[i])) abort();
    pthread_attr_destroy(&attr);
    if (!ipsw && getenv("HV_VUSB_PROBE")) {
        fprintf(stderr, "usb probe: session %lld\n", (long long)hv_usb_wait_online(board.vusb, 10000));
        fprintf(stderr, "usb probe: reset %d\n", hv_usb_reset(board.vusb, 2000));
        uint8_t dd[18] = {0};
        int n = hv_usb_control(board.vusb, 0x80, 6, 0x100, 0, dd, sizeof dd, 2000);
        fprintf(stderr, "usb probe: descriptor %d:", n);
        for (int i = 0; i < n; i++) fprintf(stderr, " %02x", dd[i]);
        fprintf(stderr, "\n");
        stop(0);
    }
    void *result;
    pthread_join(workers[0], &result);
    for (int i = 1; i < ncpus; i++) pthread_join(workers[i], NULL);
    atomic_store(&stopping, 1); pthread_join(kicker, NULL);
    /* Restore owns detached workers with pointers into the board. Exit only after
       flushing disks; process exit reaps those workers before memory is reused. */
    hv_virtio_blk_stop(board.root_blk);
    hv_virtio_blk_stop(board.aux_blk);
    if (disk_path && (hv_disk_flush(&disk) || hv_disk_flush(&aux))) { perror("disk flush"); return 1; }
#ifdef MENHIR_WITH_RESTORE
    if (ipsw) return hv_install_result() == 0 ? 0 : 1;
#endif
#ifdef MENHIR_WITH_AUDIO
    hv_audio_detach();
#endif
#ifdef MENHIR_WITH_NET
    if (net_on) hv_net_detach();
#endif
    hv_jit_signals_remove(); hv_vmapple_destroy(&board); hv_machine_free(&m);
    hv_disk_close(&disk); hv_disk_close(&aux);
    return (intptr_t)result == HV_EXIT_HALT ? 0 : 1;
}
