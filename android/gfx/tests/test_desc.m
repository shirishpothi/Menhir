/* Checks devices/pvg_vulkan_desc.c against Metal on the Mac: random render and compute pipeline descriptors are
   serialized by Metal's own serializer (-[_MTLDevice serializeRenderPipelineDescriptor:]) and read back by the
   parser; every field must match the descriptor. Run: make -f android/gfx/replay/Makefile test-desc */

#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>

#include "pvg_vulkan_desc.h"

static int failures;
#define CHECK(what, a, b)                                                                                                   \
    do {                                                                                                                    \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);                                      \
        if (_a != _b) {                                                                                                     \
            if (failures++ < 40) printf("case %d: %s: parsed %llu, Metal %llu\n", c, what, _a, _b);                         \
        }                                                                                                                   \
    } while (0)

static uint32_t pick(const uint32_t *v, int n) { return v[arc4random_uniform((uint32_t)n)]; }

int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        SEL ser = sel_registerName("serializeRenderPipelineDescriptor:"), cser = sel_registerName("serializeComputePipelineDescriptor:");
        static const uint32_t color_formats[] = {MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatR8Unorm,
                                                 MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGB10A2Unorm, MTLPixelFormatRG16Float, MTLPixelFormatR32Float};
        static const uint32_t vertex_formats[] = {MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat4, MTLVertexFormatUChar4Normalized,
                                                  MTLVertexFormatHalf2, MTLVertexFormatUInt, MTLVertexFormatShort2};
        int cases = 2000;
        for (int c = 0; c < cases; c++) {
            MTLRenderPipelineDescriptor *r = [MTLRenderPipelineDescriptor new];
            if (arc4random_uniform(4) == 0) r.label = [NSString stringWithFormat:@"pipe%d", c];
            r.rasterSampleCount = arc4random_uniform(3) ? 1 : 4;
            r.alphaToCoverageEnabled = arc4random_uniform(5) == 0;
            r.alphaToOneEnabled = arc4random_uniform(7) == 0;
            r.rasterizationEnabled = arc4random_uniform(9) != 0;
            r.inputPrimitiveTopology = arc4random_uniform(4);
            for (int i = 0; i < 8; i++) {
                if (arc4random_uniform(i ? 4 : 1) != 0) continue;
                MTLRenderPipelineColorAttachmentDescriptor *a = r.colorAttachments[i];
                a.pixelFormat = pick(color_formats, 8);
                if (arc4random_uniform(2)) {
                    a.blendingEnabled = YES;
                    a.sourceRGBBlendFactor = arc4random_uniform(19), a.destinationRGBBlendFactor = arc4random_uniform(19);
                    a.sourceAlphaBlendFactor = arc4random_uniform(19), a.destinationAlphaBlendFactor = arc4random_uniform(19);
                    a.rgbBlendOperation = arc4random_uniform(5), a.alphaBlendOperation = arc4random_uniform(5);
                }
                if (arc4random_uniform(4) == 0) a.writeMask = arc4random_uniform(16);
            }
            if (arc4random_uniform(3) == 0) r.depthAttachmentPixelFormat = arc4random_uniform(2) ? MTLPixelFormatDepth32Float : MTLPixelFormatDepth16Unorm;
            if (arc4random_uniform(4) == 0) r.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
            int nattr = (int)arc4random_uniform(4);
            for (int i = 0; i < nattr; i++) {
                int k = (int)arc4random_uniform(6), buf = (int)arc4random_uniform(4);
                r.vertexDescriptor.attributes[k].format = pick(vertex_formats, 7);
                r.vertexDescriptor.attributes[k].offset = arc4random_uniform(8) * 4;
                r.vertexDescriptor.attributes[k].bufferIndex = buf;
                r.vertexDescriptor.layouts[buf].stride = 4 + arc4random_uniform(16) * 4;
                if (arc4random_uniform(3) == 0) r.vertexDescriptor.layouts[buf].stepFunction = MTLVertexStepFunctionPerInstance;
                if (arc4random_uniform(4) == 0) r.vertexDescriptor.layouts[buf].stepRate = 1 + arc4random_uniform(3);
            }
            if (arc4random_uniform(5) == 0) r.vertexBuffers[arc4random_uniform(8)].mutability = MTLMutabilityImmutable;
            if (arc4random_uniform(5) == 0) r.fragmentBuffers[arc4random_uniform(8)].mutability = MTLMutabilityMutable;
            if (arc4random_uniform(6) == 0) r.supportIndirectCommandBuffers = YES;
            if (arc4random_uniform(8) == 0) r.maxTessellationFactor = 2 + 2 * arc4random_uniform(8);
            if (arc4random_uniform(8) == 0) r.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
            NSData *data = ((id (*)(id, SEL, id))objc_msgSend)(d, ser, r);
            PvgRenderPipeline p;
            if (pvg_parse_render_pipeline(data.bytes, data.length, &p)) { CHECK("parse", 1, 0); continue; }
            CHECK("unknown keys", p.unknown_keys, 0);
            CHECK("samples", p.raster_samples, r.rasterSampleCount);
            CHECK("alpha to coverage", p.alpha_to_coverage, r.alphaToCoverageEnabled);
            CHECK("alpha to one", p.alpha_to_one, r.alphaToOneEnabled);
            CHECK("rasterization", p.rasterization, r.rasterizationEnabled);
            CHECK("topology", p.topology, r.inputPrimitiveTopology);
            CHECK("depth", p.depth_format, r.depthAttachmentPixelFormat);
            CHECK("stencil", p.stencil_format, r.stencilAttachmentPixelFormat);
            CHECK("indirect", p.indirect, r.supportIndirectCommandBuffers);
            CHECK("max tess", p.tess_max_factor, r.maxTessellationFactor);
            CHECK("winding", p.tess_winding, r.tessellationOutputWindingOrder);
            if (r.label) CHECK("label", strcmp(p.label, r.label.UTF8String), 0);
            for (int i = 0; i < 8; i++) {
                MTLRenderPipelineColorAttachmentDescriptor *a = r.colorAttachments[i];
                PvgColorAttachment *pc = &p.colors[i];
                CHECK("color format", pc->pixel_format, a.pixelFormat);
                CHECK("blending", pc->blending, a.blendingEnabled);
                CHECK("src rgb", pc->src_rgb, a.sourceRGBBlendFactor);
                CHECK("dst rgb", pc->dst_rgb, a.destinationRGBBlendFactor);
                CHECK("src alpha", pc->src_alpha, a.sourceAlphaBlendFactor);
                CHECK("dst alpha", pc->dst_alpha, a.destinationAlphaBlendFactor);
                CHECK("rgb op", pc->rgb_op, a.rgbBlendOperation);
                CHECK("alpha op", pc->alpha_op, a.alphaBlendOperation);
                CHECK("write mask", pc->write_mask, a.writeMask);
            }
            for (int i = 0; i < 8; i++) {
                MTLVertexAttributeDescriptor *a = r.vertexDescriptor.attributes[i];
                CHECK("attr format", p.vertex.attributes[i].format, a.format);
                if (!a.format) continue;
                CHECK("attr offset", p.vertex.attributes[i].offset, a.offset);
                CHECK("attr buffer", p.vertex.attributes[i].buffer, a.bufferIndex);
                MTLVertexBufferLayoutDescriptor *l = r.vertexDescriptor.layouts[a.bufferIndex];
                PvgVertexLayout *pl = &p.vertex.layouts[a.bufferIndex];
                CHECK("layout used", pl->used, 1);
                CHECK("layout stride", pl->stride, l.stride);
                CHECK("layout step", pl->step_function, l.stepFunction);
                CHECK("layout rate", pl->step_rate, l.stepRate);
            }
            for (int i = 0; i < 8; i++) {
                CHECK("vertex mutability", p.vertex_mutability[i], r.vertexBuffers[i].mutability);
                CHECK("fragment mutability", p.fragment_mutability[i], r.fragmentBuffers[i].mutability);
            }
        }
        for (int c = 0; c < 500; c++) {
            MTLComputePipelineDescriptor *k = [MTLComputePipelineDescriptor new];
            if (arc4random_uniform(3) == 0) k.label = @"kernel";
            k.threadGroupSizeIsMultipleOfThreadExecutionWidth = arc4random_uniform(2);
            if (arc4random_uniform(2)) k.maxTotalThreadsPerThreadgroup = 32 << arc4random_uniform(5);
            k.supportIndirectCommandBuffers = arc4random_uniform(4) == 0;
            if (arc4random_uniform(4) == 0) k.buffers[arc4random_uniform(8)].mutability = MTLMutabilityImmutable;
            NSData *data = ((id (*)(id, SEL, id))objc_msgSend)(d, cser, k);
            PvgComputePipeline p;
            if (pvg_parse_compute_pipeline(data.bytes, data.length, &p)) { CHECK("compute parse", 1, 0); continue; }
            CHECK("compute unknown keys", p.unknown_keys, 0);
            CHECK("tg multiple", p.threadgroup_multiple, k.threadGroupSizeIsMultipleOfThreadExecutionWidth);
            CHECK("max threads", p.max_threads, k.maxTotalThreadsPerThreadgroup);
            CHECK("compute indirect", p.indirect, k.supportIndirectCommandBuffers);
            if (k.label) CHECK("compute label", strcmp(p.label, k.label.UTF8String), 0);
            for (int i = 0; i < 8; i++) CHECK("compute mutability", p.mutability[i], k.buffers[i].mutability);
        }
        printf("test_desc: %d render and 500 compute descriptors, %d mismatches\n", cases, failures);
    }
    return failures != 0;
}
