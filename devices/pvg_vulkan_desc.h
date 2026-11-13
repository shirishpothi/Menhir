#ifndef HENGEVM_PVG_VULKAN_DESC_H
#define HENGEVM_PVG_VULKAN_DESC_H

#include <stddef.h>
#include <stdint.h>

/* Metal's serialized pipeline descriptors, as the guest sends them (docs/android/graphics.md, section 2), read
   without Metal. Values are Metal's enums; fields the data leaves out have Metal's defaults. */

#define PVG_MAX_COLORS 8
#define PVG_MAX_VERTEX 31

typedef struct {
    uint32_t pixel_format, blending, src_rgb, dst_rgb, rgb_op, src_alpha, dst_alpha, alpha_op, write_mask;
} PvgColorAttachment;

typedef struct {
    uint32_t format, offset, buffer; /* format 0: not used */
} PvgVertexAttribute;

typedef struct {
    uint32_t used, stride, step_function, step_rate; /* step function 1 per vertex, 2 per instance, 0 constant */
} PvgVertexLayout;

typedef struct {
    int present;
    PvgVertexAttribute attributes[PVG_MAX_VERTEX];
    PvgVertexLayout layouts[PVG_MAX_VERTEX];
    uint32_t index_type, index_buffer; /* stage input only */
} PvgVertexDescriptor;

typedef struct {
    char label[64];
    uint64_t vertex_function, fragment_function; /* function refs, 0 for none */
    PvgVertexDescriptor vertex;
    uint32_t raster_samples, alpha_to_coverage, alpha_to_one, rasterization, topology;
    PvgColorAttachment colors[PVG_MAX_COLORS];
    uint32_t depth_format, stencil_format;
    uint32_t tess_partition, tess_max_factor, tess_scale, tess_index_type, tess_step, tess_winding;
    uint8_t vertex_mutability[PVG_MAX_VERTEX], fragment_mutability[PVG_MAX_VERTEX];
    uint32_t indirect, amplification;
    uint64_t unknown_keys; /* keys this does not know, one bit each, for reporting */
} PvgRenderPipeline;

typedef struct {
    char label[64];
    uint64_t function;
    uint32_t threadgroup_multiple, indirect, max_threads;
    PvgVertexDescriptor stage_in;
    uint8_t mutability[PVG_MAX_VERTEX];
    uint64_t unknown_keys;
} PvgComputePipeline;

/* 0 if the data was read; -1 if it is malformed. */
int pvg_parse_render_pipeline(const uint8_t *data, size_t len, PvgRenderPipeline *out);
int pvg_parse_compute_pipeline(const uint8_t *data, size_t len, PvgComputePipeline *out);

#endif
