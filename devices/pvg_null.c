#if !defined(__APPLE__)
/* Headless backend: retire every command; no pixels are produced. */
#include "pvg_lite.h"
#include <stdlib.h>
struct HvPvgMetal { int unused; };
HvPvgMetal *hv_pvg_metal_create(HvPvgLite *p) { (void)p; return calloc(1, sizeof(HvPvgMetal)); }
void hv_pvg_metal_destroy(HvPvgMetal *m) { free(m); }
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *p, uint32_t ch, uint32_t *cmd, uint32_t len) {
    (void)m;
    if (len >= 12) hv_pvg_lite_complete(p, ch, cmd[2]);
    free(cmd);
}
int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **p, uint32_t *w, uint32_t *h) { (void)m; (void)p; (void)w; (void)h; return -1; }
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *p, uint32_t w, uint32_t h) { (void)m; (void)p; (void)w; (void)h; return -1; }
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t id) { (void)m; (void)id; }
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *p) { (void)m; (void)p; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { (void)m; return 0; }
void hv_pvg_metal_quiesce(HvPvgMetal *m) { (void)m; }
size_t hv_pvg_metal_display_state(HvPvgMetal *m, void *p, size_t n) { (void)m; (void)p; (void)n; return 0; }
void hv_pvg_metal_set_display_state(HvPvgMetal *m, const void *p, size_t n) { (void)m; (void)p; (void)n; }
#endif
