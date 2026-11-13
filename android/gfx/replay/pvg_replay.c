/* Replays a recording of the guest's GPU work (devices/pvg_vulkan_capture.h) through a graphics backend, off the
   device, and compares what the backend draws with what the recorded backend drew.

     pvg_replay [--free] [--out DIR] [--quiet] CAPTURE.pvgcap

   The backend is whichever one the tool is linked with: pvg_metal.m (pvg_replay_metal, to check the rig itself) or
   pvg_vulkan.c (pvg_replay_vulkan). The machine is a stand-in: guest memory is filled from the recording's pages,
   commands go to the backend one at a time, each waited for, as they were recorded.

   Pages the GPU wrote while a command ran are recorded with HV_PVGCAP_PAGE_WRITTEN. By default they are applied,
   so every command starts from the recorded backend's results ("refresh"). With --free they are only compared
   with what this backend wrote, and the replay draws on its own results: the per-command page differences then say
   which command drew differently. At each swap the shown surface is compared with the recorded one; --out writes
   both and their difference as PNGs. Exit status 0 if every frame matched within the tolerance (HV_REPLAY_TOL,
   default 2 of 255, and 99.9% of pixels). */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../../../core/machine.h"
#include "../../../devices/pvg_lite.h"
#include "../../../devices/pvg_vulkan_capture.h"

#define PAGE 0x4000u

/* ---- The stand-in machine pvg_lite.c runs on ---- */

static HvMachine *machine;

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (pa >= r->pa && pa - r->pa < r->size && len <= r->size - (pa - r->pa)) return r->host + (pa - r->pa);
    }
    return NULL;
}
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque, uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)m, (void)pa, (void)size, (void)opaque, (void)read, (void)write, (void)name;
    return 0;
}
void hv_gicv3_pend(HvGic *g, int intid) { (void)g, (void)intid; }
void hv_machine_wake(HvMachine *m) { (void)m; }

/* ---- Reading the recording ---- */

typedef struct {
    FILE *f;
    uint32_t type;
    uint64_t len;
    uint8_t *data;
    size_t cap;
} Reader;

static int next_record(Reader *r) {
    uint32_t h[2];
    if (fread(h, 4, 2, r->f) != 2 || fread(&r->len, 8, 1, r->f) != 1) return 0;
    r->type = h[0];
    if (r->len > (1ull << 32)) return 0;
    if (r->len > r->cap) {
        r->cap = r->len;
        r->data = realloc(r->data, r->cap);
    }
    return r->len == 0 || fread(r->data, 1, r->len, r->f) == r->len;
}

static int unpack_page(const uint8_t *rec, uint64_t len, uint64_t *pa, uint32_t *flags, uint8_t *out) {
    uint32_t stored;
    if (len < 16) return -1;
    memcpy(pa, rec, 8), memcpy(&stored, rec + 8, 4), memcpy(flags, rec + 12, 4);
    if (16 + (uint64_t)stored > len) return -1;
    if (!stored) memset(out, 0, PAGE);
    else if (stored == PAGE) memcpy(out, rec + 16, PAGE);
    else {
        uLongf n = PAGE;
        if (uncompress(out, &n, rec + 16, stored) != Z_OK || n != PAGE) return -1;
    }
    return 0;
}

/* ---- Pictures ---- */

static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4);
    if (n) c = crc32(c, d, n);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(cb, 1, 4, f);
}

/* rgb: width * height * 3 bytes. */
static void write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) raw[(size_t)y * (w * 3 + 1)] = 0, memcpy(raw + (size_t)y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw), free(z);
}

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (!e) {
        if (!m) bits = s;
        else {
            e = 113;
            while (!(m & 1024)) m <<= 1, e--;
            bits = s | e << 23 | (m & 1023) << 13;
        }
    } else if (e == 31) bits = s | 0x7f800000u | m << 13;
    else bits = s | (e + 112) << 23 | m << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* A surface's pixels as the display shows them: 8-bit RGB. Half-float surfaces hold display-encoded values
   (docs/agents/colorprobe.md), so they are clamped, not encoded. */
