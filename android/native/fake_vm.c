/* A pretend engine behind henge-vm.h, so the app can be built and tested before the real engine runs on
   Android (CMake: MENHIR_FAKE_ENGINE). It keeps the real state machine (0 not started, 3 starting,
   1 running, 4 paused, 2 stopped), runs a pretend restore through the real step numbers, saves and
   resumes a small file in place of a snapshot, and draws a moving test pattern into the display window
   with the fingers, the pointer, the buttons and the last key on it. Every input call is also logged
   (logcat tag menhir-fake), which is what the emulator checks read. Nothing here runs guest code. */
#include "henge-vm.h"

#include <android/log.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAG "menhir-fake"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define SENSOR_W 8134
#define SENSOR_H 5206
#define MAX_FINGERS 16

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int state;
static _Atomic uint64_t generation;
static _Atomic int install_step = -1;
static _Atomic double install_progress_value;
static int installer_session;
static int exit_outcome = HENGE_VM_OUTCOME_ERROR;
static const char *exit_text = "running";
static uint64_t started_ns;

/* What the screen shows. All under lock. */
static uint32_t mode_w = 1920, mode_h = 1200, mode_mm_w, mode_mm_h;
static ANativeWindow *window;
static pthread_t render_thread;
static int render_running;
static uint64_t frames;
static struct { int down, x, y; } fingers[MAX_FINGERS];
static int pointer_x = 960, pointer_y = 600;
static int prev_x[MAX_FINGERS], prev_y[MAX_FINGERS];
static int buttons[3], pad_button;
static int scroll_x, scroll_y;
static char last_key[48] = "NONE";
static uint64_t touch_frames;
static int hid_down[256];

/* The shared clipboard. */
static char *host_clipboard, *guest_clipboard;
static int share_on;
static int copies;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static void set_err(char *err, size_t errlen, const char *text) {
    if (err && errlen) snprintf(err, errlen, "%s", text);
}

/* ---- Drawing ------------------------------------------------------------------------------------- */

/* A 3x5 font: each glyph is five rows of three bits, top row first. */
static const char glyph_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-./ ";
static const uint8_t glyphs[][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
    {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4}, {7, 4, 5, 5, 7}, {5, 5, 7, 5, 5},
    {7, 2, 2, 2, 7}, {1, 1, 1, 5, 7}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5},
    {7, 5, 5, 5, 7}, {7, 5, 7, 4, 4}, {7, 5, 5, 7, 1}, {6, 5, 6, 5, 5}, {7, 4, 7, 1, 7}, {7, 2, 2, 2, 2},
    {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
    {0, 2, 0, 2, 0}, {0, 0, 7, 0, 0}, {0, 0, 0, 0, 2}, {1, 1, 2, 4, 4}, {0, 0, 0, 0, 0},
};

typedef struct {
    uint32_t *px;
    int w, h, stride;
} Canvas;

static void fill(Canvas *c, int x0, int y0, int w, int h, uint32_t color) {
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) c->px[y * c->stride + x] = color;
}

static void text(Canvas *c, int x, int y, int scale, uint32_t color, const char *s) {
    for (; *s; s++, x += 4 * scale) {
        char ch = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        const char *at = strchr(glyph_chars, ch);
        if (!at) at = strchr(glyph_chars, ' ');
        const uint8_t *g = glyphs[at - glyph_chars];
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) fill(c, x + col * scale, y + row * scale, scale, scale, color);
    }
}

static void textf(Canvas *c, int x, int y, int scale, uint32_t color, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    text(c, x, y, scale, color, line);
}

/* RGBX in memory is R, G, B, X: a little-endian word 0xXXBBGGRR. */
static uint32_t rgb(int r, int g, int b) { return 0xff000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }

static const char *state_name(int s) {
    switch (s) {
    case 0: return "NOT STARTED";
    case 1: return installer_session ? "INSTALLING" : "RUNNING";
    case 2: return "STOPPED";
    case 3: return "STARTING";
    case 4: return "PAUSED";
    default: return "?";
    }
}

