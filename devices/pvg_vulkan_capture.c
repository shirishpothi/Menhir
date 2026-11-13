/* Records the guest's GPU work for replay (pvg_vulkan_capture.h). Off unless HV_PVG_CAPTURE is set. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#if defined(__aarch64__)
#include <arm_acle.h>
#endif

#include "../core/machine.h"
#include "pvg_lite.h"
#include "pvg_vulkan_capture.h"

#define PAGE 0x4000u
#define MAX_PAGES (1u << 22) /* guest physical addresses below 64 GB */

static struct {
    int checked, enabled;    /* HV_PVG_CAPTURE read; set */
    const char *prefix;
    pthread_mutex_t mu;
    pthread_cond_t done_cv;
    uint64_t *seen;          /* a bit per page: read by the device or its backend */
    uint32_t *touched, ntouched, cap_touched;
    HvMachine *m;
    /* Recording: */
    FILE *f;
    int recording, sessions;
    uint32_t *crc;           /* what each page held when last written, plus one (0: never written) */
    uint8_t (*pending)[PAGE];/* pages read for the first time since the last command, as they were then */
    uint64_t *pending_pa;
    uint32_t npending, cap_pending;
    uint8_t *last_state;
    size_t last_state_len;
    uint32_t done[16];       /* the newest stamp completed on each channel */
    uint32_t seq, swaps, max_swaps;
    uint64_t pages_written, bytes_written;
    double next_gate;
} cap = {.mu = PTHREAD_MUTEX_INITIALIZER, .done_cv = PTHREAD_COND_INITIALIZER};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t page_crc(const uint8_t *p) {
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    uint32_t c = 0xffffffffu;
    const uint64_t *w = (const uint64_t *)p;
    for (uint32_t i = 0; i < PAGE / 8; i += 4)
        c = __crc32cd(c, w[i]), c = __crc32cd(c, w[i + 1]), c = __crc32cd(c, w[i + 2]), c = __crc32cd(c, w[i + 3]);
    return c;
#else
    return (uint32_t)crc32(0, p, PAGE);
#endif
}

static void put(const void *d, size_t n) {
    if (fwrite(d, 1, n, cap.f) != n && cap.recording) {
        fprintf(stderr, "hengevm: pvg-capture: write failed (%s); recording stopped\n", strerror(errno));
        cap.recording = 0;
    }
    cap.bytes_written += n;
}

static void record(uint32_t type, const void *a, size_t an, const void *b, size_t bn) {
    uint32_t h[2] = {type, 0};
    uint64_t len = an + bn;
    put(h, 8), put(&len, 8);
    if (an) put(a, an);
    if (bn) put(b, bn);
}

static int is_zero(const uint8_t *p) {
    const uint64_t *w = (const uint64_t *)p;
    for (uint32_t i = 0; i < PAGE / 8; i++)
        if (w[i]) return 0;
    return 1;
}

static void write_page_as(uint64_t pa, const uint8_t *data, uint32_t flags) {
    static uint8_t out[PAGE + 1024];
    struct __attribute__((packed)) { uint64_t pa; uint32_t stored, flags; } h = {pa, 0, flags};
    const uint8_t *body = NULL;
    if (!is_zero(data)) {
        uLongf n = sizeof out;
        if (compress2(out, &n, data, PAGE, 1) == Z_OK && n < PAGE) h.stored = (uint32_t)n, body = out;
        else h.stored = PAGE, body = data;
    }
    record(HV_PVGCAP_PAGE, &h, sizeof h, body, h.stored);
    cap.crc[pa / PAGE] = page_crc(data) + 1;
    cap.pages_written++;
}

static void write_page(uint64_t pa, const uint8_t *data) { write_page_as(pa, data, 0); }

/* Every noted page that changed since it was written. */
static void flush_changed(uint32_t flags) {
    for (uint32_t i = 0; i < cap.ntouched; i++) {
        uint32_t pn = cap.touched[i];
        const uint8_t *d = hv_phys_ptr(cap.m, (uint64_t)pn * PAGE, PAGE);
        if (!d) continue;
        uint32_t c = page_crc(d) + 1;
        if (c != cap.crc[pn]) write_page_as((uint64_t)pn * PAGE, d, flags);
    }
}

