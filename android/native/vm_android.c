/* host/vm.c for Android: the same machine, made from the same code, with the Darwin parts replaced.
   Made from host/vm.c by the substitutions in its history; keep the two in step, and fold this back
   into host/vm.c (behind __ANDROID__) when the engine owner makes that file portable.
   - os_unfair_lock is a pthread mutex; the display target is an ANativeWindow, not a retained CALayer;
   - the JIT fault reporter is the engine's host/jit_signal.c (Linux ucontext), not the Mach one;
   - the timers and the serial pointer queue run on small worker threads, without libdispatch;
   - no QoS classes and no USB passthrough probe. */
#include "vm.h"
#include "pvg_lite.h"
#include "jit_signal.h"
#include <android/native_window.h>

#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "net_slirp.h"
#include "sysreg.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS]; /* the board takes them as an array */
static int ncpus = 1;
static HvVmapple board = {.ram_fd = -1};
static _Atomic int input_stopping = 1;
static void stop_pointer(void);
/* UI display changes may arrive before or during the background machine creation. */
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *display_layer;
static int display_layer_set;
static _Atomic(HvPvgLite *) display_pvg; /* published only when the board is ready */
static HvDisk disk = {.fd = -1, .wfd = -1}, aux = {.fd = -1, .wfd = -1};
static int machine_created, signals_installed;
static uint64_t boot_args_offset;
static double boot_last_scan;
static int net_on;
static void (*extra_poll)(void);
static void (*exit_cb)(int);
static pthread_t secondary_threads[HV_MAX_CPUS], kicker;
static int secondary_started[HV_MAX_CPUS], kicker_started;
static _Atomic int kicker_stopped;
static pthread_t primary_thread;
static int primary_started, cpu0_ours, sound_on;
/* Instant resume: a pause stops the CPUs and quiets the devices without taking the machine apart. */
static pthread_mutex_t pause_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv = PTHREAD_COND_INITIALIZER;
static int pause_state; /* 0 running, 1 pausing, 2 paused, 3 it stopped instead */
static int pause_clean; /* paused with nothing in flight: it can be saved */
static int run_ended;   /* CPU 0's thread is on its way out for good: nothing will finish a pause */
static uint64_t paused_counter; /* the guest's virtual counter when its CPUs stopped */
static int64_t paused_wall_ns;  /* and the wall clock then */
static _Atomic int nudge_after_start; /* resumed: tell the guest someone is here as soon as it runs */

HvMachine *hv_vm_machine(void) { return &m; }
HvVmapple *hv_vm_board(void) { return &board; }
HvCpu *hv_vm_cpu(int index) { return index >= 0 && index < ncpus ? &cpus[index] : NULL; }
int hv_vm_ncpus(void) { return ncpus; }

/* arm64 boot_args: {u16 Revision, u16 Version, u64 virtBase, physBase, memSize,
   topOfKernelData, Video {baseAddr, display, rowBytes, width, height, depth}, ...}.
   Found by scanning page starts for one whose physBase is the RAM base. */
int hv_vm_boot_display(HvBootDisplay *d) {
    /* What the guest's GPU has drawn, once it has drawn anything. */
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg) return -1;
    const uint8_t *frame;
    uint32_t fw, fh;
    if (hv_pvg_lite_frame(pvg, &frame, &fw, &fh) == 0) {
        d->pixels = (uint8_t *)frame;
        d->width = fw, d->height = fh, d->stride = fw * 4;
        return 0;
    }
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram) return -1;
    for (int pass = 0; pass < 2; pass++) {
        if (!boot_args_offset || pass) {
            /* A full scan at most every 2 s; boot_args sits low in RAM
               (iBoot puts it near 0x78ed0000), so the first GB is enough. */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = ts.tv_sec + ts.tv_nsec * 1e-9;
            if (now - boot_last_scan < 2) return -1;
            boot_last_scan = now;
            if (pass && boot_args_offset) boot_args_offset = 0;
            uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
            for (uint64_t off = 0; off + 0x1000 <= limit && !boot_args_offset; off += 0x4000) {
                const uint64_t *q = (const uint64_t *)(ram + off);
                const uint16_t *h = (const uint16_t *)(ram + off);
                if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue; /* boot_args: revision, version */
                if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
                uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
                if (w < 320 || w > 8192 || hh < 200 || hh > 8192 || row < w * 4 || (depth & 0xff) != 32) continue;
                if (base < VMAPPLE_MEM || base - VMAPPLE_MEM + row * hh > size) continue;
                boot_args_offset = off;
            }
        }
        if (!boot_args_offset) return -1;
        const uint64_t *q = (const uint64_t *)(ram + boot_args_offset);
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9];
        if (q[1] >> 40 == 0xfffffe && w >= 320 && w <= 8192 && hh >= 200 && hh <= 8192 && base >= VMAPPLE_MEM &&
            base - VMAPPLE_MEM + row * hh <= size) {
            d->pixels = ram + (base - VMAPPLE_MEM);
            d->width = (uint32_t)w, d->height = (uint32_t)hh, d->stride = (uint32_t)row;
            return 0;
        }
    }
    return -1;
}
void hv_vm_set_poll(void (*poll)(void)) { extra_poll = poll; }

