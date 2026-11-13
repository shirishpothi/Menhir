/* The engine (HengeVM, ../../host/vm.h) as the Android app sees it.

   The same functions as ios/Henge/Native/henge-vm.h, with the same names and meanings, so behaviour can
   be compared with upstream. The differences:
   - the display target is an ANativeWindow (henge_vm_set_display_surface), not a CAMetalLayer;
   - there is no JIT region to hand in: the engine maps its own;
   - henge_available_memory reads the kernel's MemAvailable, since Android has no per-app limit call.

   Two implementations: henge-vm.c over the real engine, and fake_vm.c, which simulates a Mac so the app
   can be built and tested before the engine runs on Android. CMake's MENHIR_FAKE_ENGINE picks one. */
#ifndef HENGE_VM_H
#define HENGE_VM_H

#include <stddef.h>
#include <stdint.h>

struct ANativeWindow;

/* Builds the machine from its files and starts it. 0, or -1 with a reason in err. */
int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen);
/* Installs macOS from an IPSW onto a new machine (blank disk and auxiliary storage): boots the ROM in DFU
   mode and runs the restore over its USB port. workdir is scratch space. 0, or -1 with a reason in err. */
int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen);
/* The restore's step (0 detect, 1 prepare, 2 system image, 3 verify, 4 firmware, 6 FUD,
   7 personalized images; 100 done, 101 failed) and how far it is (0..1). */
int henge_install_step(void);
double henge_install_progress(void);
/* Writes a blank auxiliary storage file. 0 or -1. */
int henge_make_aux(const char *path);

/* Instant resume (../../host/snapshot.h). See the iOS header for the full contract. */
int henge_vm_suspend(const char *path, const char *ident, char *err, size_t errlen);
int henge_vm_pause(char *err, size_t errlen);
int henge_vm_save(const char *path, const char *ident, char *err, size_t errlen);
int henge_vm_continue(void);
int henge_vm_resume(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                    int cpus, const char *path, const char *ident, char *err, size_t errlen);
int henge_vm_saved_ok(const char *path, const char *ident, uint64_t *bytes, int64_t *saved_at);

/* 0 not started, 1 running, 2 stopped, 3 starting/stopping, 4 paused. */
int henge_vm_state(void);
uint64_t henge_vm_generation(void);
int henge_vm_can_start(void);
const char *henge_vm_exit_reason(void);
enum HengeVmOutcome {
    HENGE_VM_OUTCOME_ERROR, HENGE_VM_OUTCOME_SHUTDOWN, HENGE_VM_OUTCOME_RESTART,
    HENGE_VM_OUTCOME_HOST_STOP, HENGE_VM_OUTCOME_PANIC
};
int henge_vm_exit_outcome(void);
void henge_vm_stop(void);
int henge_vm_knob(const char *name, int value);
long henge_vm_balloon(long target_mb);

/* Progress, for the session screen. */
uint64_t henge_vm_blocks(void);
uint64_t henge_vm_net_frames(int sent);
uint64_t henge_vm_dns(int which);
int henge_vm_cpus_on(void);
int henge_vm_guest_el0(void);

/* The shared folder and clipboard (host/share.c). henge_vm_share goes before henge_vm_boot; agent may be
   NULL. henge_share_take_clipboard returns what the guest copied, once: a malloc'd string, or NULL. */
void henge_vm_share(const char *folder, const char *socket_path, const char *agent);
void henge_share_set_clipboard(const char *text);
char *henge_share_take_clipboard(void);
long henge_share_agent_seen(void);

/* The size macOS is told its screen is, and the panel's physical size (0, 0: a ~122 ppi 1x screen).
   Both before boot. */
void henge_vm_set_display(uint32_t width, uint32_t height);
void henge_vm_set_display_mm(uint32_t width_mm, uint32_t height_mm);

/* Input. henge_vm_key takes Linux key codes, henge_vm_key_hid USB HID keyboard usages. Buttons: 0 left,
   1 right, 2 middle. Wheel units: dy > 0 up, dx > 0 right. */
void henge_vm_key(uint16_t code, int down);
void henge_vm_key_hid(uint8_t usage, int down);
void henge_vm_mouse_move(int dx, int dy);
void henge_vm_mouse_button(int button, int down);
void henge_vm_mouse_scroll(int dx, int dy);
/* The Magic Trackpad 2: phase 0 down, 1 move, 2 up; finger 1..15; x 0..8134, y 0..5206; ts_ms is the
   sample's own clock. */
void henge_vm_touch(int finger, int phase, int x, int y, uint32_t ts_ms);
void henge_vm_touch_frame(uint32_t ts_ms);
void henge_vm_touch_button(int down);
int henge_vm_pointer(int *x, int *y);
int henge_vm_snapshot(uint8_t *bgra, int width, int height);

/* Where the Mac's frames go: the SurfaceView's window, or NULL when that surface is destroyed (the app
   went to the background, or the session closed). A new window comes when the surface is created again.
   The caller holds its own reference to the window for as long as it is set; the engine takes its own
   (ANativeWindow_acquire) if it keeps the window, and must not touch the old window once this returns. */
void henge_vm_set_display_surface(struct ANativeWindow *window);
uint64_t henge_vm_gpu_frames(void);
int henge_vm_boot_display(const uint8_t **pixels, int *width, int *height, int *stride);
void henge_vm_free_display(const uint8_t *pixels);

uint64_t henge_largest_reservation_gb(void);
uint64_t henge_largest_mapping_mb(void);
/* Bytes the system says are available (MemAvailable in /proc/meminfo). */
size_t henge_available_memory(void);

#endif