/* Pages read for the first time since the last command, then (changed_too) every noted page that changed. */
static void flush_pages(int changed_too) {
    for (uint32_t i = 0; i < cap.npending; i++) write_page(cap.pending_pa[i], cap.pending[i]);
    cap.npending = 0;
    if (changed_too) flush_changed(0);
}

static void note_page(uint32_t pn) {
    uint64_t bit = 1ull << (pn & 63);
    if (__atomic_load_n(&cap.seen[pn >> 6], __ATOMIC_RELAXED) & bit) return;
    pthread_mutex_lock(&cap.mu);
    if (!(cap.seen[pn >> 6] & bit)) {
        cap.seen[pn >> 6] |= bit;
        if (cap.ntouched == cap.cap_touched) {
            cap.cap_touched = cap.cap_touched ? cap.cap_touched * 2 : 65536;
            cap.touched = realloc(cap.touched, cap.cap_touched * sizeof *cap.touched);
        }
        cap.touched[cap.ntouched++] = pn;
        const uint8_t *d = cap.recording ? hv_phys_ptr(cap.m, (uint64_t)pn * PAGE, PAGE) : NULL;
        if (d) { /* as it is now, before the GPU draws into it */
            if (cap.npending == cap.cap_pending) {
                cap.cap_pending = cap.cap_pending ? cap.cap_pending * 2 : 256;
                cap.pending = realloc(cap.pending, cap.cap_pending * sizeof *cap.pending);
                cap.pending_pa = realloc(cap.pending_pa, cap.cap_pending * sizeof *cap.pending_pa);
            }
            memcpy(cap.pending[cap.npending], d, PAGE);
            cap.pending_pa[cap.npending++] = (uint64_t)pn * PAGE;
        }
    }
    pthread_mutex_unlock(&cap.mu);
}

static int enabled(void) {
    if (!cap.checked) {
        cap.checked = 1;
        cap.prefix = getenv("HV_PVG_CAPTURE");
        if (cap.prefix && *cap.prefix) {
            cap.seen = calloc(MAX_PAGES / 64, 8);
            cap.enabled = cap.seen != NULL;
            const char *n = getenv("HV_PVG_CAPTURE_FRAMES");
            cap.max_swaps = n && atoi(n) > 0 ? (uint32_t)atoi(n) : 30;
            if (cap.enabled) fprintf(stderr, "hengevm: pvg-capture: noting pages; recording while %s.on exists\n", cap.prefix);
        }
    }
    return cap.enabled;
}

void *hv_pvg_capture_phys(HvMachine *m, uint64_t pa, uint64_t len) {
    void *p = hv_phys_ptr(m, pa, len);
    if (p && len && enabled()) {
        cap.m = m;
        for (uint64_t pn = pa / PAGE; pn <= (pa + len - 1) / PAGE && pn < MAX_PAGES; pn++) note_page((uint32_t)pn);
    }
    return p;
}

static void stop_recording(const char *why) {
    if (!cap.f) return;
    record(HV_PVGCAP_END, NULL, 0, NULL, 0);
    fclose(cap.f);
    cap.f = NULL;
    cap.recording = 0;
    fprintf(stderr, "hengevm: pvg-capture: session %d ended (%s): %u commands, %u swaps, %llu pages, %llu MB\n", cap.sessions - 1, why, cap.seq,
            cap.swaps, (unsigned long long)cap.pages_written, (unsigned long long)(cap.bytes_written >> 20));
}