const char *hv_vm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

/* Host-side device work on CPU 0's thread each tick. */
static void board_poll(void) {
    if (extra_poll) extra_poll();
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

void (*hv_prof_register)(void);

void hv_vm_set_display_layer(void *layer) {
    ANativeWindow *retained = layer;
    if (retained) ANativeWindow_acquire(retained);
    pthread_mutex_lock(&display_lock);
    ANativeWindow *previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (pvg) hv_pvg_lite_set_layer(pvg, layer);
    pthread_mutex_unlock(&display_lock);
    if (previous) ANativeWindow_release(previous);
}

/* The Android display: frames go to this window. The graphics backend takes it through
   hv_pvg_lite_set_layer, as Metal takes a layer; it must be done with the old window when the call
   returns, and takes its own reference if it keeps the new one. */
void hv_vm_set_display_window(ANativeWindow *window) { hv_vm_set_display_layer(window); }

uint64_t hv_vm_gpu_frames(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_frames(pvg) : 0;
}

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (machine_created) return fail(err, errlen, "The previous machine has not been released.");
    if (primary_started) {
        pthread_join(primary_thread, NULL);
        primary_started = 0;
    }
    if (!c->rom || !c->disk || !c->aux) return fail(err, errlen, "The ROM, disk and auxiliary storage are all needed.");
    machine_created = 1;
    signal(SIGPIPE, SIG_IGN); /* a guest connection closing at the far end must not end the machine */
    if (hv_disk_open(&disk, c->disk, 0, 0) != 0) return fail(err, errlen, "Cannot open the disk image.");
    if (hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, 0) != 0)
        return fail(err, errlen, "Cannot open the auxiliary storage.");
    size_t jit = c->jit_bytes ? c->jit_bytes : (size_t)768 << 20;
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot set up the JIT region.");
    if (!signals_installed) { /* once per process: chains to ART's handlers for faults outside the JIT */
        if (hv_jit_signals_install(&m) != 0) return fail(err, errlen, "Cannot install the JIT fault handlers.");
        signals_installed = 1;
    }
    m.log = c->log;
    m.gen_checks = getenv("HV_JIT_POISON") != NULL;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1; /* debugging: every load and store through the helper */
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK"); /* debugging: one CPU in guest code at a time */
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = c->ecid, .seed = c->seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    if (c->installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    if (getenv("HV_RUN_INSTALLER")) { /* experiment: "a,b" for the two installer words */
        unsigned a = 0, b2 = 0;
        sscanf(getenv("HV_RUN_INSTALLER"), "%u,%u", &a, &b2);
        cfg.run_installer1 = a, cfg.run_installer2 = b2;
    }
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, cpus, (c->ram_mb ? c->ram_mb : 2048) << 20, &disk, &aux, &cfg, mac) != 0)
        return fail(err, errlen, "Cannot build the machine (not enough memory for the guest's RAM?).");
    if (hv_vmapple_load_rom(&board, c->rom) != 0) return fail(err, errlen, "Cannot read the ROM (AVPBooter.vmapple2.bin).");
    hv_vmapple_reset_cpu(&board);
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) != 0) fprintf(stderr, "hengevm: user-mode network unavailable\n");
        else net_on = 1;
    }
    sound_on = board.snd && !c->no_sound;
    if (sound_on) hv_audio_attach(board.snd);
    pause_state = 0;
    board.poll = board_poll;
    // The setter and publication share this lock: an older preboot target can
    // never overwrite a monitor change made while the board was being built.
    pthread_mutex_lock(&display_lock);
    if (display_layer_set && board.pvg) hv_pvg_lite_set_layer(board.pvg, (void *)display_layer);
    atomic_store_explicit(&display_pvg, board.pvg, memory_order_release);
    pthread_mutex_unlock(&display_lock);
    atomic_store(&input_stopping, 0);
    return 0;
}