static uint8_t *to_rgb(const uint8_t *px, uint32_t w, uint32_t h, uint32_t fourcc, uint32_t bpp) {
    uint8_t *out = malloc((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (bpp == 8 && fourcc == 0x52476841) {
            const uint16_t *s = (const uint16_t *)(px + i * 8);
            for (int c = 0; c < 3; c++) {
                float v = half_to_float(s[c]);
                v = v > 0 ? (v < 1 ? v : 1) : 0;
                out[i * 3 + c] = (uint8_t)(v * 255 + 0.5f);
            }
        } else { /* BGRA */
            const uint8_t *s = px + i * bpp;
            out[i * 3] = s[2], out[i * 3 + 1] = s[1], out[i * 3 + 2] = s[0];
        }
    }
    return out;
}

/* The surface as it is in (stand-in) guest memory, packed rows; layouts as pvg_vulkan_capture.c write_frame. */
static uint8_t *surface_pixels(HvPvgLite *p, uint64_t mapping, uint32_t *w_out, uint32_t *h_out, uint32_t *fourcc_out, uint32_t *bpp_out) {
    const uint8_t *desc = hv_pvg_lite_surface(p, (uint32_t)mapping);
    if (!desc) return NULL;
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
    uint32_t bpp = fourcc == 0x52476841 ? 8 : 4;
    if (!w || !h || w > 8192 || h > 8192 || row < w * bpp || !(list & 1)) return NULL;
    uint64_t npages = (first + row * h + PAGE - 1) / PAGE;
    const uint32_t *pages = hv_pvg_lite_phys(p, (uint64_t)(list & ~1u) << 12, npages * 4);
    if (!pages) return NULL;
    size_t packed = (size_t)(w * bpp);
    uint8_t *img = calloc(packed, h);
    for (uint64_t y = 0; y < h; y++)
        for (uint64_t x = 0; x < packed;) {
            uint64_t at = first + y * row + x, in = at % PAGE;
            uint64_t c = PAGE - in < packed - x ? PAGE - in : packed - x;
            const uint8_t *src = (pages[at / PAGE] & 1) ? hv_pvg_lite_phys(p, ((uint64_t)(pages[at / PAGE] & ~1u) << 12) + in, c) : NULL;
            if (src) memcpy(img + y * packed + x, src, c);
            x += c;
        }
    *w_out = (uint32_t)w, *h_out = (uint32_t)h, *fourcc_out = fourcc, *bpp_out = bpp;
    return img;
}

/* ---- The replay ---- */

static int quiet;
static const char *out_dir;
static int tolerance = 2;

typedef struct {
    uint64_t frames, frames_ok, commands, pages, written_pages, written_differ, timeouts;
} Stats;

static int compare_frame(HvPvgLite *p, const uint8_t *rec, uint64_t len, Stats *st) {
    struct __attribute__((packed)) { uint64_t mapping; uint32_t w, h, fourcc, bpp; } fh;
    if (len < sizeof fh) return -1;
    memcpy(&fh, rec, sizeof fh);
    size_t n = (size_t)fh.w * fh.h * fh.bpp;
    uint8_t *want = malloc(n);
    uLongf got_n = n;
    if (uncompress(want, &got_n, rec + sizeof fh, (uLong)(len - sizeof fh)) != Z_OK || got_n != n) {
        free(want);
        return -1;
    }
    uint32_t w, h, fourcc, bpp;
    uint8_t *have = surface_pixels(p, fh.mapping, &w, &h, &fourcc, &bpp);
    uint8_t *a = to_rgb(want, fh.w, fh.h, fh.fourcc, fh.bpp);
    st->frames++;
    int ok = 0;
    if (!have || w != fh.w || h != fh.h) {
        printf("frame %llu: surface %llu: recorded %ux%u, replay has %s\n", (unsigned long long)st->frames, (unsigned long long)fh.mapping, fh.w, fh.h,
               have ? "another size" : "nothing");
    } else {
        uint8_t *b = to_rgb(have, w, h, fourcc, bpp);
        uint64_t differ = 0;
        double se = 0;
        int worst = 0;
        uint8_t *diff = malloc((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            int m = 0;
            for (int c = 0; c < 3; c++) {
                int d = abs((int)a[i * 3 + c] - (int)b[i * 3 + c]);
                se += (double)d * d;
                if (d > m) m = d;
            }
            if (m > tolerance) differ++;
            if (m > worst) worst = m;
            uint8_t v = m > tolerance ? 255 : (uint8_t)(m * 40);
            diff[i * 3] = v, diff[i * 3 + 1] = m > tolerance ? 0 : v, diff[i * 3 + 2] = m > tolerance ? 0 : v;
        }
        double mse = se / ((double)w * h * 3), psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
        double share = (double)differ / ((double)w * h);
        ok = share <= 0.001;
        st->frames_ok += ok;
        printf("frame %llu: surface %llu %ux%u %c%c%c%c: %s, %.4f%% of pixels differ by more than %d, worst %d, PSNR %.1f dB\n",
               (unsigned long long)st->frames, (unsigned long long)fh.mapping, w, h, fourcc >> 24, fourcc >> 16 & 0xff, fourcc >> 8 & 0xff,
               fourcc & 0xff, ok ? "MATCH" : "DIFFERS", share * 100, tolerance, worst, psnr);
        if (out_dir) {
            char path[1200];
            snprintf(path, sizeof path, "%s/frame-%03llu-recorded.png", out_dir, (unsigned long long)st->frames);
            write_png(path, a, w, h);
            snprintf(path, sizeof path, "%s/frame-%03llu-replayed.png", out_dir, (unsigned long long)st->frames);
            write_png(path, b, w, h);
            snprintf(path, sizeof path, "%s/frame-%03llu-diff.png", out_dir, (unsigned long long)st->frames);
            write_png(path, diff, w, h);
        }
        free(b), free(diff);
    }
    free(a), free(want), free(have);
    return ok ? 0 : 1;
}

/* Where hv_pvg_lite_complete stores a channel's stamp: the FIFO buffer (register 0x1030, in the state blob). */
static uint64_t stamp_base;

static int wait_stamp(uint32_t ch, uint32_t stamp) {
    uint32_t *s = stamp_base ? hv_phys_ptr(machine, stamp_base + 4ull * ch, 4) : NULL;
    if (!s) return 0;
    for (int i = 0; i < 100000; i++) { /* 5 s */
        if ((int32_t)(__atomic_load_n(s, __ATOMIC_ACQUIRE) - stamp) >= 0) return 0;
        usleep(50);
    }
    return -1;
}

int main(int argc, char **argv) {
    int free_running = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--free")) free_running = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else path = argv[i];
    }
    if (getenv("HV_REPLAY_TOL")) tolerance = atoi(getenv("HV_REPLAY_TOL"));
    if (!path) {
        fprintf(stderr, "usage: pvg_replay [--free] [--out DIR] [--quiet] CAPTURE.pvgcap\n");
        return 2;
    }
    if (out_dir) mkdir(out_dir, 0755);
    Reader r = {.f = fopen(path, "rb")};
    char magic[8];
    if (!r.f || fread(magic, 1, 8, r.f) != 8 || memcmp(magic, HV_PVGCAP_MAGIC, 8)) {
        fprintf(stderr, "pvg_replay: %s is not a recording\n", path);
        return 2;
    }
    machine = calloc(1, sizeof *machine);
    machine->ncpus = 2;
    HvPvgLite *lite = NULL;
    HvPvgMetal *backend = NULL;
    uint8_t *display = NULL, page[PAGE];
    size_t display_len = 0;
    Stats st = {0};
    uint64_t cmd_diffs = 0;
    while (next_record(&r)) {
        switch (r.type) {
        case HV_PVGCAP_HEADER: {
            uint32_t version, n;
            memcpy(&version, r.data, 4), memcpy(&n, r.data + 4, 4);
            if (version != HV_PVGCAP_VERSION || n > HV_MAX_RAM) { fprintf(stderr, "pvg_replay: version %u, %u regions\n", version, n); return 2; }
            for (uint32_t i = 0; i < n; i++) {
                HvRam *ram = &machine->ram[machine->nram++];
                memcpy(&ram->pa, r.data + 8 + 16 * i, 8), memcpy(&ram->size, r.data + 16 + 16 * i, 8);
                ram->host = mmap(NULL, ram->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
                if (ram->host == MAP_FAILED) { fprintf(stderr, "pvg_replay: no memory for %llu MB of guest RAM\n", (unsigned long long)(ram->size >> 20)); return 2; }
            }
            lite = hv_pvg_lite_create(machine, NULL, 0, 0, 0, 0);
            backend = hv_pvg_metal_create(lite);
            if (!backend) { fprintf(stderr, "pvg_replay: the backend did not start\n"); return 2; }
            break;
        }
        case HV_PVGCAP_STATE: {
            if (hv_pvg_lite_set_state(lite, r.data, r.len)) { fprintf(stderr, "pvg_replay: state of %llu bytes not this build's\n", (unsigned long long)r.len); return 2; }
            uint32_t fifo_page;
            memcpy(&fifo_page, r.data + (0x1030 - 0x1000), 4);
            stamp_base = (uint64_t)fifo_page << 14;
            break;
        }
        case HV_PVGCAP_DISPLAY: /* shown once the first pages are in */
            display = malloc(r.len), display_len = r.len;
            memcpy(display, r.data, r.len);
            break;
        case HV_PVGCAP_PAGE: {
            uint64_t pa;
            uint32_t flags;
            if (unpack_page(r.data, r.len, &pa, &flags, page)) { fprintf(stderr, "pvg_replay: bad page record\n"); return 2; }
            uint8_t *d = hv_phys_ptr(machine, pa, PAGE);
            if (!d) break;
            st.pages++;
            if (flags & HV_PVGCAP_PAGE_WRITTEN) {
                st.written_pages++;
                if (memcmp(d, page, PAGE)) st.written_differ++, cmd_diffs++;
                if (free_running) break;
            }
            memcpy(d, page, PAGE);
            break;
        }
        case HV_PVGCAP_COMMAND: {
            if (display) {
                hv_pvg_metal_set_display_state(backend, display, display_len);
                free(display), display = NULL;
            }
            uint32_t ch, seq;
            memcpy(&ch, r.data, 4), memcpy(&seq, r.data + 4, 4);
            uint32_t len = (uint32_t)(r.len - 8), *cmd = malloc(len);
            memcpy(cmd, r.data + 8, len);
            uint32_t op = cmd[0] & 0xffff, stamp = cmd[2];
            if (cmd_diffs && !quiet && st.commands) printf("  command %llu wrote %llu pages that differ from the recording\n", (unsigned long long)st.commands - 1, (unsigned long long)cmd_diffs);
            cmd_diffs = 0;
            hv_pvg_metal_submit(backend, lite, ch, cmd, len);
            if (wait_stamp(ch, stamp)) {
                st.timeouts++;
                fprintf(stderr, "pvg_replay: command %u (channel %u op %x) not completed in 5 s\n", seq, ch, op);
            }
            st.commands++;
            break;
        }
        case HV_PVGCAP_UNMAP: {
            uint32_t mapping;
            memcpy(&mapping, r.data, 4);
            hv_pvg_metal_unmap(backend, mapping);
            break;
        }
        case HV_PVGCAP_CURSOR:
            hv_pvg_metal_cursor_moved(backend);
            break;
        case HV_PVGCAP_FRAME:
            compare_frame(lite, r.data, r.len, &st);
            break;
        case HV_PVGCAP_END:
            break;
        default:
            fprintf(stderr, "pvg_replay: record type %u skipped\n", r.type);
        }
    }
    printf("%llu commands, %llu pages, %llu GPU-written pages of which %llu differ (%s), %llu of %llu frames match, %llu timeouts\n",
           (unsigned long long)st.commands, (unsigned long long)st.pages, (unsigned long long)st.written_pages,
           (unsigned long long)st.written_differ, free_running ? "free-running" : "refreshed", (unsigned long long)st.frames_ok,
           (unsigned long long)st.frames, (unsigned long long)st.timeouts);
    hv_pvg_metal_destroy(backend);
    return st.frames_ok == st.frames && !st.timeouts ? 0 : 1;
}