static void draw(Canvas *c) {
    static const uint32_t bars[] = {0xffc0c0c0, 0xff00c0c0, 0xffc0c000, 0xff00c000,
                                    0xffc000c0, 0xff0000c0, 0xffc00000, 0xff202020};
    int s = state;
    int shift = (int)(frames * 6 % (uint64_t)c->w);
    int bar_w = c->w / 8 > 0 ? c->w / 8 : 1;
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < c->w; x++) {
            uint32_t color = bars[((x + shift + scroll_x * 8) / bar_w) % 8];
            if (((y + scroll_y * 8) / 64) % 2) color = (color >> 1) & 0xff7f7f7f;
            if (s == 4) color = (color >> 2) & 0xff3f3f3f;
            row[x] = color | 0xff000000u;
        }
    }
    int scale = c->h / 160 > 2 ? c->h / 160 : 2;
    int line = 7 * scale, x0 = 4 * scale, y0 = 4 * scale;
    fill(c, 0, 0, 120 * scale, 11 * line, 0xff000000u);
    uint32_t white = rgb(255, 255, 255);
    textf(c, x0, y0, scale, white, "MENHIR FAKE ENGINE");
    textf(c, x0, y0 + line, scale, white, "MODE %ux%u MM %ux%u", mode_w, mode_h, mode_mm_w, mode_mm_h);
    textf(c, x0, y0 + 2 * line, scale, white, "STATE %s FRAME %llu", state_name(s), (unsigned long long)frames);
    textf(c, x0, y0 + 3 * line, scale, white, "KEY %s", last_key);
    int n = 0;
    for (int i = 0; i < MAX_FINGERS; i++) n += fingers[i].down;
    textf(c, x0, y0 + 4 * line, scale, white, "FINGERS %d FRAMES %llu", n, (unsigned long long)touch_frames);
    textf(c, x0, y0 + 5 * line, scale, white, "BUTTONS L%d R%d M%d PAD%d", buttons[0], buttons[1], buttons[2], pad_button);
    textf(c, x0, y0 + 6 * line, scale, white, "POINTER %d %d SCROLL %d %d", pointer_x, pointer_y, scroll_x, scroll_y);
    textf(c, x0, y0 + 7 * line, scale, white, "SHARE %s COPIES %d", share_on ? "ON" : "OFF", copies);
    if (installer_session && s == 1)
        textf(c, x0, y0 + 8 * line, scale, white, "INSTALL STEP %d %d", (int)install_step,
              (int)(install_progress_value * 100));
    /* The fingers where they are on the pad, which fills the screen here. */
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!fingers[i].down) continue;
        int fx = (int)((int64_t)fingers[i].x * c->w / SENSOR_W), fy = (int)((int64_t)fingers[i].y * c->h / SENSOR_H);
        int r = 10 * scale;
        fill(c, fx - r, fy - r, 2 * r, 2 * r, rgb(255, 255, 255));
        fill(c, fx - r / 2, fy - r / 2, r, r, rgb(255, 80, 0));
        textf(c, fx + r + scale, fy - r, scale, white, "%d", i);
    }
    /* The pointer: white, red with the left button down, blue with the right. */
    uint32_t pc = buttons[0] || pad_button ? rgb(255, 0, 0) : buttons[1] ? rgb(0, 80, 255) : rgb(255, 255, 255);
    fill(c, pointer_x - 2 * scale, pointer_y - 2 * scale, 8 * scale, 8 * scale, rgb(0, 0, 0));
    fill(c, pointer_x - scale, pointer_y - scale, 6 * scale, 6 * scale, pc);
}

static void *render_loop(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        if (!render_running) {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        if (window) {
            ANativeWindow_Buffer buf;
            if (ANativeWindow_lock(window, &buf, NULL) == 0) {
                Canvas c = {buf.bits, buf.width, buf.height, buf.stride};
                if (buf.format == WINDOW_FORMAT_RGBX_8888 || buf.format == WINDOW_FORMAT_RGBA_8888) draw(&c);
                ANativeWindow_unlockAndPost(window);
                frames++;
            }
        }
        pthread_mutex_unlock(&lock);
        sleep_ms(33);
    }
}