void hv_vm_destroy(void) {
    if (primary_started && !pthread_equal(primary_thread, pthread_self())) {
        pthread_join(primary_thread, NULL);
        primary_started = 0;
    }
    if (!machine_created) return;
    /* The bridge excludes readers. Unpublish the display before draining
       queued GPU work; completion callbacks still need the GIC and RAM. */
    pthread_mutex_lock(&display_lock);
    atomic_store_explicit(&display_pvg, NULL, memory_order_release);
    pthread_mutex_unlock(&display_lock);
    stop_pointer();
    hv_net_detach();
    net_on = 0;
    hv_audio_detach();
    hv_vmapple_destroy(&board);
    hv_disk_close(&disk);
    hv_disk_close(&aux);
    hv_machine_free(&m);
    memset(cpus, 0, sizeof cpus);
    ncpus = 0;
    boot_args_offset = 0;
    boot_last_scan = 0;
    extra_poll = NULL;
    machine_created = 0;
}

static void *kicker_thread(void *unused) {
    (void)unused;
    while (!atomic_load(&kicker_stopped)) {
        usleep(1000);
        hv_machine_kick_all(&m);
    }
    return NULL;
}

/* A thread that runs guest code. While a block runs its SP register holds the
   guest's SP, a guest address, so the kernel must never push a signal frame
   there: other threads take the asynchronous signals, and a handler that does
   run here (a fault) gets a stack of its own. */
static pthread_key_t stack_key;
static pthread_once_t stack_key_once = PTHREAD_ONCE_INIT;
static void free_cpu_stack(void *memory) {
    stack_t disabled = {.ss_flags = SS_DISABLE};
    sigaltstack(&disabled, NULL);
    free(memory);
}
static void make_stack_key(void) { pthread_key_create(&stack_key, free_cpu_stack); }

void hv_vm_prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    if (hv_prof_register) hv_prof_register();
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) {
        sigaltstack(&ss, NULL);
        pthread_once(&stack_key_once, make_stack_key);
        pthread_setspecific(stack_key, ss.ss_sp);
    }
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGTRAP), sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* Android: the guest's CPU threads keep the default priority. */
void hv_vm_cpu_qos(void) { hv_vm_prepare_cpu_thread(); }

/* CPUs 1 and up: run whenever PSCI CPU_ON starts them; the machine stops with CPU 0. */
static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    hv_vm_cpu_qos();
    int r = hv_run(c);
    if (!c->halted)
        fprintf(stderr, "hengevm: cpu %u stopped (%s) at pc %llx el%u\n", c->index, hv_vm_reason_name(r),
                (unsigned long long)c->pc, c->el);
    return NULL;
}

/* Runs fn on a thread of its own after ms. */
typedef struct { int ms; void (*fn)(void); } After;
static void *after_thread(void *arg) {
    After a = *(After *)arg;
    free(arg);
    usleep((useconds_t)a.ms * 1000);
    a.fn();
    return NULL;
}
static void after_ms(int ms, void (*fn)(void)) {
    After *a = malloc(sizeof *a);
    if (!a) return;
    a->ms = ms, a->fn = fn;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, after_thread, a) != 0) free(a);
    pthread_attr_destroy(&attr);
}

static void link_up(void) {
    if (!machine_created || !board.net) return;
    hv_dev_lock(&m);
    hv_virtio_net_set_link(board.net, 1);
    hv_dev_unlock(&m);
    fprintf(stderr, "hengevm: network link up\n");
}

static void link_down(void) {
    if (!machine_created || !board.net || hv_vm_paused()) return;
    hv_dev_lock(&m);
    hv_virtio_net_set_link(board.net, 0);
    hv_dev_unlock(&m);
    fprintf(stderr, "hengevm: network link down (after a resume)\n");
    after_ms(2000, link_up);
}

