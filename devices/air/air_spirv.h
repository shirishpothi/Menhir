#ifndef MENHIR_AIR_SPIRV_H
#define MENHIR_AIR_SPIRV_H

#include <stddef.h>
#include <stdint.h>

/* One Metal function, as AIR bitcode, to a SPIR-V module for the Vulkan backend: Stela's AIR to NIR compiler
   (devices/air/stela/), then Mesa's NIR to SPIR-V emitter (zink's nir_to_spirv). The bindings follow Stela's ABI
   (devices/air/stela/metal_abi.h): push constants hold each stage's argument table address; buffers are device
   addresses in it, textures and samplers indices into descriptor set STELA_METAL_HEAP_SET's arrays. */

typedef struct {
    uint32_t *words;
    size_t count;
    char error[300];
} AirSpirv;

/* What a function is compiled for. A render pipeline's vertex and fragment functions must agree on their varyings:
   both are compiled with the linkage of link (normally the vertex function); with no link, a function's own varyings.
   A kernel is compiled for one threadgroup size (Metal gives it with each dispatch). */
typedef struct {
    const void *link_bitcode;
    size_t link_size;
    const char *link_entry;
    uint32_t workgroup[3];
    /* The sampler for a constexpr sampler state in Apple's encoding: its MTLResourceID as the backend's queries
       answer it (the low half is its index in the sampler heap). */
    uint64_t (*constexpr_sampler)(void *ctx, uint64_t state);
    void *ctx;
    int no_memory_model; /* the device has no vulkanMemoryModel (MoltenVK): GLSL450 memory model */
    int print_nir; /* dump the NIR to stdout */
} AirSpirvOptions;

/* 0 on success. */
int air_compile_spirv(const void *bitcode, size_t size, const char *entry, const AirSpirvOptions *options, AirSpirv *out);
void air_spirv_free(AirSpirv *s);

#endif