void henge_vm_set_display_surface(struct ANativeWindow *w) {
    pthread_mutex_lock(&lock);
    if (window) ANativeWindow_release(window);
    window = w;
    if (window) {
        ANativeWindow_acquire(window);
        ANativeWindow_setBuffersGeometry(window, (int32_t)mode_w, (int32_t)mode_h, WINDOW_FORMAT_RGBX_8888);
    }
    if (window && !render_running) {
        render_running = 1;
        pthread_create(&render_thread, NULL, render_loop, NULL);
        pthread_detach(render_thread);
    }
    pthread_mutex_unlock(&lock);
    LOG("display surface %s", w ? "set" : "gone");
}

uint64_t henge_vm_gpu_frames(void) {
    pthread_mutex_lock(&lock);
    uint64_t n = frames;
    pthread_mutex_unlock(&lock);
    return n;
}

/* ---- The machine --------------------------------------------------------------------------------- */

static int begin_start(char *err, size_t errlen) {
    pthread_mutex_lock(&lock);
    int s = state;
    if ((s != 0 && s != 2) || installer_session) {
        pthread_mutex_unlock(&lock);
        set_err(err, errlen, installer_session ? "The installer is still attached. Restart Menhir before starting a Mac."
                                               : "The previous Mac is still running or finishing its shutdown.");
        return -1;
    }
    state = 3;
    exit_outcome = HENGE_VM_OUTCOME_ERROR;
    exit_text = "running";
    memset(fingers, 0, sizeof fingers);
    memset(buttons, 0, sizeof buttons);
    memset(hid_down, 0, sizeof hid_down);
    pad_button = scroll_x = scroll_y = 0;
    pointer_x = (int)mode_w / 2;
    pointer_y = (int)mode_h / 2;
    snprintf(last_key, sizeof last_key, "NONE");
    pthread_mutex_unlock(&lock);
    return 0;
}

static void *boot_thread(void *arg) {
    (void)arg;
    sleep_ms(1000);
    pthread_mutex_lock(&lock);
    if (state == 3) {
        state = 1;
        started_ns = now_ns();
        generation++;
    }
    pthread_mutex_unlock(&lock);
    LOG("the Mac is running");
    return NULL;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen) {
    (void)seed;
    if (!file_exists(rom) || !file_exists(disk) || !file_exists(aux)) {
        set_err(err, errlen, "The Mac's boot ROM, disk or auxiliary storage is missing.");
        return -1;
    }
    if (begin_start(err, errlen)) return -1;
    LOG("boot: ecid %llu, %llu MB, %d CPUs, disk %s", (unsigned long long)ecid, (unsigned long long)ram_mb, cpus, disk);
    pthread_t t;
    pthread_create(&t, NULL, boot_thread, NULL);
    pthread_detach(t);
    return 0;
}

static void *install_thread(void *arg) {
    (void)arg;
    static const struct { int step, ms; } plan[] = {{0, 1000}, {1, 1500}, {2, 12000}, {3, 2000}, {4, 2000}, {7, 2000}};
    for (size_t i = 0; i < sizeof plan / sizeof plan[0]; i++) {
        install_step = plan[i].step;
        for (int t = 0; t <= plan[i].ms; t += 100) {
            if (state != 1) {
                install_step = 101;
                LOG("install: stopped during step %d", plan[i].step);
                return NULL;
            }
            install_progress_value = (double)t / plan[i].ms;
            sleep_ms(100);
        }
        LOG("install: step %d done", plan[i].step);
    }
    install_progress_value = 1;
    install_step = 100;
    pthread_mutex_lock(&lock);
    if (state == 1) {
        state = 2;
        exit_outcome = HENGE_VM_OUTCOME_RESTART;
        exit_text = "guest requested a restart";
    }
    pthread_mutex_unlock(&lock);
    LOG("install: done");
    return NULL;
}

int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen) {
    (void)workdir;
    if (!file_exists(rom) || !file_exists(disk) || !file_exists(aux) || !file_exists(ipsw)) {
        set_err(err, errlen, "The boot ROM, disk, auxiliary storage or restore image is missing.");
        return -1;
    }
    if (begin_start(err, errlen)) return -1;
    LOG("install %s: ecid %llu, %llu MB, %d CPUs", ipsw, (unsigned long long)ecid, (unsigned long long)ram_mb, cpus);
    pthread_mutex_lock(&lock);
    installer_session = 1;
    install_step = 0;
    install_progress_value = 0;
    state = 1;
    generation++;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_create(&t, NULL, install_thread, NULL);
    pthread_detach(t);
    return 0;
}