/* The network cable out for a moment and back in, as a Mac waking from sleep sees its network come back: macOS
   renews its address and lets connections that died while it was saved go at once. */
void hv_vm_link_flap(void) {
    if (!machine_created || !board.net) return;
    after_ms(1500, link_down);
}

int hv_vm_disk_identity(char *out, size_t n) {
    struct stat a, b;
    if (disk.fd < 0 || aux.fd < 0 || fstat(disk.fd, &a) != 0 || fstat(aux.fd, &b) != 0) return -1;
    snprintf(out, n, "disk %llu %lld %ld.%09ld aux %llu %lld %ld.%09ld", (unsigned long long)a.st_ino, (long long)a.st_size,
             (long)a.st_mtim.tv_sec, (long)a.st_mtim.tv_nsec, (unsigned long long)b.st_ino, (long long)b.st_size,
             (long)b.st_mtim.tv_sec, (long)b.st_mtim.tv_nsec);
    return 0;
}

void hv_vm_flush_disks(void) {
    hv_disk_flush(&disk);
    hv_disk_flush(&aux);
}

int hv_vm_run_cpu0(void) {
    return hv_run(&cpus[0]);
}

int hv_vm_paused(void);
static void set_pause_state(int v) {
    pthread_mutex_lock(&pause_mu);
    pause_state = v;
    pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_mu);
}

/* Every CPU halted to be saved (a CPU stopping the machine itself at the same moment keeps its own reason). */
static int all_saving(void) {
    for (int i = 0; i < ncpus; i++)
        if (atomic_load(&cpus[i].halted) != HV_HALT_SAVE) return 0;
    return 1;
}

int hv_vm_finish_pause(void) {
    atomic_store(&kicker_stopped, 1);
    for (int i = 1; i < ncpus; i++)
        if (secondary_started[i]) pthread_join(secondary_threads[i], NULL);
    if (kicker_started) pthread_join(kicker, NULL);
    kicker_started = 0;
    memset(secondary_started, 0, sizeof secondary_started);
    if (!all_saving()) return -1;
    paused_counter = hv_counter() - m.cntvoff;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    paused_wall_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    hv_virtio_blk_stop(board.root_blk);
    hv_virtio_blk_stop(board.aux_blk);
    if (sound_on) hv_audio_detach(); /* the host's player stops; the stream's state stays in the device */
    hv_pvg_lite_quiesce(board.pvg);
    hv_dev_lock(&m);
    int idle = hv_virtio_blk_idle(board.root_blk) && hv_virtio_blk_idle(board.aux_blk);
    hv_dev_unlock(&m);
    if (!idle) fprintf(stderr, "hengevm: pause: disk requests still in flight; it cannot be saved like this\n");
    pause_clean = idle;
    hv_vm_flush_disks();
    set_pause_state(2);
    return 0;
}

int hv_vm_pause_clean(void) { return hv_vm_paused() && pause_clean; }

/* Where the guest's counter goes on from after a pause or a restore: as much later as the wall clock moved meanwhile, so
   its clock is right at once (macOS then also checks it with the network) -- the counter on a host (an iPhone asleep)
   stops for longer than the wall clock does. A jump of 10 minutes was seen to do no harm beyond what a Mac left alone
   that long does (its display goes off for being idle: hv_vm_start nudges the pointer after it). HV_RESUME_CLOCK=
   continuous: no time passes for the guest at all (its clock is then behind until macOS sets it, which can be hours). */
uint64_t hv_vm_resume_counter(uint64_t counter, int64_t wall_ns) {
    const char *how = getenv("HV_RESUME_CLOCK");
    if (how && !strcmp(how, "continuous")) return counter;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t gone = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec - wall_ns;
    if (getenv("HV_RESUME_SKEW")) gone += (int64_t)atoll(getenv("HV_RESUME_SKEW")) * 1000000000; /* testing: as if away longer */
    atomic_store(&nudge_after_start, 1);
    return gone > 0 ? counter + (uint64_t)gone / 125 * 3 : counter; /* 24 MHz: 3 ticks every 125 ns */
}
uint64_t hv_vm_paused_counter(void) { return paused_counter; }
int64_t hv_vm_paused_wall_ns(void) { return paused_wall_ns; }

