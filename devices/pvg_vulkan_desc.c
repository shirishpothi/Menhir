/* Metal's serialized pipeline descriptors (pvg_vulkan_desc.h). An object is a u8 field count, then per field
   {u8 key, u8 size, value}; strings, objects and arrays are stored after the field table, the value being their
   offset from the start of the object. An array is u32 count, count u32 offsets from the array's start, then the
   element objects. */

#include <string.h>

#include "pvg_vulkan_desc.h"

typedef struct {
    const uint8_t *d;
    size_t len;
} Buf;

typedef void (*FieldFn)(Buf b, size_t obj, uint32_t key, uint64_t value, void *ctx);

static int walk(Buf b, size_t obj, FieldFn fn, void *ctx) {
    if (obj >= b.len) return -1;
    uint32_t n = b.d[obj];
    size_t at = obj + 1;
    for (uint32_t i = 0; i < n; i++) {
        if (at + 2 > b.len) return -1;
        uint32_t key = b.d[at], size = b.d[at + 1];
        if (size > 8 || at + 2 + size > b.len) return -1;
        uint64_t v = 0;
        memcpy(&v, b.d + at + 2, size); /* little-endian, as the host */
        fn(b, obj, key, v, ctx);
        at += 2 + size;
    }
    return 0;
}

static void string_at(Buf b, size_t obj, uint64_t offset, char *out, size_t cap) {
    size_t at = obj + (size_t)offset, n = 0;
    while (at + n < b.len && b.d[at + n] && n + 1 < cap) n++;
    if (offset < b.len) memcpy(out, b.d + at, n);
    out[n] = 0;
}

/* ---- Pieces shared by both pipelines ---- */

typedef struct {
    uint32_t index, value;
    int have_index;
} Pair;

static void mutability_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    (void)b, (void)obj;
    Pair *p = ctx;
    if (key == 0) p->index = (uint32_t)v, p->have_index = 1;
    else if (key == 1) p->value = (uint32_t)v;
}

static int read_mutability(Buf b, size_t obj, uint64_t offset, uint8_t *out) {
    size_t arr = obj + (size_t)offset;
    uint32_t count;
    if (arr + 4 > b.len) return -1;
    memcpy(&count, b.d + arr, 4);
    if (count > 256 || arr + 4 + 4ull * count > b.len) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off;
        memcpy(&off, b.d + arr + 4 + 4 * i, 4);
        Pair p = {0};
        if (walk(b, arr + off, mutability_field, &p)) return -1;
        if (p.index < PVG_MAX_VERTEX) out[p.index] = (uint8_t)p.value;
    }
    return 0;
}

typedef struct {
    PvgVertexAttribute a;
    uint32_t index;
} AttrCtx;

static void attribute_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    (void)b, (void)obj;
    AttrCtx *c = ctx;
    switch (key) {
    case 0: c->index = (uint32_t)v; break;
    case 1: c->a.format = (uint32_t)v; break;
    case 2: c->a.offset = (uint32_t)v; break;
    case 3: c->a.buffer = (uint32_t)v; break;
    }
}

typedef struct {
    PvgVertexLayout l;
    uint32_t index;
} LayoutCtx;

static void layout_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    (void)b, (void)obj;
    LayoutCtx *c = ctx;
    switch (key) {
    case 0: c->index = (uint32_t)v; break;
    case 1: c->l.step_function = (uint32_t)v; break;
    case 2: c->l.step_rate = (uint32_t)v; break;
    case 3: c->l.stride = (uint32_t)v; break;
    }
}

typedef struct {
    PvgVertexDescriptor *vd;
    int bad;
} VdCtx;

static int read_elements(Buf b, size_t obj, uint64_t offset, int layouts, PvgVertexDescriptor *vd) {
    size_t arr = obj + (size_t)offset;
    uint32_t count;
    if (arr + 4 > b.len) return -1;
    memcpy(&count, b.d + arr, 4);
    if (count > 256 || arr + 4 + 4ull * count > b.len) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off;
        memcpy(&off, b.d + arr + 4 + 4 * i, 4);
        if (layouts) {
            LayoutCtx c = {.l = {.used = 1, .step_function = 1, .step_rate = 1}};
            if (walk(b, arr + off, layout_field, &c)) return -1;
            if (c.index < PVG_MAX_VERTEX) vd->layouts[c.index] = c.l;
        } else {
            AttrCtx c = {0};
            if (walk(b, arr + off, attribute_field, &c)) return -1;
            if (c.index < PVG_MAX_VERTEX) vd->attributes[c.index] = c.a;
        }
    }
    return 0;
}

static void vertex_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    VdCtx *c = ctx;
    switch (key) {
    case 0: c->bad |= read_elements(b, obj, v, 0, c->vd); break;
    case 1: c->bad |= read_elements(b, obj, v, 1, c->vd); break;
    case 2: c->vd->index_type = (uint32_t)v; break;
    case 3: c->vd->index_buffer = (uint32_t)v; break;
    }
}

static int read_vertex(Buf b, size_t obj, uint64_t offset, PvgVertexDescriptor *vd) {
    VdCtx c = {vd, 0};
    vd->present = 1;
    if (walk(b, obj + (size_t)offset, vertex_field, &c)) return -1;
    return c.bad ? -1 : 0;
}