int henge_install_step(void) { return install_step; }
double henge_install_progress(void) { return install_progress_value; }

/* The same file the real engine writes: a 24-byte header, the rest zero (32 MB + 16 KB), sparse. */
int henge_make_aux(const char *path) {
    static const unsigned char header[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27, 1, 0, 0, 0, 0, 0, 0, 0,
                                             1, 0, 0, 0, 0, 0, 0, 0};
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, header, sizeof header) == (ssize_t)sizeof header && ftruncate(fd, 33570816) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int henge_vm_state(void) { return state; }
uint64_t henge_vm_generation(void) { return generation; }
int henge_vm_can_start(void) {
    int s = state;
    return (s == 0 || s == 2) && !installer_session;
}
const char *henge_vm_exit_reason(void) { return state == 2 ? exit_text : "running"; }
int henge_vm_exit_outcome(void) { return state == 2 ? exit_outcome : HENGE_VM_OUTCOME_ERROR; }

static void *stop_thread(void *arg) {
    (void)arg;
    sleep_ms(500);
    pthread_mutex_lock(&lock);
    state = 2;
    exit_outcome = HENGE_VM_OUTCOME_HOST_STOP;
    exit_text = "stopped by host";
    pthread_mutex_unlock(&lock);
    LOG("the Mac stopped");
    return NULL;
}

void henge_vm_stop(void) {
    pthread_mutex_lock(&lock);
    int s = state;
    if (s == 1 || s == 4) state = 3;
    pthread_mutex_unlock(&lock);
    if (s != 1 && s != 4) return;
    LOG("stop");
    pthread_t t;
    pthread_create(&t, NULL, stop_thread, NULL);
    pthread_detach(t);
}

int henge_vm_knob(const char *name, int value) {
    LOG("knob %s = %d", name, value);
    return 0;
}
long henge_vm_balloon(long target_mb) {
    (void)target_mb;
    return -1;
}

/* ---- Instant resume: a small text file stands in for the snapshot -------------------------------- */

int henge_vm_pause(char *err, size_t errlen) {
    pthread_mutex_lock(&lock);
    int s = state, ok = s == 1 && !installer_session;
    if (ok) state = 4;
    pthread_mutex_unlock(&lock);
    if (s == 4) return 0;
    if (!ok) {
        set_err(err, errlen, installer_session ? "An install cannot be paused." : "No Mac is running.");
        return -1;
    }
    LOG("paused");
    return 0;
}

int henge_vm_save(const char *path, const char *ident, char *err, size_t errlen) {
    if (state != 4) {
        set_err(err, errlen, "The Mac is not paused.");
        return -1;
    }
    char part[1024];
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "w");
    if (!f) {
        set_err(err, errlen, "Cannot write the saved Mac.");
        return -1;
    }
    fprintf(f, "menhir-fake-snapshot\n%s\n%lld\n", ident, (long long)time(NULL));
    fclose(f);
    if (rename(part, path) != 0) {
        set_err(err, errlen, "Cannot write the saved Mac.");
        return -1;
    }
    LOG("saved to %s", path);
    return 0;
}

int henge_vm_suspend(const char *path, const char *ident, char *err, size_t errlen) {
    if (henge_vm_pause(err, errlen) != 0) return -1;
    int r = henge_vm_save(path, ident, err, errlen);
    if (r) henge_vm_continue();
    return r;
}

int henge_vm_continue(void) {
    pthread_mutex_lock(&lock);
    int ok = state == 4;
    if (ok) state = 1;
    pthread_mutex_unlock(&lock);
    if (ok) LOG("runs on");
    return ok ? 0 : -1;
}

