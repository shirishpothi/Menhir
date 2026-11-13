#ifndef HENGEVM_PVG_VULKAN_CAPTURE_H
#define HENGEVM_PVG_VULKAN_CAPTURE_H

#include <stdint.h>

/* A recording of the guest's GPU work, for replaying it off the device through any backend (Metal or Vulkan)
   and comparing what each draws. Used by pvg_lite.c; the format and the replay tool are in
   docs/android/graphics.md and android/gfx/replay/.

   HV_PVG_CAPTURE=PREFIX turns it on. Every guest page the device or its backend reads is then noted (cheap). While
   a file named PREFIX.on exists, the device runs each command to completion before the next, and writes
   PREFIX-N.pvgcap: first the device's state, the display state and every page noted so far, then for each command
   the pages that changed and the pages it read for the first time, then the command. After each swap it writes the
   swapped surface's pixels as the backend left them, the reference a replay is compared with.
   HV_PVG_CAPTURE_FRAMES=N ends a recording after N swaps (default 30). Record with one guest CPU (--cpus 1): the
   recorder stops only the CPU that rang, so with more, pages other CPUs write while a command runs look like the
   GPU's work. */

struct HvMachine;
typedef struct HvPvgLite HvPvgLite;

/* Record types. Every record is {u32 type, u32 0, u64 length} then length bytes. */
enum {
    HV_PVGCAP_HEADER = 1,  /* u32 version, u32 ram regions, {u64 pa, u64 size}..., u64 guest CPUs */
    HV_PVGCAP_STATE = 2,   /* hv_pvg_lite_state's blob */
    HV_PVGCAP_DISPLAY = 3, /* hv_pvg_lite_display_state's blob */
    HV_PVGCAP_PAGE = 4,    /* u64 pa, u32 stored bytes (0: all zero, 16384: raw, else zlib), u32 flags, data */
    HV_PVGCAP_COMMAND = 5, /* u32 channel, u32 sequence, the command as the backend is given it */
    HV_PVGCAP_UNMAP = 6,   /* u32 mapping */
    HV_PVGCAP_CURSOR = 7,  /* the pointer moved (the display page holds where) */
    HV_PVGCAP_FRAME = 8,   /* u64 mapping, u32 width, height, fourcc, bytes per pixel, then zlib of the rows, packed */
    HV_PVGCAP_END = 9,
};
/* A page's flags: it changed while the command before it ran, so the backend (almost always the GPU) wrote it. */
#define HV_PVGCAP_PAGE_WRITTEN 1u
#define HV_PVGCAP_MAGIC "PVGCAP\0\1"
#define HV_PVGCAP_VERSION 2

/* hv_phys_ptr for pvg_lite.c: notes the pages, and while recording copies a page the first time it is read. */
void *hv_pvg_capture_phys(struct HvMachine *m, uint64_t pa, uint64_t len);
/* Before a command goes to the backend; returns nonzero if it is being recorded (call hv_pvg_capture_wait after). */
int hv_pvg_capture_command(HvPvgLite *p, uint32_t channel, const uint32_t *command, uint32_t length);
/* After hv_pvg_capture_command said yes and the backend has it: waits for its stamp, then writes it. */
void hv_pvg_capture_wait(HvPvgLite *p, uint32_t channel, const uint32_t *command, uint32_t length);
/* A stamp the backend completed. */
void hv_pvg_capture_complete(uint32_t channel, uint32_t stamp);
void hv_pvg_capture_unmap(uint32_t mapping);
void hv_pvg_capture_cursor(void);

#endif