/* ---- Render pipelines ---- */

typedef struct {
    PvgColorAttachment c;
    uint32_t index;
} ColorCtx;

static void color_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    (void)b, (void)obj;
    ColorCtx *c = ctx;
    uint32_t x = (uint32_t)v;
    switch (key) {
    case 0: c->index = x; break;
    case 1: c->c.pixel_format = x; break;
    case 2: c->c.blending = x; break;
    case 3: c->c.src_rgb = x; break;
    case 4: c->c.dst_rgb = x; break;
    case 5: c->c.rgb_op = x; break;
    case 6: c->c.src_alpha = x; break;
    case 7: c->c.dst_alpha = x; break;
    case 8: c->c.alpha_op = x; break;
    case 9: c->c.write_mask = x; break;
    }
}

static const PvgColorAttachment default_color = {.src_rgb = 1, .src_alpha = 1, .write_mask = 0xf}; /* One, Zero, Add, all channels */

static int read_colors(Buf b, size_t obj, uint64_t offset, PvgColorAttachment *out) {
    size_t arr = obj + (size_t)offset;
    uint32_t count;
    if (arr + 4 > b.len) return -1;
    memcpy(&count, b.d + arr, 4);
    if (count > 256 || arr + 4 + 4ull * count > b.len) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off;
        memcpy(&off, b.d + arr + 4 + 4 * i, 4);
        ColorCtx c = {.c = default_color};
        if (walk(b, arr + off, color_field, &c)) return -1;
        if (c.index < PVG_MAX_COLORS) out[c.index] = c.c;
    }
    return 0;
}

typedef struct {
    PvgRenderPipeline *p;
    int bad;
} RenderCtx;

static void render_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    RenderCtx *c = ctx;
    PvgRenderPipeline *p = c->p;
    uint32_t x = (uint32_t)v;
    switch (key) {
    case 0x00: string_at(b, obj, v, p->label, sizeof p->label); break;
    case 0x01: p->vertex_function = v; break;
    case 0x02: p->fragment_function = v; break;
    case 0x03: c->bad |= read_vertex(b, obj, v, &p->vertex); break;
    case 0x04: p->raster_samples = x; break;
    case 0x05: p->alpha_to_coverage = x; break;
    case 0x06: p->alpha_to_one = x; break;
    case 0x07: p->rasterization = x; break;
    case 0x08: c->bad |= read_colors(b, obj, v, p->colors); break;
    case 0x09: p->depth_format = x; break;
    case 0x0a: p->stencil_format = x; break;
    case 0x0b: p->topology = x; break;
    case 0x0c: p->tess_partition = x; break;
    case 0x0d: p->tess_max_factor = x; break;
    case 0x0e: p->tess_scale = x; break;
    case 0x10: p->tess_index_type = x; break;
    case 0x11: p->tess_step = x; break;
    case 0x12: p->tess_winding = x; break;
    case 0x15: c->bad |= read_mutability(b, obj, v, p->vertex_mutability); break;
    case 0x16: c->bad |= read_mutability(b, obj, v, p->fragment_mutability); break;
    case 0x19: p->indirect = x; break;
    case 0x1a: p->amplification = x; break;
    default: p->unknown_keys |= 1ull << (key & 63); break;
    }
}

int pvg_parse_render_pipeline(const uint8_t *data, size_t len, PvgRenderPipeline *p) {
    memset(p, 0, sizeof *p);
    p->raster_samples = 1, p->rasterization = 1, p->tess_max_factor = 16, p->amplification = 1;
    for (int i = 0; i < PVG_MAX_COLORS; i++) p->colors[i] = default_color;
    RenderCtx c = {p, 0};
    if (!len || walk((Buf){data, len}, 0, render_field, &c)) return -1;
    return c.bad ? -1 : 0;
}

/* ---- Compute pipelines ---- */

typedef struct {
    PvgComputePipeline *p;
    int bad;
} ComputeCtx;

static void compute_field(Buf b, size_t obj, uint32_t key, uint64_t v, void *ctx) {
    ComputeCtx *c = ctx;
    PvgComputePipeline *p = c->p;
    switch (key) {
    case 0: p->function = v; break;
    case 1: p->threadgroup_multiple = (uint32_t)v; break;
    case 2: string_at(b, obj, v, p->label, sizeof p->label); break;
    case 3: c->bad |= read_vertex(b, obj, v, &p->stage_in); break;
    case 6: c->bad |= read_mutability(b, obj, v, p->mutability); break;
    case 7: p->indirect = (uint32_t)v; break;
    case 8: p->max_threads = (uint32_t)v; break;
    default: p->unknown_keys |= 1ull << (key & 63); break;
    }
}

int pvg_parse_compute_pipeline(const uint8_t *data, size_t len, PvgComputePipeline *p) {
    memset(p, 0, sizeof *p);
    ComputeCtx c = {p, 0};
    if (!len || walk((Buf){data, len}, 0, compute_field, &c)) return -1;
    return c.bad ? -1 : 0;
}