static void start_recording(HvPvgLite *p) {
    char path[1024];
    snprintf(path, sizeof path, "%s-%d.pvgcap", cap.prefix, cap.sessions);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "hengevm: pvg-capture: cannot write %s\n", path); return; }
    if (!cap.crc) cap.crc = calloc(MAX_PAGES, 4);
    memset(cap.crc, 0, MAX_PAGES * 4ull);
    setvbuf(f, NULL, _IOFBF, 8 << 20);
    /* What only the GPU holds goes back to guest memory first, as for instant resume: a replay made from guest
       memory then starts where this backend is. */
    hv_pvg_lite_quiesce(p);
    pthread_mutex_lock(&cap.mu);
    cap.f = f, cap.recording = 1, cap.sessions++;
    cap.seq = cap.swaps = 0, cap.pages_written = cap.bytes_written = 0, cap.npending = 0;
    fwrite(HV_PVGCAP_MAGIC, 1, 8, f);
    uint32_t head[2] = {HV_PVGCAP_VERSION, (uint32_t)cap.m->nram};
    uint64_t ranges[2 * HV_MAX_RAM + 1];
    for (int i = 0; i < cap.m->nram; i++) ranges[2 * i] = cap.m->ram[i].pa, ranges[2 * i + 1] = cap.m->ram[i].size;
    ranges[2 * cap.m->nram] = (uint64_t)cap.m->ncpus; /* one CPU: everything that changes while a command runs is the GPU's */
    record(HV_PVGCAP_HEADER, head, sizeof head, ranges, 16ull * cap.m->nram + 8);
    pthread_mutex_unlock(&cap.mu);
    size_t n = hv_pvg_lite_display_state(p, NULL, 0);
    if (n) {
        uint8_t *d = malloc(n);
        if (d && hv_pvg_lite_display_state(p, d, n) == n) record(HV_PVGCAP_DISPLAY, d, n, NULL, 0);
        free(d);
    }
    pthread_mutex_lock(&cap.mu);
    flush_pages(1); /* everything noted so far */
    pthread_mutex_unlock(&cap.mu);
    fprintf(stderr, "hengevm: pvg-capture: recording %s (%u pages noted)\n", path, cap.ntouched);
}

int hv_pvg_capture_command(HvPvgLite *p, uint32_t ch, const uint32_t *cmd, uint32_t len) {
    (void)cmd, (void)len, (void)ch;
    if (!enabled() || !cap.m) return 0;
    double t = now_s();
    if (t >= cap.next_gate) {
        char gate[1024];
        snprintf(gate, sizeof gate, "%s.on", cap.prefix);
        int on = access(gate, F_OK) == 0;
        cap.next_gate = t + 0.1;
        if (on && !cap.f && cap.sessions < 100) start_recording(p);
        else if (!on && cap.f) {
            pthread_mutex_lock(&cap.mu);
            stop_recording("gate file removed");
            pthread_mutex_unlock(&cap.mu);
        }
    }
    if (!cap.recording) return 0;
    size_t n = hv_pvg_lite_state(p, NULL, 0);
    uint8_t *s = malloc(n);
    hv_pvg_lite_state(p, s, n);
    pthread_mutex_lock(&cap.mu);
    if (n != cap.last_state_len || memcmp(s, cap.last_state, n)) {
        record(HV_PVGCAP_STATE, s, n, NULL, 0);
        free(cap.last_state);
        cap.last_state = s, cap.last_state_len = n;
    } else free(s);
    flush_pages(1);
    pthread_mutex_unlock(&cap.mu);
    return 1;
}

void hv_pvg_capture_complete(uint32_t ch, uint32_t stamp) {
    if (!cap.enabled || ch >= 16) return;
    pthread_mutex_lock(&cap.mu);
    cap.done[ch] = stamp;
    pthread_cond_broadcast(&cap.done_cv);
    pthread_mutex_unlock(&cap.mu);
}