int henge_vm_saved_ok(const char *path, const char *ident, uint64_t *bytes, int64_t *saved_at) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char magic[64] = "", saved_ident[1024] = "";
    long long at = 0;
    int ok = fgets(magic, sizeof magic, f) && fgets(saved_ident, sizeof saved_ident, f) && fscanf(f, "%lld", &at) == 1;
    long size = ftell(f);
    fclose(f);
    saved_ident[strcspn(saved_ident, "\n")] = 0;
    if (!ok || strcmp(magic, "menhir-fake-snapshot\n") != 0 || strcmp(saved_ident, ident) != 0) return -1;
    if (bytes) *bytes = (uint64_t)size;
    if (saved_at) *saved_at = at;
    return 0;
}

int henge_vm_resume(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                    int cpus, const char *path, const char *ident, char *err, size_t errlen) {
    if (henge_vm_saved_ok(path, ident, NULL, NULL) != 0) {
        unlink(path);
        set_err(err, errlen, "The saved Mac is not for this Mac or this build.");
        return -1;
    }
    unlink(path);
    LOG("resuming from %s", path);
    return henge_vm_boot(rom, disk, aux, ecid, seed, ram_mb, cpus, err, errlen);
}

/* ---- Progress ------------------------------------------------------------------------------------ */

uint64_t henge_vm_blocks(void) { return state == 1 ? (now_ns() - started_ns) / 10000 : 0; }
uint64_t henge_vm_net_frames(int sent) { return state == 1 ? (now_ns() - started_ns) / (sent ? 50000000 : 40000000) : 0; }
uint64_t henge_vm_dns(int which) { return which == 0 && state == 1 ? 3 : 0; }
int henge_vm_cpus_on(void) { return state == 1 ? 4 : 0; }
int henge_vm_guest_el0(void) { return state == 1; }

/* ---- Sharing ------------------------------------------------------------------------------------- */

void henge_vm_share(const char *folder, const char *socket_path, const char *agent) {
    LOG("share: folder %s, socket %s, agent %s", folder, socket_path, agent ? agent : "(none)");
    pthread_mutex_lock(&lock);
    share_on = 1;
    pthread_mutex_unlock(&lock);
}

void henge_share_set_clipboard(const char *text) {
    pthread_mutex_lock(&lock);
    free(host_clipboard);
    host_clipboard = strdup(text ? text : "");
    pthread_mutex_unlock(&lock);
    LOG("clipboard from the device: %s", text);
}

char *henge_share_take_clipboard(void) {
    pthread_mutex_lock(&lock);
    char *text = guest_clipboard;
    guest_clipboard = NULL;
    pthread_mutex_unlock(&lock);
    return text;
}

/* The helper "polls" every few seconds while the Mac runs. */
long henge_share_agent_seen(void) { return share_on && state == 1 ? 2 : -1; }

/* ---- Display mode -------------------------------------------------------------------------------- */

void henge_vm_set_display(uint32_t width, uint32_t height) {
    pthread_mutex_lock(&lock);
    mode_w = width;
    mode_h = height;
    if (window) ANativeWindow_setBuffersGeometry(window, (int32_t)width, (int32_t)height, WINDOW_FORMAT_RGBX_8888);
    pthread_mutex_unlock(&lock);
    LOG("display %ux%u", width, height);
}

void henge_vm_set_display_mm(uint32_t width_mm, uint32_t height_mm) {
    pthread_mutex_lock(&lock);
    mode_mm_w = width_mm;
    mode_mm_h = height_mm;
    pthread_mutex_unlock(&lock);
    LOG("display size %ux%u mm", width_mm, height_mm);
}

/* ---- Input --------------------------------------------------------------------------------------- */

static int running(void) { return state == 1; }

static void clamp_pointer(void) {
    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if (pointer_x >= (int)mode_w) pointer_x = (int)mode_w - 1;
    if (pointer_y >= (int)mode_h) pointer_y = (int)mode_h - 1;
}

void henge_vm_key(uint16_t code, int down) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    snprintf(last_key, sizeof last_key, "LINUX %u %s", code, down ? "DOWN" : "UP");
    pthread_mutex_unlock(&lock);
    LOG("key linux %u %s", code, down ? "down" : "up");
}