int hv_vm_pause(void) {
    pthread_mutex_lock(&pause_mu);
    if (pause_state == 2) {
        pthread_mutex_unlock(&pause_mu);
        return 0;
    }
    if (run_ended) {
        pthread_mutex_unlock(&pause_mu);
        return -1;
    }
    pause_state = 1;
    pthread_mutex_unlock(&pause_mu);
    atomic_store(&input_stopping, 1);
    hv_machine_halt(&m, HV_HALT_SAVE);
    pthread_mutex_lock(&pause_mu);
    while (pause_state == 1) pthread_cond_wait(&pause_cv, &pause_mu);
    int r = pause_state == 2 ? 0 : -1;
    pthread_mutex_unlock(&pause_mu);
    return r;
}

int hv_vm_paused(void) {
    pthread_mutex_lock(&pause_mu);
    int r = pause_state == 2;
    pthread_mutex_unlock(&pause_mu);
    return r;
}

int hv_vm_continue(void) {
    pthread_mutex_lock(&pause_mu);
    int paused = pause_state == 2;
    pthread_mutex_unlock(&pause_mu);
    if (!paused) return -1;
    if (cpu0_ours && primary_started && !pthread_equal(primary_thread, pthread_self())) {
        pthread_join(primary_thread, NULL);
        primary_started = 0;
    }
    m.cntvoff = hv_counter() - hv_vm_resume_counter(paused_counter, paused_wall_ns);
    for (int i = 0; i < ncpus; i++) {
        cpus[i].cntvoff = m.cntvoff;
        atomic_store(&cpus[i].halted, HV_HALT_NONE);
    }
    hv_virtio_blk_start(board.root_blk);
    hv_virtio_blk_start(board.aux_blk);
    if (sound_on) {
        hv_audio_attach(board.snd);
        hv_virtio_snd_resume(board.snd);
    }
    atomic_store(&input_stopping, 0);
    set_pause_state(0);
    return hv_vm_start(cpu0_ours, exit_cb);
}

static void nudge(void) {
    hv_vm_mouse_move(1, 0);
    hv_vm_mouse_move(-1, 0);
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    hv_vm_cpu_qos();
    int r = hv_vm_run_cpu0();
    if (atomic_load(&cpus[0].halted) == HV_HALT_SAVE && hv_vm_finish_pause() == 0) return NULL;
    pthread_mutex_lock(&pause_mu);
    run_ended = 1;
    if (pause_state == 1) pause_state = 3, pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_mu);
    /* Finish CPU activity before flushing or telling the UI the Mac stopped.
       CPU 0 can also leave for a fatal execution error. */
    hv_machine_halt(&m, HV_HALT_HOST);
    atomic_store(&kicker_stopped, 1);
    for (int i = 1; i < ncpus; i++)
        if (secondary_started[i]) pthread_join(secondary_threads[i], NULL);
    if (kicker_started) pthread_join(kicker, NULL);
    kicker_started = 0;
    memset(secondary_started, 0, sizeof secondary_started);
    /* A DMA worker can still be writing after the guest CPUs have exited. */
    hv_virtio_blk_stop(board.root_blk);
    hv_virtio_blk_stop(board.aux_blk);
    hv_audio_detach();
    hv_vm_flush_disks();
    if (exit_cb) exit_cb(r);
    return NULL;
}

int hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    cpu0_ours = cpu0;
    if (atomic_exchange(&nudge_after_start, 0)) /* someone is back: macOS counts the time away as idle (display off) */
        after_ms(200, nudge);
    pthread_mutex_lock(&pause_mu);
    run_ended = 0;
    pthread_mutex_unlock(&pause_mu);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    atomic_store(&kicker_stopped, 0);
    /* Publish all join handles before CPU 0 can reach its exit callback. */
    for (int i = 1; i < ncpus; i++) {
        secondary_started[i] = pthread_create(&secondary_threads[i], &attr, secondary_thread, &cpus[i]) == 0;
        if (!secondary_started[i]) goto failed;
    }
    kicker_started = pthread_create(&kicker, NULL, kicker_thread, NULL) == 0;
    if (!kicker_started) goto failed;
    if (cpu0) {
        primary_started = pthread_create(&primary_thread, &attr, cpu0_thread, NULL) == 0;
        if (!primary_started) goto failed;
    }
    pthread_attr_destroy(&attr);
    return 0;