/* The swapped surface as it is in guest memory (descriptor layouts: pvg_metal.m newSurface). */
static void write_frame(HvPvgLite *p, uint64_t mapping) {
    const uint8_t *desc = hv_pvg_lite_surface(p, (uint32_t)mapping);
    if (!desc) return;
    uint32_t list, fourcc, first = 0;
    uint64_t row, w, h;
    memcpy(&list, desc, 4), memcpy(&fourcc, desc + 4, 4);
    if (hv_pvg_lite_old_driver(p)) {
        uint32_t r32, w32, h32;
        memcpy(&r32, desc + 0x1c, 4), memcpy(&w32, desc + 0x14, 4), memcpy(&h32, desc + 0x18, 4);
        row = r32, w = w32 >> 8, h = h32 >> 8;
    } else {
        memcpy(&row, desc + 0x30, 8), memcpy(&w, desc + 0x20, 8), memcpy(&h, desc + 0x28, 8), memcpy(&first, desc + 0x60, 4);
    }
    uint32_t bpp = fourcc == 0x52476841 /* 'RGhA' */ ? 8 : 4;
    if (!w || !h || w > 8192 || h > 8192 || row < w * bpp || !(list & 1)) return;
    uint64_t bytes = first + row * h, npages = (bytes + PAGE - 1) / PAGE;
    const uint32_t *pages = hv_pvg_lite_phys(p, (uint64_t)(list & ~1u) << 12, npages * 4);
    if (!pages) return;
    size_t packed = (size_t)(w * bpp), total = packed * h;
    uint8_t *img = malloc(total);
    for (uint64_t y = 0; y < h; y++)
        for (uint64_t x = 0; x < packed;) {
            uint64_t at = first + y * row + x, in = at % PAGE;
            uint64_t c = PAGE - in < packed - x ? PAGE - in : packed - x;
            const uint8_t *src = (pages[at / PAGE] & 1) ? hv_pvg_lite_phys(p, ((uint64_t)(pages[at / PAGE] & ~1u) << 12) + in, c) : NULL;
            if (src) memcpy(img + y * packed + x, src, c);
            else memset(img + y * packed + x, 0, c);
            x += c;
        }
    uLongf zn = compressBound(total);
    uint8_t *z = malloc(zn);
    if (compress2(z, &zn, img, total, 1) == Z_OK) {
        struct __attribute__((packed)) { uint64_t mapping; uint32_t w, h, fourcc, bpp; } fh = {mapping, (uint32_t)w, (uint32_t)h, fourcc, bpp};
        pthread_mutex_lock(&cap.mu);
        record(HV_PVGCAP_FRAME, &fh, sizeof fh, z, zn);
        pthread_mutex_unlock(&cap.mu);
    }
    free(z), free(img);
}

void hv_pvg_capture_wait(HvPvgLite *p, uint32_t ch, const uint32_t *cmd, uint32_t len) {
    if (!cap.recording || ch >= 16) return;
    uint32_t stamp = cmd[2], op = cmd[0] & 0xffff, skip = (cmd[0] >> 16) * 8;
    pthread_mutex_lock(&cap.mu);
    while ((int32_t)(cap.done[ch] - stamp) < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        if (pthread_cond_timedwait(&cap.done_cv, &cap.mu, &ts) == ETIMEDOUT) {
            fprintf(stderr, "hengevm: pvg-capture: channel %u stamp %x not completed in 5 s (op %x); going on\n", ch, stamp, op);
            break;
        }
    }
    if (!cap.recording) { pthread_mutex_unlock(&cap.mu); return; }
    flush_pages(0); /* what this command read for the first time goes before it */
    uint32_t h[2] = {ch, cap.seq++};
    record(HV_PVGCAP_COMMAND, h, sizeof h, cmd, len);
    /* What changed while it ran: the GPU's work (or, rarely, another CPU's). A replay checks its own against these. */
    flush_changed(HV_PVGCAP_PAGE_WRITTEN);
    pthread_mutex_unlock(&cap.mu);
    if (op == 8 && len >= 12 + skip + 16) {
        const uint32_t *a = (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
        write_frame(p, (uint64_t)a[3] << 32 | a[2]);
        if (++cap.swaps >= cap.max_swaps) {
            pthread_mutex_lock(&cap.mu);
            stop_recording("frame limit");
            pthread_mutex_unlock(&cap.mu);
        }
    }
}

void hv_pvg_capture_unmap(uint32_t mapping) {
    if (!cap.recording) return;
    pthread_mutex_lock(&cap.mu);
    if (cap.recording) record(HV_PVGCAP_UNMAP, &mapping, 4, NULL, 0);
    pthread_mutex_unlock(&cap.mu);
}

void hv_pvg_capture_cursor(void) {
    if (!cap.recording) return;
    pthread_mutex_lock(&cap.mu);
    if (cap.recording) record(HV_PVGCAP_CURSOR, NULL, 0, NULL, 0);
    pthread_mutex_unlock(&cap.mu);
}