void henge_vm_key_hid(uint8_t usage, int down) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    hid_down[usage] = down;
    snprintf(last_key, sizeof last_key, "HID %02X %s", usage, down ? "DOWN" : "UP");
    /* Command-C copies on the pretend Mac, so the clipboard can be tested end to end. */
    int command = hid_down[0xe3] || hid_down[0xe7];
    if (down && command && usage == 0x06) {
        free(guest_clipboard);
        char text[64];
        snprintf(text, sizeof text, "Copied on the fake Mac (%d)", ++copies);
        guest_clipboard = strdup(text);
    }
    if (down && command && usage == 0x19) LOG("pasted on the fake Mac: %s", host_clipboard ? host_clipboard : "");
    pthread_mutex_unlock(&lock);
    LOG("key hid 0x%02x %s", usage, down ? "down" : "up");
}

void henge_vm_mouse_move(int dx, int dy) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    pointer_x += dx;
    pointer_y += dy;
    clamp_pointer();
    pthread_mutex_unlock(&lock);
    LOG("mouse move %d %d", dx, dy);
}

void henge_vm_mouse_button(int button, int down) {
    if (!running() || button < 0 || button > 2) return;
    pthread_mutex_lock(&lock);
    buttons[button] = down;
    pthread_mutex_unlock(&lock);
    LOG("mouse button %d %s", button, down ? "down" : "up");
}

void henge_vm_mouse_scroll(int dx, int dy) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    scroll_x += dx;
    scroll_y += dy;
    pthread_mutex_unlock(&lock);
    LOG("mouse scroll %d %d", dx, dy);
}

void henge_vm_touch(int finger, int phase, int x, int y, uint32_t ts_ms) {
    if (!running() || finger < 1 || finger >= MAX_FINGERS) return;
    pthread_mutex_lock(&lock);
    if (phase == 2) {
        fingers[finger].down = 0;
    } else {
        /* One finger moves the pointer, as the Mac's own trackpad driver would. */
        int count = 0;
        for (int i = 0; i < MAX_FINGERS; i++) count += fingers[i].down;
        if (phase == 1 && fingers[finger].down && count == 1) {
            pointer_x += (x - prev_x[finger]) * (int)mode_w / SENSOR_W;
            pointer_y += (y - prev_y[finger]) * (int)mode_h / SENSOR_H;
            clamp_pointer();
        }
        fingers[finger].down = 1;
        fingers[finger].x = x;
        fingers[finger].y = y;
        prev_x[finger] = x;
        prev_y[finger] = y;
    }
    pthread_mutex_unlock(&lock);
    LOG("touch finger %d phase %d at %d,%d t=%u", finger, phase, x, y, ts_ms);
}

void henge_vm_touch_frame(uint32_t ts_ms) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    touch_frames++;
    pthread_mutex_unlock(&lock);
    LOG("touch frame t=%u", ts_ms);
}

void henge_vm_touch_button(int down) {
    if (!running()) return;
    pthread_mutex_lock(&lock);
    pad_button = down;
    pthread_mutex_unlock(&lock);
    LOG("pad button %s", down ? "down" : "up");
}

int henge_vm_pointer(int *x, int *y) {
    if (!running()) return -1;
    pthread_mutex_lock(&lock);
    *x = pointer_x;
    *y = pointer_y;
    pthread_mutex_unlock(&lock);
    return 0;
}

int henge_vm_snapshot(uint8_t *bgra, int width, int height) {
    if (!running()) return -1;
    memset(bgra, 0x80, (size_t)width * (size_t)height * 4);
    return 0;
}

int henge_vm_boot_display(const uint8_t **pixels, int *width, int *height, int *stride) {
    (void)pixels, (void)width, (void)height, (void)stride;
    return -1;
}
void henge_vm_free_display(const uint8_t *pixels) { free((void *)pixels); }

/* ---- Memory -------------------------------------------------------------------------------------- */

uint64_t henge_largest_reservation_gb(void) {
    for (uint64_t gb = 4096; gb >= 4; gb /= 2) {
        void *p = mmap(NULL, gb << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            munmap(p, gb << 30);
            return gb;
        }
    }
    return 0;
}

uint64_t henge_largest_mapping_mb(void) {
    for (uint64_t mb = 16896; mb >= 1024; mb -= 512) {
        void *p = mmap(NULL, mb << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            munmap(p, mb << 20);
            return mb;
        }
    }
    return 0;
}

size_t henge_available_memory(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return (size_t)kb << 10;
}