failed:
    pthread_attr_destroy(&attr);
    hv_machine_halt(&m, HV_HALT_HOST);
    atomic_store(&kicker_stopped, 1);
    for (int i = 1; i < ncpus; i++)
        if (secondary_started[i]) pthread_join(secondary_threads[i], NULL);
    memset(secondary_started, 0, sizeof secondary_started);
    if (kicker_started) pthread_join(kicker, NULL);
    kicker_started = 0;
    return -1;
}

void hv_vm_stop(void) {
    hv_machine_halt(&m, HV_HALT_HOST);
}

/* macOS's virtio mouse driver drops BTN_RIGHT, so the right button is a Control-click, with the right
   Control. The USB keyboard is read every 10 ms and the click goes over virtio, so Control goes down
   50 ms before the click and comes up 50 ms after it. The waits run on a serial queue that the pointer's
   moves and buttons all go through, so they stay in order and the caller (often the main thread) never
   waits. The guest's right Control and left button each have two owners, and stay down while either holds them. */
#define HID_RIGHT_CONTROL 0xe4
#define LINUX_KEY_RIGHTCTRL 97
static pthread_mutex_t rctrl_lock = PTHREAD_MUTEX_INITIALIZER;
static int rctrl_user, rctrl_click;
static int left_user, left_click, left_sent; /* only the pointer queue touches these */

/* A serial queue on one worker thread: pointer moves and buttons run in order, and the 50 ms waits
   of a right click never hold up the caller. */
#define POINTER_QUEUE 1024
static struct { void (*fn)(void *); void *ctx; } pq[POINTER_QUEUE];
static unsigned pq_head, pq_tail; /* run up to head, queued up to tail */
static pthread_mutex_t pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t pq_once = PTHREAD_ONCE_INIT;
static unsigned pq_finished; /* items finished, counted as head is */

static void *pointer_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&pq_mu);
    for (;;) {
        while (pq_head == pq_tail) pthread_cond_wait(&pq_cv, &pq_mu);
        void (*fn)(void *) = pq[pq_head % POINTER_QUEUE].fn;
        void *ctx = pq[pq_head % POINTER_QUEUE].ctx;
        pq_head++;
        pthread_mutex_unlock(&pq_mu);
        if (fn) fn(ctx);
        pthread_mutex_lock(&pq_mu);
        pq_finished++;
        pthread_cond_broadcast(&pq_cv);
    }
    return NULL;
}

static void pointer_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, pointer_worker, NULL) == 0) pthread_detach(t);
}

/* Queues fn and returns the count of items that will have finished when it has. A full queue drops
   the oldest input not yet run, rather than make the caller (the UI thread) wait. */
static unsigned pointer_async(void (*fn)(void *), void *ctx) {
    pthread_once(&pq_once, pointer_start);
    pthread_mutex_lock(&pq_mu);
    if (pq_tail - pq_head == POINTER_QUEUE) {
        pq_head++;
        pq_finished++;
    }
    pq[pq_tail % POINTER_QUEUE].fn = fn;
    pq[pq_tail % POINTER_QUEUE].ctx = ctx;
    pq_tail++;
    unsigned until = pq_finished + (pq_tail - pq_head);
    pthread_cond_broadcast(&pq_cv);
    pthread_mutex_unlock(&pq_mu);
    return until;
}

static void pointer_sync(void (*fn)(void *), void *ctx) {
    unsigned until = pointer_async(fn, ctx);
    pthread_mutex_lock(&pq_mu);
    while ((int)(pq_finished - until) < 0) pthread_cond_wait(&pq_cv, &pq_mu);
    pthread_mutex_unlock(&pq_mu);
}

static void pointer_reset(void *unused) {
    (void)unused;
    left_user = left_click = left_sent = 0;
    pthread_mutex_lock(&rctrl_lock);
    rctrl_user = rctrl_click = 0;
    pthread_mutex_unlock(&rctrl_lock);
}

static void stop_pointer(void) {
    atomic_store(&input_stopping, 1);
    /* The bridge excludes new input. Finish a click already in progress and
       discard queued input before releasing the keyboard and mouse. */
    pointer_sync(pointer_reset, NULL);
}

static void rctrl_hold(int *owner, int down) {
    pthread_mutex_lock(&rctrl_lock);
    *owner = down;
    hv_xhci_key(board.usb, HID_RIGHT_CONTROL, rctrl_user || rctrl_click);
    pthread_mutex_unlock(&rctrl_lock);
}

void hv_vm_key(uint16_t code, int down) {
    if (code == LINUX_KEY_RIGHTCTRL) rctrl_hold(&rctrl_user, down);
    else hv_xhci_key_linux(board.usb, code, down);
}

void hv_vm_key_hid(uint8_t usage, int down) {
    if (usage == HID_RIGHT_CONTROL) rctrl_hold(&rctrl_user, down);
    else hv_xhci_key(board.usb, usage, down);
}

static void move_now(void *ctx) {
    if (atomic_load(&input_stopping)) return;
    int64_t packed = (int64_t)(intptr_t)ctx;
    int32_t dx = (int32_t)(packed >> 32), dy = (int32_t)packed;
    if (dx) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, dx);
    if (dy) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_move(int dx, int dy) {
    int64_t packed = (int64_t)((uint64_t)(uint32_t)dx << 32 | (uint32_t)dy);
    pointer_async(move_now, (void *)(intptr_t)packed);
}

static void left_update(void) {
    int down = left_user || left_click;
    if (down == left_sent) return;
    left_sent = down;
    hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_LEFT, down);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

static void button_now(void *ctx) {
    if (atomic_load(&input_stopping)) return;
    intptr_t v = (intptr_t)ctx;
    int button = (int)(v >> 1), down = (int)(v & 1);
    if (button == 0) {
        left_user = down;
        left_update();
    } else if (button == 1 && down != left_click) {
        if (down) {
            rctrl_hold(&rctrl_click, 1);
            usleep(50000);
        }
        left_click = down;
        left_update();
        if (!down) {
            usleep(50000);
            rctrl_hold(&rctrl_click, 0);
        }
    } else if (button == 2) {
        hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_MIDDLE, down);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
    }
}

void hv_vm_mouse_button(int button, int down) {
    if (button < 0 || button > 2) return;
    pointer_async(button_now, (void *)(intptr_t)(button << 1 | !!down));
}

/* The guest takes a wheel event as 8 bits: -200 arrives as +56, the wrong way. So big moves go in pieces. */
static void wheel(uint16_t axis, int amount) {
    while (amount) {
        int piece = amount > 100 ? 100 : amount < -100 ? -100 : amount;
        hv_virtio_input_send(board.mouse, HV_EV_REL, axis, piece);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        amount -= piece;
    }
}

void hv_vm_mouse_wheel(int dy) { wheel(HV_REL_WHEEL, dy); }
void hv_vm_mouse_hwheel(int dx) { wheel(HV_REL_HWHEEL, dx); }

void hv_vm_touch(int id, int phase, int x, int y, uint32_t ts_ms) {
    if (x < 0) x = 0;
    if (x > HV_TP_SENSOR_X) x = HV_TP_SENSOR_X;
    if (y < 0) y = 0;
    if (y > HV_TP_SENSOR_Y) y = HV_TP_SENSOR_Y;
    hv_xhci_touch(board.usb, id, phase, x, y, ts_ms);
}

void hv_vm_touch_frame(uint32_t ts_ms) { hv_xhci_touch_frame(board.usb, ts_ms); }

void hv_vm_touch_button(int down) { hv_xhci_touch_button(board.usb, down); }

int hv_vm_pointer(int *x, int *y) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    const uint8_t *page = pvg ? hv_pvg_lite_display_page(pvg) : NULL;
    if (!page) return -1;
    uint32_t at = *(const volatile uint32_t *)(page + 0xe00);
    if (at == 0xffffffff) return -1;
    *x = (int)(at & 0xffff), *y = (int)(at >> 16);
    return 0;
}

int hv_vm_snapshot(uint8_t *bgra, int width, int height) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg || width <= 0 || height <= 0) return -1;
    return hv_pvg_lite_snapshot(pvg, bgra, (uint32_t)width, (uint32_t)height);
}
