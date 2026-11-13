/* Runs the guest's GPU commands with Vulkan, for hosts without Metal (Android). The same job as pvg_metal.m, with
   the same C interface (pvg_lite.h hv_pvg_metal_*), and the same structure: a worker takes commands in order,
   objects are made the first time a command names them, and a command's stamp is given back when its GPU work is
   done. The formats are in docs/android/graphics.md; the shaders go through devices/air (Stela's AIR to NIR, then
   SPIR-V) and follow its binding ABI (devices/air/stela/metal_abi.h).

   Memory: guest memory is copied, not shared (the tablet's driver has no VK_EXT_external_memory_host). A buffer is
   a host-visible device buffer, filled from the guest's pages when they changed, and copied back after an exec
   that wrote it. A surface is a device image, uploaded when the guest's pages changed and downloaded after an exec
   that drew into it. Each exec is waited for before the next command: correct first, overlap later. */

#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include <vulkan/vulkan.h>

#include "pvg_lite.h"
#include "pvg_vulkan_desc.h"
#include "air/air_library.h"
#include "air/air_spirv.h"

#define PAGE 0x4000u
#define MAX_TASKS 512

/* metal_abi.h, which needs Mesa's headers: the parts the device side needs. */
#define PUSH_BYTES 64u
#define PUSH_TABLES 0u
#define PUSH_GRID 40u
#define TABLE_BUFFERS 0u
#define TABLE_LENGTHS 248u
#define TABLE_TEXTURES 496u
#define TABLE_SAMPLERS 1520u
#define TABLE_SAMPLE_COUNT 1728u
#define TABLE_BLEND_COLOR 1744u
#define TABLE_BYTES 1840u
#define LEVEL_SHIFT 27u
#define SAMPLER_PIXELS 0x10000000u
enum { HEAP_SAMPLED, HEAP_STORAGE, HEAP_SAMPLERS, HEAP_UNIFORM_TEXEL, HEAP_STORAGE_TEXEL, HEAPS };
static const uint32_t heap_size[HEAPS] = {65536, 16384, 4096, 4096, 4096};
enum { TABLE_PRIMARY, TABLE_FRAGMENT, STAGES };

static void complain(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Formats ---- */

typedef struct { uint16_t mtl; VkFormat vk; uint8_t bytes; } Format;
static const Format formats[] = {
    {1, VK_FORMAT_R8_UNORM, 1}, {10, VK_FORMAT_R8_UNORM, 1}, {11, VK_FORMAT_R8_SRGB, 1}, {12, VK_FORMAT_R8_SNORM, 1},
    {13, VK_FORMAT_R8_UINT, 1}, {14, VK_FORMAT_R8_SINT, 1}, {20, VK_FORMAT_R16_UNORM, 2}, {22, VK_FORMAT_R16_SNORM, 2},
    {23, VK_FORMAT_R16_UINT, 2}, {24, VK_FORMAT_R16_SINT, 2}, {25, VK_FORMAT_R16_SFLOAT, 2}, {30, VK_FORMAT_R8G8_UNORM, 2},
    {31, VK_FORMAT_R8G8_SRGB, 2}, {32, VK_FORMAT_R8G8_SNORM, 2}, {33, VK_FORMAT_R8G8_UINT, 2}, {34, VK_FORMAT_R8G8_SINT, 2},
    {40, VK_FORMAT_R5G6B5_UNORM_PACK16, 2}, {41, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2}, {42, VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2},
    {43, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2}, {53, VK_FORMAT_R32_UINT, 4}, {54, VK_FORMAT_R32_SINT, 4}, {55, VK_FORMAT_R32_SFLOAT, 4},
    {60, VK_FORMAT_R16G16_UNORM, 4}, {62, VK_FORMAT_R16G16_SNORM, 4}, {63, VK_FORMAT_R16G16_UINT, 4}, {64, VK_FORMAT_R16G16_SINT, 4},
    {65, VK_FORMAT_R16G16_SFLOAT, 4}, {70, VK_FORMAT_R8G8B8A8_UNORM, 4}, {71, VK_FORMAT_R8G8B8A8_SRGB, 4}, {72, VK_FORMAT_R8G8B8A8_SNORM, 4},
    {73, VK_FORMAT_R8G8B8A8_UINT, 4}, {74, VK_FORMAT_R8G8B8A8_SINT, 4}, {80, VK_FORMAT_B8G8R8A8_UNORM, 4}, {81, VK_FORMAT_B8G8R8A8_SRGB, 4},
    {90, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4}, {91, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4}, {92, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4},
    {93, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4}, {94, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4}, {103, VK_FORMAT_R32G32_UINT, 8},
    {104, VK_FORMAT_R32G32_SINT, 8}, {105, VK_FORMAT_R32G32_SFLOAT, 8}, {110, VK_FORMAT_R16G16B16A16_UNORM, 8}, {112, VK_FORMAT_R16G16B16A16_SNORM, 8},
    {113, VK_FORMAT_R16G16B16A16_UINT, 8}, {114, VK_FORMAT_R16G16B16A16_SINT, 8}, {115, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
    {123, VK_FORMAT_R32G32B32A32_UINT, 16}, {124, VK_FORMAT_R32G32B32A32_SINT, 16}, {125, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
    {250, VK_FORMAT_D16_UNORM, 2}, {252, VK_FORMAT_D32_SFLOAT, 4}, {253, VK_FORMAT_S8_UINT, 1}, {255, VK_FORMAT_D24_UNORM_S8_UINT, 4},
    {260, VK_FORMAT_D32_SFLOAT_S8_UINT, 8}, {130, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0}, {131, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0},
    {132, VK_FORMAT_BC2_UNORM_BLOCK, 0}, {133, VK_FORMAT_BC2_SRGB_BLOCK, 0}, {134, VK_FORMAT_BC3_UNORM_BLOCK, 0},
    {135, VK_FORMAT_BC3_SRGB_BLOCK, 0}, {140, VK_FORMAT_BC4_UNORM_BLOCK, 0}, {142, VK_FORMAT_BC5_UNORM_BLOCK, 0},
    {150, VK_FORMAT_BC6H_SFLOAT_BLOCK, 0}, {152, VK_FORMAT_BC7_UNORM_BLOCK, 0}, {153, VK_FORMAT_BC7_SRGB_BLOCK, 0},
};

static const Format *format_of(uint32_t mtl) {
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++)
        if (formats[i].mtl == mtl) return &formats[i];
    return NULL;
}

static int is_depth_stencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkImageAspectFlags aspects(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* MTLVertexFormat to VkFormat (vertex input). */
static VkFormat vertex_format(uint32_t f) {
    static const VkFormat map[] = {
        [1] = VK_FORMAT_R8G8_UINT, [2] = VK_FORMAT_R8G8B8_UINT, [3] = VK_FORMAT_R8G8B8A8_UINT, [4] = VK_FORMAT_R8G8_SINT,
        [5] = VK_FORMAT_R8G8B8_SINT, [6] = VK_FORMAT_R8G8B8A8_SINT, [7] = VK_FORMAT_R8G8_UNORM, [8] = VK_FORMAT_R8G8B8_UNORM,
        [9] = VK_FORMAT_R8G8B8A8_UNORM, [10] = VK_FORMAT_R8G8_SNORM, [11] = VK_FORMAT_R8G8B8_SNORM, [12] = VK_FORMAT_R8G8B8A8_SNORM,
        [13] = VK_FORMAT_R16G16_UINT, [14] = VK_FORMAT_R16G16B16_UINT, [15] = VK_FORMAT_R16G16B16A16_UINT, [16] = VK_FORMAT_R16G16_SINT,
        [17] = VK_FORMAT_R16G16B16_SINT, [18] = VK_FORMAT_R16G16B16A16_SINT, [19] = VK_FORMAT_R16G16_UNORM, [20] = VK_FORMAT_R16G16B16_UNORM,
        [21] = VK_FORMAT_R16G16B16A16_UNORM, [22] = VK_FORMAT_R16G16_SNORM, [23] = VK_FORMAT_R16G16B16_SNORM, [24] = VK_FORMAT_R16G16B16A16_SNORM,
        [25] = VK_FORMAT_R16G16_SFLOAT, [26] = VK_FORMAT_R16G16B16_SFLOAT, [27] = VK_FORMAT_R16G16B16A16_SFLOAT, [28] = VK_FORMAT_R32_SFLOAT,
        [29] = VK_FORMAT_R32G32_SFLOAT, [30] = VK_FORMAT_R32G32B32_SFLOAT, [31] = VK_FORMAT_R32G32B32A32_SFLOAT, [32] = VK_FORMAT_R32_SINT,
        [33] = VK_FORMAT_R32G32_SINT, [34] = VK_FORMAT_R32G32B32_SINT, [35] = VK_FORMAT_R32G32B32A32_SINT, [36] = VK_FORMAT_R32_UINT,
        [37] = VK_FORMAT_R32G32_UINT, [38] = VK_FORMAT_R32G32B32_UINT, [39] = VK_FORMAT_R32G32B32A32_UINT, [40] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
        [41] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, [42] = VK_FORMAT_B8G8R8A8_UNORM, [45] = VK_FORMAT_R8_UINT, [46] = VK_FORMAT_R8_SINT,
        [47] = VK_FORMAT_R8_UNORM, [48] = VK_FORMAT_R8_SNORM, [49] = VK_FORMAT_R16_UINT, [50] = VK_FORMAT_R16_SINT,
        [51] = VK_FORMAT_R16_UNORM, [52] = VK_FORMAT_R16_SNORM, [53] = VK_FORMAT_R16_SFLOAT, [54] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [55] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    };
    return f < sizeof map / sizeof *map ? map[f] : VK_FORMAT_UNDEFINED;
}

static VkBlendFactor blend_factor(uint32_t f) {
    static const VkBlendFactor map[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA};
    return f < sizeof map / sizeof *map ? map[f] : VK_BLEND_FACTOR_ONE;
}

static VkBlendOp blend_op(uint32_t o) {
    static const VkBlendOp map[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return o < 5 ? map[o] : VK_BLEND_OP_ADD;
}

static VkCompareOp compare_op(uint32_t c) { return (VkCompareOp)(c & 7); } /* MTLCompareFunction has Vulkan's order */

static VkStencilOp stencil_op(uint32_t o) {
    static const VkStencilOp map[] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                      VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                      VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return map[o & 7];
}

/* ---- Objects ---- */

enum { OBJ_BUFFER = 1, OBJ_TEXTURE = 2, OBJ_FUNCTION = 6, OBJ_SERIALIZED = 7, OBJ_LINEAR_TEXTURE = 8, OBJ_MAPPED_TEXTURE = 0xb };
enum { K_NONE, K_BUFFER, K_IMAGE, K_SAMPLER, K_DEPTH_STENCIL, K_RENDER, K_COMPUTE, K_FUNCTION, K_FENCE };

typedef struct Surface Surface;
typedef struct Obj Obj;

typedef struct {
    uint32_t flags, front[3], back[3];
} DepthStencil;

typedef struct {
    uint32_t key[4]; /* workgroup size, threadgroup bytes of index 0 */
    VkPipeline pipeline;
} ComputeVariant;

struct Obj {
    uint32_t entry[3];
    int kind;
    uint64_t id; /* unique, for query 460 */
    /* A buffer: its guest memory (task pages from vpage) and a device copy. A heap is a buffer too, and a buffer in a
       heap is a range of its heap's. */
    uint32_t vpage;
    uint64_t length, offset;
    VkBuffer buffer;
    VkDeviceMemory memory, texture_memory;
    uint8_t *map;
    VkDeviceAddress address;
    uint32_t guest_crc;
    uint8_t *tex_layout;
    uint32_t tex_mips, tex_slices;
    uint64_t tex_base, tex_stride;
    int guest_backed;
    Obj *parent; /* heap of a heap buffer, buffer of a linear texture, texture of a view */
    /* An image (texture, view, surface texture). */
    VkImage image;
    VkImageView view, *level_views;
    VkFormat format;
    uint32_t mtl_format, width, height, depth, levels, layers, samples, type, usage;
    uint32_t sampled_index, storage_index;
    int owns_image;
    Surface *surface;
    uint64_t linear_offset, linear_row; /* over a buffer */
    /* A sampler, its heap index; a depth-stencil state. */
    VkSampler sampler;
    uint32_t sampler_index;
    uint64_t sampler_id; /* MTLResourceID as shaders read it (metal_abi.h): index, and in the high half its twin and flags */
    DepthStencil ds;
    /* A function: its library. */
    uint8_t *library;
    size_t library_len;
    AirFunction fn;
    /* Pipelines. */
    PvgRenderPipeline rp;
    PvgComputePipeline cp;
    VkPipeline render[3]; /* by topology class: points, lines, triangles */
    int render_failed;
    ComputeVariant compute[8];
    int ncompute;
};

struct Surface {
    uint32_t mapping, fourcc, width, height, first;
    uint64_t row, alloc;
    uint32_t *pages; /* physical page numbers (16 KB), one per page of the allocation from its first byte */
    uint32_t npages;
    VkImage image;
    VkDeviceMemory memory;
    VkFormat format;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    uint8_t *staging_map;
    uint32_t guest_crc;
    int drawn; /* the GPU wrote it in the exec running now */
    uint32_t sampled_index;
};

typedef struct {
    Obj **refs;
    uint32_t n;
} Task;

/* ---- The backend ---- */

typedef struct Cmd {
    struct Cmd *next;
    uint32_t channel, len;
    uint32_t *words;
    int kind; /* 0 command, 1 unmap, 2 cursor moved, 3 set layer */
    uint32_t mapping;
    void *layer;
} Cmd;

typedef struct HvPvgMetal {
    HvPvgLite *lite;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv, idle_cv;
    Cmd *head, *tail;
    int stopping, busy;

    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties memory_props;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet heap;
    VkPipelineLayout layout;
    VkPipelineCache cache;
    uint32_t next_index[HEAPS], heap_capacity[HEAPS];
    struct { uint64_t state; Obj *object; } constexpr_samplers[256];
    unsigned nconstexpr;
    uint32_t *free_index[HEAPS], nfree[HEAPS];
    VkSampler default_sampler;

    /* Argument tables, uploaded per draw or dispatch into a ring. */
    uint8_t table[STAGES][TABLE_BYTES];
    int table_dirty[STAGES];
    Obj *stage_buffers[STAGES][31];
    Obj *vertex_buffers[31];
    uint64_t vertex_offsets[31];
    VkBuffer ring;
    VkDeviceMemory ring_memory;
    uint8_t *ring_map;
    VkDeviceAddress ring_address;
    uint64_t ring_used, ring_size;

    Task tasks[MAX_TASKS];
    Surface **surfaces;
    uint32_t nsurfaces;
    uint64_t next_id;

    /* The display. */
    uint64_t on_screen;
    uint8_t *cursor;
    uint32_t cursor_w, cursor_h, cursor_stride;
    int cursor_hot_x, cursor_hot_y, cursor_shown;
    void *window; /* ANativeWindow * */
    _Atomic uint64_t gpu_frames;

    uint64_t execs, draws, dispatches, failed_draws;
    int log;
} Backend;

static int complaints;
static void complain(const char *fmt, ...) {
    static char seen[64][160];
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    for (int i = 0; i < complaints && i < 64; i++)
        if (!strcmp(seen[i], line)) return;
    if (complaints < 64) strcpy(seen[complaints], line);
    complaints++;
    fprintf(stderr, "hengevm: pvg-vulkan: %s\n", line);
}

/* ---- Memory ---- */

static int memory_type(Backend *b, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < b->memory_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (b->memory_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* A host-visible buffer with a device address. */
static int make_buffer(Backend *b, uint64_t size, VkBufferUsageFlags extra, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map, VkDeviceAddress *address) {
    if (!size) size = 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra};
    if (vkCreateBuffer(b->device, &bi, NULL, buf)) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(b->device, *buf, &req);
    int t = memory_type(b, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = memory_type(b, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vkAllocateMemory(b->device, &ai, NULL, mem)) {
        vkDestroyBuffer(b->device, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return -1;
    }
    vkBindBufferMemory(b->device, *buf, *mem, 0);
    vkMapMemory(b->device, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map);
    VkBufferDeviceAddressInfo dai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = *buf};
    if (address) *address = vkGetBufferDeviceAddress(b->device, &dai);
    return 0;
}

static int make_image(Backend *b, Obj *o, VkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers, uint32_t samples, int cube) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(b->physical, o->format, &fp);
    VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples <= 1) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                            .imageType = type, .format = o->format, .extent = {w ? w : 1, h ? h : 1, d ? d : 1}, .mipLevels = levels ? levels : 1,
                            .arrayLayers = layers ? layers : 1, .samples = (VkSampleCountFlagBits)(samples ? samples : 1),
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(b->device, &ii, NULL, &o->image)) return -1;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(b->device, o->image, &req);
    int t = memory_type(b, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vkAllocateMemory(b->device, &ai, NULL, &o->memory)) {
        vkDestroyImage(b->device, o->image, NULL);
        o->image = VK_NULL_HANDLE;
        return -1;
    }
    vkBindImageMemory(b->device, o->image, o->memory, 0);
    o->owns_image = 1;
    o->usage = usage;
    return 0;
}

/* Every image lives in GENERAL: one layout for sampling, storage, attachments and copies. */
static void to_general(Backend *b, VkCommandBuffer cb, VkImage image, VkFormat format) {
    (void)b;
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image, .subresourceRange = {aspects(format), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* A one-off command buffer, run to completion. */
static VkCommandBuffer begin_once(Backend *b) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = b->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(b->device, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static VkResult end_once(Backend *b, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vkResetFences(b->device, 1, &b->fence);
    VkResult r = vkQueueSubmit(b->queue, 1, &si, b->fence);
    if (!r) r = vkWaitForFences(b->device, 1, &b->fence, VK_TRUE, 10ull * 1000000000ull);
    vkFreeCommandBuffers(b->device, b->pool, 1, &cb);
    if (r) complain("GPU work failed: %d", r);
    return r;
}

/* ---- The descriptor heap ---- */

static uint32_t heap_alloc(Backend *b, int heap) {
    if (b->nfree[heap]) return b->free_index[heap][--b->nfree[heap]];
    if (b->next_index[heap] >= b->heap_capacity[heap]) { complain("descriptor heap %d full", heap); return 0; }
    return b->next_index[heap]++;
}

static void heap_free(Backend *b, int heap, uint32_t i) {
    if (!i) return;
    b->free_index[heap] = realloc(b->free_index[heap], (b->nfree[heap] + 1) * sizeof(uint32_t));
    b->free_index[heap][b->nfree[heap]++] = i;
}

static void write_image_descriptor(Backend *b, int heap, uint32_t index, VkImageView view) {
    VkDescriptorImageInfo ii = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->heap, .dstBinding = (uint32_t)heap, .dstArrayElement = index,
                              .descriptorCount = 1, .descriptorType = heap == HEAP_SAMPLED ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                              .pImageInfo = &ii};
    vkUpdateDescriptorSets(b->device, 1, &w, 0, NULL);
}

/* ---- Guest memory ---- */

/* n bytes of task memory from a task page on, to or from host memory. */
static void task_copy(Backend *b, uint32_t task, uint32_t vpage, uint64_t length, uint8_t *host, int to_guest) {
    uint64_t n = (length + PAGE - 1) / PAGE;
    uint32_t stack[256], *pages = n <= 256 ? stack : malloc(n * 4);
    hv_pvg_lite_task_pages(b->lite, task, vpage, n, pages);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = length - i * PAGE < PAGE ? length - i * PAGE : PAGE;
        uint8_t *g = pages[i] ? hv_pvg_lite_page(b->lite, pages[i]) : NULL;
        if (!g) continue;
        if (to_guest) memcpy(g, host + i * PAGE, c);
        else memcpy(host + i * PAGE, g, c);
    }
    if (pages != stack) free(pages);
}

static uint32_t task_crc(Backend *b, uint32_t task, uint32_t vpage, uint64_t length) {
    uint64_t n = (length + PAGE - 1) / PAGE;
    uint32_t stack[256], *pages = n <= 256 ? stack : malloc(n * 4);
    hv_pvg_lite_task_pages(b->lite, task, vpage, n, pages);
    uLong c = crc32(0, NULL, 0);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t len = length - i * PAGE < PAGE ? length - i * PAGE : PAGE;
        uint8_t *g = pages[i] ? hv_pvg_lite_page(b->lite, pages[i]) : NULL;
        c = g ? crc32(c, g, (uInt)len) : crc32(c, (const Bytef *)&pages[i], 4);
    }
    if (pages != stack) free(pages);
    return (uint32_t)c | 1;
}

static uint8_t *task_ptr(Backend *b, uint32_t task, uint64_t offset, uint32_t want) {
    uint32_t page = hv_pvg_lite_task_page(b->lite, task, offset);
    uint8_t *p = page ? hv_pvg_lite_page(b->lite, page) : NULL;
    if (!p || (offset & (PAGE - 1)) + want > PAGE) return NULL;
    return p + (offset & (PAGE - 1));
}

/* ---- Surfaces ---- */

static uint32_t surface_crc(Backend *b, Surface *s) {
    uLong c = crc32(0, NULL, 0);
    for (uint32_t i = 0; i < s->npages; i++) {
        uint8_t *g = s->pages[i] ? hv_pvg_lite_phys(b->lite, (uint64_t)s->pages[i] << 14, PAGE) : NULL;
        if (g) c = crc32(c, g, PAGE);
    }
    return (uint32_t)c | 1;
}

/* Rows between the surface's guest pages and its staging buffer (packed rows of width * bpp). */
static void surface_copy(Backend *b, Surface *s, int to_guest) {
    uint32_t bpp = s->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    uint64_t packed = (uint64_t)s->width * bpp;
    for (uint32_t y = 0; y < s->height; y++)
        for (uint64_t x = 0; x < packed;) {
            uint64_t at = s->first + y * s->row + x, in = at % PAGE;
            uint64_t c = PAGE - in < packed - x ? PAGE - in : packed - x;
            uint32_t pi = (uint32_t)(at / PAGE);
            uint8_t *g = pi < s->npages && s->pages[pi] ? hv_pvg_lite_phys(b->lite, ((uint64_t)s->pages[pi] << 14) + in, c) : NULL;
            uint8_t *h = s->staging_map + y * packed + x;
            if (g) {
                if (to_guest) memcpy(g, h, c);
                else memcpy(h, g, c);
            }
            x += c;
        }
}

static Surface *surface(Backend *b, uint32_t mapping) {
    if (mapping < b->nsurfaces && b->surfaces[mapping]) return b->surfaces[mapping];
    const uint8_t *desc = hv_pvg_lite_surface(b->lite, mapping);
    if (!desc) { complain("surface %u has no descriptor", mapping); return NULL; }
    if (hv_pvg_lite_old_driver(b->lite)) { complain("macOS 14's surface descriptions are not read yet"); return NULL; }
    Surface *s = calloc(1, sizeof *s);
    uint32_t list;
    uint64_t w, h;
    memcpy(&list, desc, 4), memcpy(&s->fourcc, desc + 4, 4), memcpy(&s->alloc, desc + 0x18, 8), memcpy(&w, desc + 0x20, 8);
    memcpy(&h, desc + 0x28, 8), memcpy(&s->row, desc + 0x30, 8), memcpy(&s->first, desc + 0x60, 4);
    s->mapping = mapping, s->width = (uint32_t)w, s->height = (uint32_t)h;
    if (desc[0x3e] >= 2) complain("multi-plane surfaces (video) are not drawn yet");
    s->format = s->fourcc == 0x52476841 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM; /* 'RGhA', else BGRA */
    s->npages = (uint32_t)((s->first + s->alloc + PAGE - 1) / PAGE);
    const uint32_t *pages = (list & 1) ? hv_pvg_lite_phys(b->lite, (uint64_t)(list & ~1u) << 12, s->npages * 4ull) : NULL;
    if (!pages || !w || !h || w > 16384 || h > 16384) { free(s); complain("surface %u: no pages or size", mapping); return NULL; }
    s->pages = malloc(s->npages * 4);
    for (uint32_t i = 0; i < s->npages; i++) s->pages[i] = (pages[i] & 1) ? (pages[i] & ~1u) >> 2 : 0; /* >> 12 << 12 >> 14 */
    Obj tmp = {.format = s->format};
    if (make_image(b, &tmp, VK_IMAGE_TYPE_2D, s->width, s->height, 1, 1, 1, 1, 0)) { complain("surface %u: no image", mapping); free(s->pages), free(s); return NULL; }
    s->image = tmp.image, s->memory = tmp.memory;
    uint32_t bpp = s->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    make_buffer(b, (uint64_t)s->width * s->height * bpp, 0, &s->staging, &s->staging_memory, &s->staging_map, NULL);
    VkCommandBuffer cb = begin_once(b);
    to_general(b, cb, s->image, s->format);
    end_once(b, cb);
    if (mapping >= b->nsurfaces) {
        uint32_t n = mapping + 64;
        b->surfaces = realloc(b->surfaces, n * sizeof *b->surfaces);
        memset(b->surfaces + b->nsurfaces, 0, (n - b->nsurfaces) * sizeof *b->surfaces);
        b->nsurfaces = n;
    }
    b->surfaces[mapping] = s;
    fprintf(stderr, "hengevm: pvg-vulkan: surface %u: %ux%u, row %llu, fourcc %c%c%c%c\n", mapping, s->width, s->height,
            (unsigned long long)s->row, desc[7], desc[6], desc[5], desc[4]);
    return s;
}

static void surface_upload(Backend *b, VkCommandBuffer cb, Surface *s) {
    uint32_t c = surface_crc(b, s);
    if (c == s->guest_crc) return;
    s->guest_crc = c;
    surface_copy(b, s, 0);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {s->width, s->height, 1}};
    vkCmdCopyBufferToImage(cb, s->staging, s->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void surface_download_record(VkCommandBuffer cb, Surface *s) {
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {s->width, s->height, 1}};
    vkCmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, s->staging, 1, &r);
}

static void surface_free(Backend *b, Surface *s) {
    vkDestroyImage(b->device, s->image, NULL);
    vkFreeMemory(b->device, s->memory, NULL);
    vkDestroyBuffer(b->device, s->staging, NULL);
    vkFreeMemory(b->device, s->staging_memory, NULL);
    free(s->pages);
    free(s);
}

/* ---- Making objects ---- */

static Obj *object(Backend *b, uint32_t ref, uint32_t task);
static void install_sampler(Backend *b, Obj *o, const VkSamplerCreateInfo *si, int pixels);

/* The serialized texture descriptions (pvg_metal.m descriptor_v1, descriptor_v2). */
typedef struct { uint32_t type, usage, format, w, h, d, levels, samples, layers; uint8_t swizzle[4]; } TexDesc;
static void tex_v1(const uint8_t *d, TexDesc *t) {
    t->type = d[0] & 0xf, t->usage = d[1], t->format = d[2] | d[3] << 8;
    memcpy(&t->w, d + 4, 4), memcpy(&t->h, d + 8, 4), memcpy(&t->d, d + 12, 4);
    uint16_t m, s, a;
    memcpy(&m, d + 16, 2), memcpy(&s, d + 18, 2), memcpy(&a, d + 20, 2);
    t->levels = m, t->samples = s, t->layers = a;
    memset(t->swizzle, 0, 4);
}
static void tex_v2(const uint8_t *d, TexDesc *t) {
    t->type = d[0] & 0xf, t->format = d[1] | d[2] << 8;
    memcpy(&t->usage, d + 3, 4), memcpy(&t->w, d + 7, 4), memcpy(&t->h, d + 11, 4), memcpy(&t->d, d + 15, 4);
    uint16_t m, s, a;
    memcpy(&m, d + 19, 2), memcpy(&s, d + 21, 2), memcpy(&a, d + 23, 2);
    t->levels = m, t->samples = s, t->layers = a;
    memcpy(t->swizzle, d + 0x23, 4);
}

/* MTLTextureType: 0 1D, 1 1DArray, 2 2D, 3 2DArray, 4 2DMultisample, 5 Cube, 6 CubeArray, 7 3D, 8 2DMSArray, 9 buffer. */
static VkImageViewType view_type(uint32_t t) {
    switch (t) {
    case 0: return VK_IMAGE_VIEW_TYPE_1D;
    case 1: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case 3: case 8: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case 5: return VK_IMAGE_VIEW_TYPE_CUBE;
    case 6: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case 7: return VK_IMAGE_VIEW_TYPE_3D;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static VkComponentSwizzle swizzle(uint8_t s, VkComponentSwizzle identity) {
    switch (s) { /* MTLTextureSwizzle: 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha */
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 2: return VK_COMPONENT_SWIZZLE_R;
    case 3: return VK_COMPONENT_SWIZZLE_G;
    case 4: return VK_COMPONENT_SWIZZLE_B;
    case 5: return VK_COMPONENT_SWIZZLE_A;
    default: return identity;
    }
}

/* The views a texture's resource ID names: a sampled one, and a storage one per level where the format allows. */
static void make_views(Backend *b, Obj *o, VkImage image, VkFormat format, VkImageViewType type, uint32_t base_level, uint32_t levels,
                       uint32_t base_layer, uint32_t layers, const uint8_t *sw) {
    VkImageViewUsageCreateInfo uci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = o->usage & ~VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &uci, .image = image, .viewType = type, .format = format,
                                .subresourceRange = {aspects(format), base_level, levels ? levels : 1, base_layer, layers ? layers : 1}};
    if (aspects(format) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (o->mtl_format == 1) vi.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    if (sw && (sw[0] | sw[1] | sw[2] | sw[3]) && !is_depth_stencil(format))
        vi.components = (VkComponentMapping){swizzle(sw[0], 0), swizzle(sw[1], 0), swizzle(sw[2], 0), swizzle(sw[3], 0)};
    if (vkCreateImageView(b->device, &vi, NULL, &o->view)) { complain("image view of format %d failed", format); return; }
    if (o->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        o->sampled_index = heap_alloc(b, HEAP_SAMPLED);
        write_image_descriptor(b, HEAP_SAMPLED, o->sampled_index, o->view);
    }
    if ((o->usage & VK_IMAGE_USAGE_STORAGE_BIT) && type != VK_IMAGE_VIEW_TYPE_CUBE && type != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        uint32_t n = levels ? levels : 1;
        o->level_views = calloc(n, sizeof *o->level_views);
        uci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        for (uint32_t l = 0; l < n; l++) {
            uint32_t idx = heap_alloc(b, HEAP_STORAGE);
            if (l == 0) o->storage_index = idx;
            else if (idx != o->storage_index + l) { complain("storage descriptors not contiguous"); break; }
            VkImageViewCreateInfo si = vi;
            si.components = (VkComponentMapping){0};
            si.subresourceRange.baseMipLevel = base_level + l, si.subresourceRange.levelCount = 1;
            if (!vkCreateImageView(b->device, &si, NULL, &o->level_views[l])) write_image_descriptor(b, HEAP_STORAGE, idx, o->level_views[l]);
        }
    }
}

static uint64_t texture_id(Obj *o) {
    uint32_t levels = o->levels ? o->levels : 1;
    return (uint64_t)o->sampled_index | (uint64_t)(o->storage_index | (levels - 1) << LEVEL_SHIFT) << 32;
}

static void make_texture(Backend *b, Obj *o, const TexDesc *t) {
    const Format *f = format_of(t->format);
    if (!f) { complain("pixel format %u not drawn", t->format); return; }
    o->format = f->vk, o->mtl_format = t->format;
    o->width = t->w, o->height = t->h, o->depth = t->d ? t->d : 1, o->levels = t->levels ? t->levels : 1, o->type = t->type;
    o->layers = (t->layers ? t->layers : 1) * (t->type == 5 || t->type == 6 ? 6 : 1);
    o->samples = t->samples ? t->samples : 1;
    VkImageType it = t->type == 7 ? VK_IMAGE_TYPE_3D : t->type <= 1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    if (make_image(b, o, it, o->width, o->height, it == VK_IMAGE_TYPE_3D ? o->depth : 1, o->levels, o->layers, o->samples, t->type == 5 || t->type == 6)) {
        complain("texture %ux%u format %u failed", t->w, t->h, t->format);
        return;
    }
    VkCommandBuffer cb = begin_once(b);
    to_general(b, cb, o->image, o->format);
    end_once(b, cb);
    make_views(b, o, o->image, o->format, view_type(t->type), 0, o->levels, 0, o->layers, t->swizzle);
}

static void make_buffer_object(Backend *b, Obj *o, uint64_t at, uint32_t task) {
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage; uint32_t flags; } h;
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, sizeof h)) return;
    o->length = h.length, o->vpage = h.vpage, o->guest_backed = 1;
    if (make_buffer(b, (h.length + PAGE - 1) & ~(uint64_t)(PAGE - 1), 0, &o->buffer, &o->memory, &o->map, &o->address)) {
        complain("buffer of %llu bytes failed", (unsigned long long)h.length);
        return;
    }
    if (o->guest_backed && o->map) {
        task_copy(b, task, o->vpage, o->length, o->map, 0);
        o->guest_crc = task_crc(b, task, o->vpage, o->length);
    }
    o->kind = K_BUFFER;
}

static void make_heap(Backend *b, Obj *o, uint64_t at, uint32_t task) {
    struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } h;
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, sizeof h)) return;
    o->length = (h.length + PAGE - 1) & ~(uint64_t)(PAGE - 1), o->vpage = h.vpage, o->guest_backed = h.vpage != 0;
    if (make_buffer(b, o->length, 0, &o->buffer, &o->memory, &o->map, &o->address)) { complain("heap of %llu bytes failed", (unsigned long long)h.length); return; }
    if (o->guest_backed && o->map) {
        task_copy(b, task, o->vpage, o->length, o->map, 0);
        o->guest_crc = task_crc(b, task, o->vpage, o->length);
    }
    o->kind = K_BUFFER;
}

static void make_heap_buffer(Backend *b, Obj *o, uint64_t at, uint32_t task) {
    struct __attribute__((packed)) { uint32_t heap; uint64_t offset, length; } h;
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, sizeof h)) return;
    Obj *heap = object(b, h.heap, task);
    if (!heap || heap->kind != K_BUFFER || h.offset + h.length > heap->length) { complain("heap buffer outside its heap"); return; }
    o->parent = heap, o->buffer = heap->buffer, o->offset = h.offset, o->length = h.length;
    o->map = heap->map + h.offset, o->address = heap->address + h.offset;
    if (o->guest_backed && o->map) {
        task_copy(b, task, o->vpage, o->length, o->map, 0);
        o->guest_crc = task_crc(b, task, o->vpage, o->length);
    }
    o->kind = K_BUFFER;
}

static void make_texture_entry(Backend *b, Obj *o, uint64_t at, uint32_t size, uint32_t task) {
    uint8_t *d = malloc(size);
    if (hv_pvg_lite_task_read(b->lite, task, at, d, size)) { free(d); return; }
    uint32_t mips = *(uint16_t *)(d + 12) & 0x3fff, table = mips * 36;
    if (12 + 0x18 + table + 12 > size) { free(d); return; }
    const uint8_t *rec = d + 12 + 0x18 + table;
    uint32_t tag;
    memcpy(&tag, rec, 4);
    TexDesc t;
    if (tag == 0x34 || tag == 0x39) tex_v2(rec + 12, &t);
    else tex_v1(rec + 12, &t);
    make_texture(b, o, &t);
    if (o->image) o->kind = K_IMAGE;
    memcpy(&o->length, d, 8);
    memcpy(&o->vpage, d + 8, 4);
    o->tex_mips = mips;
    o->tex_slices = *(uint16_t *)(d + 14);
    memcpy(&o->tex_base, d + 16, 8);
    memcpy(&o->tex_stride, d + 24, 8);
    o->tex_layout = malloc(table);
    memcpy(o->tex_layout, d + 36, table);
    o->guest_backed = 1;
    o->texture_memory = o->memory;
    o->memory = VK_NULL_HANDLE;
    if (make_buffer(b, o->length, 0, &o->buffer, &o->memory, &o->map, &o->address))
        complain("texture staging allocation failed");
    free(d);
}

static void make_linear_texture(Backend *b, Obj *o, uint64_t at, uint32_t size, uint32_t task) {
    struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, size < sizeof h ? size : (uint32_t)sizeof h)) return;
    TexDesc t;
    if (h.tag == 0x34 || h.tag == 0x14 || h.tag == 1) { /* private */
        const uint8_t *desc = (const uint8_t *)&h.buffer;
        if (h.tag == 0x34) tex_v2(desc, &t);
        else tex_v1(desc, &t);
        make_texture(b, o, &t);
    } else if (h.tag == 0x15 || h.tag == 0x38) { /* in a heap: made as its own image (no aliasing) */
        const uint8_t *desc = (const uint8_t *)&h.offset;
        if (h.tag == 0x38) tex_v2(desc, &t);
        else tex_v1(desc, &t);
        make_texture(b, o, &t);
    } else if (h.tag == 7 || h.tag == 8 || h.tag == 0x1b) { /* a view */
        struct __attribute__((packed)) { uint16_t format, type; uint64_t levels[2], slices[2]; uint8_t swizzle[4]; } v;
        memcpy(&v, &h.offset, sizeof v);
        Obj *p = object(b, h.buffer, task);
        if (!p || p->kind != K_IMAGE) { complain("view of nothing (tag %u)", h.tag); return; }
        const Format *f = format_of(v.format);
        if (!f) { complain("view format %u not drawn", v.format); return; }
        o->parent = p, o->surface = p->surface, o->image = p->image, o->format = f->vk, o->mtl_format = v.format, o->usage = p->usage;
        o->width = p->width, o->height = p->height, o->depth = p->depth, o->samples = p->samples;
        uint32_t base_level = 0, base_layer = 0;
        o->levels = p->levels, o->layers = p->layers, o->type = p->type;
        if (h.tag != 7) {
            base_level = (uint32_t)v.levels[0], o->levels = (uint32_t)v.levels[1];
            base_layer = (uint32_t)v.slices[0], o->layers = (uint32_t)v.slices[1], o->type = v.type;
        }
        make_views(b, o, o->image, o->format, view_type(o->type), base_level, o->levels, base_layer, o->layers, h.tag == 0x1b ? v.swizzle : NULL);
    } else if (h.tag == 9 || h.tag == 0x37) { /* over a buffer: its own image, copied from the buffer when used */
        Obj *buf = object(b, h.buffer, task);
        if (h.tag == 0x37) tex_v2(h.d, &t);
        else tex_v1(h.d, &t);
        make_texture(b, o, &t);
        o->parent = buf, o->linear_offset = h.offset, o->linear_row = h.row;
        if (buf && buf->map && o->image) {
            const Format *f = format_of(o->mtl_format);
            if (f && f->bytes && h.offset + h.row * (o->height - 1) + (uint64_t)o->width * f->bytes <= buf->length) {
                VkCommandBuffer cb = begin_once(b);
                VkBufferImageCopy copy = {.bufferOffset = buf->offset + h.offset,
                    .bufferRowLength = (uint32_t)(h.row / f->bytes),
                    .imageSubresource = {aspects(o->format), 0, 0, 1}, .imageExtent = {o->width, o->height, 1}};
                vkCmdCopyBufferToImage(cb, buf->buffer, o->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                end_once(b, cb);
            }
        }
    } else {
        complain("serialized texture tag %u", h.tag);
        return;
    }
    if (o->image) o->kind = K_IMAGE;
}

static void make_mapped_texture(Backend *b, Obj *o, uint64_t at, uint32_t task) {
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, 20 + 32)) return;
    if (h.size > 32) hv_pvg_lite_task_read(b->lite, task, at, &h, 20 + (h.size < 64 ? h.size : 64));
    Surface *s = surface(b, (uint32_t)h.mapping);
    if (!s) return;
    TexDesc t;
    if (h.tag == 0x39) tex_v2(h.d, &t);
    else tex_v1(h.d, &t);
    uint16_t plane;
    memcpy(&plane, h.d + (h.tag == 0x39 ? 0x28 : 0x20), 2);
    if (plane) { complain("textures on a surface's second plane (video) are not drawn"); return; }
    const Format *f = format_of(t.format);
    if (!f) { complain("surface view format %u not drawn", t.format); return; }
    if (f->bytes != (s->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4)) {
        complain("surface %u viewed as format %u of another size", s->mapping, t.format);
        return;
    }
    o->surface = s, o->image = s->image, o->format = f->vk, o->mtl_format = t.format;
    o->width = s->width, o->height = s->height, o->depth = 1, o->levels = 1, o->layers = 1, o->samples = 1, o->type = 2;
    o->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(b->physical, s->format, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) o->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    make_views(b, o, o->image, o->format, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, NULL);
    o->kind = K_IMAGE;
}

static void make_sampler(Backend *b, Obj *o, const uint32_t *w) {
    uint64_t bits;
    memcpy(&bits, w + 3, 8);
    float lod[2];
    memcpy(lod, w + 5, 8);
    static const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static const VkBorderColor borders[] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    uint32_t aniso = (bits >> 26) & 31, cmp = (bits >> 5) & 7;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .minFilter = (bits & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .magFilter = ((bits >> 2) & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = ((bits >> 24) & 3) == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = modes[((bits >> 8) & 15) % 6], .addressModeV = modes[((bits >> 12) & 15) % 6],
                              .addressModeW = modes[((bits >> 16) & 15) % 6], .anisotropyEnable = aniso > 1, .maxAnisotropy = aniso > 1 ? (float)aniso : 1,
                              .compareEnable = cmp != 0 && cmp != 7, .compareOp = compare_op(cmp), .minLod = lod[0], .maxLod = lod[1],
                              .borderColor = borders[((bits >> 20) & 15) % 3]};
    if (((bits >> 24) & 3) == 0) si.maxLod = si.minLod + 0.25f; /* MTLSamplerMipFilterNotMipmapped: the base level only */
    install_sampler(b, o, &si, !((bits >> 31) & 1));
}

static float half_bits(uint32_t h) {
    uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = !e ? m * powf(2, -24) : e == 31 ? INFINITY : ldexpf(1 + m / 1024.0f, (int)e - 15);
    return h & 0x8000 ? -v : v;
}

/* Shaders take pixel coordinates themselves (SAMPLER_PIXELS in the ID); the integer twin is the sampler itself. */
static void install_sampler(Backend *b, Obj *o, const VkSamplerCreateInfo *si, int pixels) {
    if (vkCreateSampler(b->device, si, NULL, &o->sampler)) { complain("sampler failed"); return; }
    o->sampler_index = heap_alloc(b, HEAP_SAMPLERS);
    VkDescriptorImageInfo ii = {.sampler = o->sampler};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->heap, .dstBinding = HEAP_SAMPLERS,
                               .dstArrayElement = o->sampler_index, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(b->device, 1, &wr, 0, NULL);
    o->sampler_id = o->sampler_index | (uint64_t)(o->sampler_index | (pixels ? SAMPLER_PIXELS : 0)) << 32;
    o->kind = K_SAMPLER;
}

/* A constexpr sampler a shader declares, in the encoding Apple's compiler writes into AIR (from the low bit: three
   3-bit address modes, 2-bit mag, min and mip filters, the pixel-coordinates flag, a 4-bit compare function, the
   anisotropy less one in 4 bits, half-float LOD minimum and maximum, 2-bit border color; Stela's metal/sampler.rs
   from_constexpr). Each state gets one sampler for the life of the backend. */
static uint64_t constexpr_sampler(void *ctx, uint64_t state) {
    Backend *b = ctx;
    for (unsigned i = 0; i < b->nconstexpr; i++)
        if (b->constexpr_samplers[i].state == state) return b->constexpr_samplers[i].object->sampler_id;
    if (b->nconstexpr == 256) { complain("constexpr sampler cache full"); return 0; }
#define F(shift, bits) ((uint32_t)(state >> (shift)) & ((1u << (bits)) - 1))
    static const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    static const VkCompareOp compares[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                           VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_ALWAYS,
                                           VK_COMPARE_OP_NEVER};
    static const VkBorderColor borders[] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    uint32_t cmp = F(16, 4), aniso = F(20, 4) + 1, mip = F(13, 2);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = F(9, 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, /* bicubic: linear, for now */
                              .minFilter = F(11, 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = modes[F(0, 3) & 3], .addressModeV = modes[F(3, 3) & 3], .addressModeW = modes[F(6, 3) & 3],
                              .anisotropyEnable = aniso > 1, .maxAnisotropy = (float)aniso,
                              .compareEnable = cmp >= 1 && cmp <= 7, .compareOp = compares[cmp <= 8 ? cmp : 0],
                              .minLod = half_bits(F(24, 16)), .maxLod = half_bits(F(40, 16)), .borderColor = borders[F(56, 2)],
                              .unnormalizedCoordinates = F(15, 1)};
#undef F
    if (!mip) si.maxLod = si.minLod + 0.25f; /* not mipmapped: the base level only */
    int pixels = si.unnormalizedCoordinates;
    si.unnormalizedCoordinates = VK_FALSE;
    Obj *o = calloc(1, sizeof *o);
    install_sampler(b, o, &si, pixels);
    b->constexpr_samplers[b->nconstexpr].state = state;
    b->constexpr_samplers[b->nconstexpr++].object = o;
    return o->sampler_id;
}

static void make_function(Backend *b, Obj *o, uint64_t at, uint32_t task) {
    struct __attribute__((packed)) { uint64_t off; uint32_t len; } h;
    if (hv_pvg_lite_task_read(b->lite, task, at, &h, sizeof h) || h.len > (64u << 20)) return;
    o->library = malloc(h.len), o->library_len = h.len;
    if (hv_pvg_lite_task_read(b->lite, task, h.off, o->library, h.len)) { free(o->library), o->library = NULL; return; }
    if (air_library_functions(o->library, o->library_len, &o->fn, 1) < 1) { complain("a function library this cannot read"); return; }
    o->kind = K_FUNCTION;
}

static void make_serialized(Backend *b, Obj *o, uint64_t at, uint32_t size, uint32_t task) {
    uint32_t *w = malloc(size < 64 ? 64 : size);
    memset(w, 0, size < 64 ? 64 : size);
    if (hv_pvg_lite_task_read(b->lite, task, at, w, size)) { free(w); return; }
    switch (w[0]) {
    case 3: make_sampler(b, o, w); break;
    case 4:
        o->ds.flags = w[3];
        memcpy(o->ds.front, w + 4, 12), memcpy(o->ds.back, w + 7, 12);
        o->kind = K_DEPTH_STENCIL;
        break;
    case 0xd: o->kind = K_FENCE; break;
    case 0xe:
        if (16 + w[3] > size || pvg_parse_render_pipeline((const uint8_t *)(w + 4), w[3], &o->rp)) { complain("render pipeline descriptor unreadable"); break; }
        if (o->rp.unknown_keys) complain("render pipeline descriptor keys %#llx not known", (unsigned long long)o->rp.unknown_keys);
        o->kind = K_RENDER;
        break;
    case 0xb:
        if (16 + w[3] > size || pvg_parse_compute_pipeline((const uint8_t *)(w + 4), w[3], &o->cp)) { complain("compute pipeline descriptor unreadable"); break; }
        o->kind = K_COMPUTE;
        break;
    default: complain("serialized object tag %u", w[0]); break;
    }
    free(w);
}

static void free_object(Backend *b, Obj *o) {
    if (!o) return;
    vkDeviceWaitIdle(b->device);
    for (int i = 0; i < 3; i++)
        if (o->render[i]) vkDestroyPipeline(b->device, o->render[i], NULL);
    for (int i = 0; i < o->ncompute; i++) vkDestroyPipeline(b->device, o->compute[i].pipeline, NULL);
    if (o->view) vkDestroyImageView(b->device, o->view, NULL);
    if (o->level_views) {
        for (uint32_t l = 0; l < (o->levels ? o->levels : 1); l++)
            if (o->level_views[l]) vkDestroyImageView(b->device, o->level_views[l], NULL), heap_free(b, HEAP_STORAGE, o->storage_index + l);
        free(o->level_views);
    }
    if (o->sampled_index) heap_free(b, HEAP_SAMPLED, o->sampled_index);
    if (o->owns_image) vkDestroyImage(b->device, o->image, NULL);
    if (o->sampler) vkDestroySampler(b->device, o->sampler, NULL), heap_free(b, HEAP_SAMPLERS, o->sampler_index);
    if (o->buffer && !o->parent) vkDestroyBuffer(b->device, o->buffer, NULL);
    if (o->memory) vkFreeMemory(b->device, o->memory, NULL);
    if (o->texture_memory) vkFreeMemory(b->device, o->texture_memory, NULL);
    free(o->tex_layout);
    free(o->library);
    free(o);
}

static Obj *object(Backend *b, uint32_t ref, uint32_t task) {
    if (!ref || task >= MAX_TASKS) return NULL;
    uint64_t list = hv_pvg_lite_task_objects(b->lite, task);
    uint32_t e[3];
    if (!list || hv_pvg_lite_task_read(b->lite, task, list + (uint64_t)ref * 12, e, 12)) return NULL;
    Task *t = &b->tasks[task];
    if (ref >= t->n) {
        uint32_t n = ref + 256;
        t->refs = realloc(t->refs, n * sizeof *t->refs);
        memset(t->refs + t->n, 0, (n - t->n) * sizeof *t->refs);
        t->n = n;
    }
    Obj *o = t->refs[ref];
    if (o && !memcmp(o->entry, e, 12) && (o->kind != K_NONE || (e[0] & 0xff) != OBJ_MAPPED_TEXTURE)) return o;
    free_object(b, o);
    o = t->refs[ref] = calloc(1, sizeof *o);
    memcpy(o->entry, e, 12);
    o->id = ++b->next_id;
    uint32_t type = e[0] & 0xff, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    switch (type) {
    case OBJ_BUFFER: case 19: make_buffer_object(b, o, at, task); break;
    case OBJ_TEXTURE: case 3: make_texture_entry(b, o, at, size, task); break;
    case OBJ_FUNCTION: make_function(b, o, at, task); break;
    case OBJ_SERIALIZED: make_serialized(b, o, at, size, task); break;
    case OBJ_LINEAR_TEXTURE: case 9: make_linear_texture(b, o, at, size, task); break;
    case 13: make_heap(b, o, at, task); break;
    case 14: make_heap_buffer(b, o, at, task); break;
    case OBJ_MAPPED_TEXTURE: make_mapped_texture(b, o, at, task); break;
    case 0: break;
    default: complain("object type %u", type); break;
    }
    return o;
}

/* ---- Pipelines ---- */

static VkShaderModule shader(Backend *b, Obj *fn, Obj *link, const uint32_t workgroup[3], int no_memory_model) {
    if (!fn || fn->kind != K_FUNCTION) return VK_NULL_HANDLE;
    AirSpirvOptions o = {.constexpr_sampler = constexpr_sampler, .ctx = b, .no_memory_model = no_memory_model};
    if (link && link->kind == K_FUNCTION) o.link_bitcode = link->fn.bitcode, o.link_size = link->fn.bitcode_size, o.link_entry = link->fn.name;
    if (workgroup) memcpy(o.workgroup, workgroup, sizeof o.workgroup);
    AirSpirv s;
    if (air_compile_spirv(fn->fn.bitcode, fn->fn.bitcode_size, fn->fn.name, &o, &s)) {
        complain("%s: %s", fn->fn.name, s.error);
        return VK_NULL_HANDLE;
    }
    if (getenv("HV_PVG_SPIRV_DIR")) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%llu.spv", getenv("HV_PVG_SPIRV_DIR"), (unsigned long long)fn->id);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(s.words, 4, s.count, f); fclose(f); }
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = s.count * 4, .pCode = s.words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(b->device, &ci, NULL, &m)) complain("%s: the driver refused its SPIR-V", fn->fn.name);
    air_spirv_free(&s);
    return m;
}

static int no_memory_model; /* set at creation from the device's features */

/* Metal primitive types (0 point, 1 line, 2 line strip, 3 triangle, 4 triangle strip) and their classes. */
static const VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
static int topology_class(uint32_t p) { return p == 0 ? 0 : p <= 2 ? 1 : 2; }

static VkPipeline render_pipeline(Backend *b, Obj *p, uint32_t task, int cls) {
    if (p->render[cls] || p->render_failed) return p->render[cls];
    PvgRenderPipeline *rp = &p->rp;
    Obj *vfn = object(b, (uint32_t)rp->vertex_function, task), *ffn = rp->fragment_function ? object(b, (uint32_t)rp->fragment_function, task) : NULL;
    VkShaderModule vs = shader(b, vfn, vfn, NULL, no_memory_model), fs = ffn ? shader(b, ffn, vfn, NULL, no_memory_model) : VK_NULL_HANDLE;
    if (!vs || (ffn && !fs)) {
        p->render_failed = 1;
        if (vs) vkDestroyShaderModule(b->device, vs, NULL);
        if (fs) vkDestroyShaderModule(b->device, fs, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    /* Vertex input, from the descriptor: Metal attribute n is location n, Metal buffer n is binding n. */
    VkVertexInputBindingDescription vb[PVG_MAX_VERTEX];
    VkVertexInputAttributeDescription va[PVG_MAX_VERTEX];
    uint32_t nvb = 0, nva = 0;
    for (uint32_t i = 0; rp->vertex.present && i < PVG_MAX_VERTEX; i++) {
        PvgVertexAttribute *a = &rp->vertex.attributes[i];
        if (!a->format) continue;
        VkFormat f = vertex_format(a->format);
        if (!f) { complain("vertex format %u", a->format); continue; }
        va[nva++] = (VkVertexInputAttributeDescription){.location = i, .binding = a->buffer, .format = f, .offset = a->offset};
    }
    for (uint32_t i = 0; rp->vertex.present && i < PVG_MAX_VERTEX; i++) {
        PvgVertexLayout *l = &rp->vertex.layouts[i];
        if (!l->used) continue;
        vb[nvb++] = (VkVertexInputBindingDescription){.binding = i, .stride = l->step_function == 0 ? 0 : l->stride,
                                                       .inputRate = l->step_function == 2 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va};
    static const VkPrimitiveTopology class_topology[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = class_topology[cls]};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .rasterizerDiscardEnable = !rp->rasterization, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)(rp->raster_samples ? rp->raster_samples : 1),
                                               .alphaToCoverageEnable = rp->alpha_to_coverage, .alphaToOneEnable = VK_FALSE};
    VkFormat colors[PVG_MAX_COLORS];
    VkPipelineColorBlendAttachmentState blends[PVG_MAX_COLORS];
    uint32_t ncolors = 0;
    for (uint32_t i = 0; i < PVG_MAX_COLORS; i++) {
        PvgColorAttachment *c = &rp->colors[i];
        const Format *f = c->pixel_format ? format_of(c->pixel_format) : NULL;
        colors[i] = f ? f->vk : VK_FORMAT_UNDEFINED;
        if (f) ncolors = i + 1;
        blends[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = c->blending, .srcColorBlendFactor = blend_factor(c->src_rgb), .dstColorBlendFactor = blend_factor(c->dst_rgb),
            .colorBlendOp = blend_op(c->rgb_op), .srcAlphaBlendFactor = blend_factor(c->src_alpha), .dstAlphaBlendFactor = blend_factor(c->dst_alpha),
            .alphaBlendOp = blend_op(c->alpha_op),
            /* MTLColorWriteMask: alpha 1, blue 2, green 4, red 8 */
            .colorWriteMask = ((c->write_mask & 8) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((c->write_mask & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                              ((c->write_mask & 2) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((c->write_mask & 1) ? VK_COLOR_COMPONENT_A_BIT : 0)};
    }
    const Format *df = rp->depth_format ? format_of(rp->depth_format) : NULL, *sf = rp->stencil_format ? format_of(rp->stencil_format) : NULL;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = ncolors,
                                        .pColorAttachmentFormats = colors, .depthAttachmentFormat = df ? df->vk : VK_FORMAT_UNDEFINED,
                                        .stencilAttachmentFormat = sf ? sf->vk : VK_FORMAT_UNDEFINED};
    if (df && (aspects(df->vk) & VK_IMAGE_ASPECT_STENCIL_BIT) && !sf) ri.stencilAttachmentFormat = df->vk;
    if (sf && (aspects(sf->vk) & VK_IMAGE_ASPECT_DEPTH_BIT) && !df) ri.depthAttachmentFormat = sf->vk;
    VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = ncolors, .pAttachments = blends};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
                                         VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                         VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                         VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                         VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                                         VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = fs ? 2 : 1, .pStages = st,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = b->layout};
    VkResult r = vkCreateGraphicsPipelines(b->device, b->cache, 1, &gi, NULL, &p->render[cls]);
    if (r) {
        complain("pipeline %s / %s: %d", vfn ? vfn->fn.name : "?", ffn ? ffn->fn.name : "-", r);
        p->render_failed = 1;
        p->render[cls] = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(b->device, vs, NULL);
    if (fs) vkDestroyShaderModule(b->device, fs, NULL);
    return p->render[cls];
}

static VkPipeline compute_pipeline(Backend *b, Obj *p, uint32_t task, const uint32_t wg[3]) {
    for (int i = 0; i < p->ncompute; i++)
        if (!memcmp(p->compute[i].key, wg, 12)) return p->compute[i].pipeline;
    Obj *fn = object(b, (uint32_t)p->cp.function, task);
    VkShaderModule cs = shader(b, fn, NULL, wg, no_memory_model);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (cs) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = b->layout,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = cs, .pName = "main"}};
        if (vkCreateComputePipelines(b->device, b->cache, 1, &ci, NULL, &pipe)) complain("compute pipeline %s failed", fn->fn.name);
        vkDestroyShaderModule(b->device, cs, NULL);
    }
    if (p->ncompute < 8) {
        memcpy(p->compute[p->ncompute].key, wg, 12);
        p->compute[p->ncompute++].pipeline = pipe;
    }
    return pipe;
}

/* ---- Encoding an exec ---- */

typedef struct {
    const uint8_t *p;
    uint32_t size;
} Rec;
#define U32(off) (*(const uint32_t *)(r.p + (off)))
#define U16(off) (*(const uint16_t *)(r.p + (off)))
#define U64(off) ({ uint64_t _v; memcpy(&_v, r.p + (off), 8); _v; })
#define F32(off) ({ float _v; memcpy(&_v, r.p + (off), 4); _v; })

typedef struct {
    Backend *b;
    uint32_t task;
    VkCommandBuffer cb;
    int in_pass, encoder; /* encoder: -1 none, 0 render, 1 compute, 2 blit */
    Obj *pipeline;
    int cls_bound;
    VkPipeline bound;
    Obj *depth_stencil;
    uint32_t cull, winding, stencil_front, stencil_back;
    float depth_bias[3];
    float blend[4];
    VkViewport viewport;
    VkRect2D scissor, area;
    int viewport_set, scissor_set;
    uint32_t threadgroup[4];
    VkImageView temp_views[64];
    int ntemp;
    Surface *drawn[64];
    int ndrawn;
} Enc;

static void note_drawn(Enc *e, Obj *o) {
    if (!o || !o->surface) return;
    for (int i = 0; i < e->ndrawn; i++)
        if (e->drawn[i] == o->surface) return;
    if (e->ndrawn < 64) e->drawn[e->ndrawn++] = o->surface;
}

static void set_buffer(Backend *b, int stage, uint32_t index, Obj *o, uint64_t offset) {
    if (index < 31) b->stage_buffers[stage][index] = o;
    if (index >= 31) return;
    uint64_t addr = o && o->kind == K_BUFFER ? o->address + offset : 0, len = o && o->kind == K_BUFFER && offset < o->length ? o->length - offset : 0;
    memcpy(b->table[stage] + TABLE_BUFFERS + 8 * index, &addr, 8);
    memcpy(b->table[stage] + TABLE_LENGTHS + 8 * index, &len, 8);
    if (stage == TABLE_PRIMARY) b->vertex_buffers[index] = o && o->kind == K_BUFFER ? o : NULL, b->vertex_offsets[index] = offset;
}

static void set_texture(Backend *b, int stage, uint32_t index, Obj *o) {
    if (index >= 128) return;
    uint64_t id = o && o->kind == K_IMAGE ? texture_id(o) : 0;
    memcpy(b->table[stage] + TABLE_TEXTURES + 8 * index, &id, 8);
}

static void set_sampler(Backend *b, int stage, uint32_t index, Obj *o) {
    if (index >= 16) return;
    uint64_t id = o && o->kind == K_SAMPLER ? o->sampler_id : 0;
    memcpy(b->table[stage] + TABLE_SAMPLERS + 8 * index, &id, 8);
}

/* Each stage's table into the ring, and their addresses into the push constants. */
static void push_tables(Enc *e, VkPipelineBindPoint point, const uint32_t grid[3]) {
    Backend *b = e->b;
    uint8_t push[PUSH_BYTES] = {0};
    for (int s = 0; s < STAGES; s++) {
        if (b->ring_used + TABLE_BYTES > b->ring_size) { complain("argument ring full"); b->ring_used = 0; }
        memcpy(b->ring_map + b->ring_used, b->table[s], TABLE_BYTES);
        uint64_t a = b->ring_address + b->ring_used;
        memcpy(push + PUSH_TABLES + 8 * s, &a, 8);
        b->ring_used += (TABLE_BYTES + 63) & ~63u;
    }
    if (grid) memcpy(push + PUSH_GRID, grid, 12);
    (void)point;
    vkCmdPushConstants(e->cb, b->layout, VK_SHADER_STAGE_ALL, 0, PUSH_BYTES, push);
}

static void end_encoder(Enc *e) {
    if (e->in_pass) vkCmdEndRendering(e->cb), e->in_pass = 0;
    if (e->encoder >= 0) barrier(e->cb);
    e->encoder = -1, e->pipeline = NULL, e->bound = VK_NULL_HANDLE;
}

static VkImageView attachment_view(Enc *e, Obj *o, uint32_t level, uint32_t slice) {
    if (level == 0 && slice == 0 && o->levels <= 1 && o->layers <= 1) return o->view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->format,
                                .subresourceRange = {aspects(o->format), level, 1, slice, 1}};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(e->b->device, &vi, NULL, &v);
    if (v && e->ntemp < 64) e->temp_views[e->ntemp++] = v;
    return v;
}

static void begin_pass(Enc *e, Rec r) {
    Backend *b = e->b;
    end_encoder(e);
    VkRenderingAttachmentInfo colors[8] = {{0}}, depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                              stencil = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t ncolors = 0, w = UINT32_MAX, h = UINT32_MAX;
    static const VkAttachmentLoadOp loads[] = {VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR};
    for (int i = -2; i < 8; i++) {
        uint32_t o = i == -2 ? 0 : i == -1 ? 0x28 : 0x4c + (uint32_t)i * 0x3c;
        if (o + 0x3c > r.size && i >= 0) break;
        Obj *t = U32(o) ? object(b, U32(o), e->task) : NULL;
        if (!t || t->kind != K_IMAGE) continue;
        VkRenderingAttachmentInfo *a = i == -2 ? &depth : i == -1 ? &stencil : &colors[i];
        a->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a->imageView = attachment_view(e, t, U16(o + 8), U16(o + 10));
        a->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        a->loadOp = loads[U16(o + 20) < 3 ? U16(o + 20) : 0];
        uint32_t store = U16(o + 22);
        a->storeOp = store == 0 || store == 2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (t->surface) a->storeOp = VK_ATTACHMENT_STORE_OP_STORE; /* the guest's memory is the truth */
        Obj *res = U32(o + 4) ? object(b, U32(o + 4), e->task) : NULL;
        if (res && res->kind == K_IMAGE && (store == 2 || store == 3)) {
            a->resolveMode = is_depth_stencil(t->format) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
            a->resolveImageView = attachment_view(e, res, U16(o + 14), U16(o + 16));
            a->resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            note_drawn(e, res);
        }
        if (i >= 0) {
            double c[4];
            memcpy(c, r.p + o + 0x1c, sizeof c);
            a->clearValue.color.float32[0] = (float)c[0], a->clearValue.color.float32[1] = (float)c[1];
            a->clearValue.color.float32[2] = (float)c[2], a->clearValue.color.float32[3] = (float)c[3];
            ncolors = (uint32_t)i + 1;
        } else if (i == -2) {
            double cd;
            memcpy(&cd, r.p + 0x1c, 8);
            a->clearValue.depthStencil.depth = (float)cd;
        } else a->clearValue.depthStencil.stencil = U32(0x44);
        uint32_t lw = t->width >> U16(o + 8), lh = t->height >> U16(o + 8);
        if (lw < w) w = lw ? lw : 1;
        if (lh < h) h = lh ? lh : 1;
        note_drawn(e, t);
    }
    if (r.size >= 0x248 && U64(0x238) && U64(0x240) && w == UINT32_MAX) w = (uint32_t)U64(0x238), h = (uint32_t)U64(0x240);
    if (w == UINT32_MAX) { complain("render pass without attachments"); return; }
    /* A combined depth-stencil texture is one attachment in Metal's two slots, and in Vulkan's. */
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = ncolors, .pColorAttachments = colors,
                          .pDepthAttachment = depth.imageView ? &depth : NULL, .pStencilAttachment = stencil.imageView ? &stencil : NULL};
    vkCmdBeginRendering(e->cb, &ri);
    e->in_pass = 1, e->encoder = 0;
    e->area = ri.renderArea;
    e->viewport = (VkViewport){0, 0, (float)w, (float)h, 0, 1};
    e->scissor = ri.renderArea;
    e->cull = 0, e->winding = 0, e->depth_stencil = NULL, e->stencil_front = e->stencil_back = 0;
    memset(e->depth_bias, 0, sizeof e->depth_bias);
    uint32_t one = 1;
    memcpy(b->table[TABLE_FRAGMENT] + TABLE_SAMPLE_COUNT, &one, 4);
}

static void dynamic_state(Enc *e, uint32_t primitive, int indexed) {
    VkCommandBuffer cb = e->cb;
    /* Metal's normalized device coordinates have y up: a viewport of negative height flips it as Metal does. */
    VkViewport v = e->viewport;
    v.y += v.height, v.height = -v.height;
    vkCmdSetViewportWithCount(cb, 1, &v);
    VkRect2D s = e->scissor;
    if (s.offset.x < 0) s.offset.x = 0;
    if (s.offset.y < 0) s.offset.y = 0;
    vkCmdSetScissorWithCount(cb, 1, &s);
    static const VkCullModeFlags culls[] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT};
    vkCmdSetCullMode(cb, culls[e->cull < 3 ? e->cull : 0]);
    /* The negative-height viewport reverses the winding used for front faces. */
    vkCmdSetFrontFace(cb, e->winding ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, topologies[primitive < 5 ? primitive : 3]);
    vkCmdSetPrimitiveRestartEnable(cb, indexed && (primitive == 2 || primitive == 4));
    DepthStencil ds = e->depth_stencil ? e->depth_stencil->ds : (DepthStencil){.flags = 7}; /* Always, no writes */
    vkCmdSetDepthTestEnable(cb, (ds.flags & 7) != 7 || (ds.flags & 8));
    vkCmdSetDepthWriteEnable(cb, (ds.flags >> 3) & 1);
    vkCmdSetDepthCompareOp(cb, compare_op(ds.flags & 7));
    int stencil = (ds.flags >> 4) & 3;
    vkCmdSetStencilTestEnable(cb, stencil != 0);
    for (int side = 0; side < 2; side++) {
        const uint32_t *t = side ? ds.back : ds.front;
        int on = (ds.flags >> (4 + side)) & 1;
        VkStencilFaceFlags f = side ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilOp(cb, f, on ? stencil_op(t[0] >> 3) : VK_STENCIL_OP_KEEP, on ? stencil_op(t[0] >> 9) : VK_STENCIL_OP_KEEP,
                          on ? stencil_op(t[0] >> 6) : VK_STENCIL_OP_KEEP, on ? compare_op(t[0]) : VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilCompareMask(cb, f, on ? t[1] : 0xff);
        vkCmdSetStencilWriteMask(cb, f, on ? t[2] : 0xff);
    }
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, e->stencil_front);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, e->stencil_back);
    vkCmdSetDepthBiasEnable(cb, e->depth_bias[0] != 0 || e->depth_bias[1] != 0);
    vkCmdSetDepthBias(cb, e->depth_bias[0], e->depth_bias[2], e->depth_bias[1]);
    vkCmdSetBlendConstants(cb, e->blend);
}

/* Binds what a draw needs; 0 if it cannot draw. */
static int prepare_draw(Enc *e, uint32_t primitive, int indexed) {
    Backend *b = e->b;
    if (!e->in_pass || !e->pipeline) return 0;
    int cls = topology_class(primitive);
    VkPipeline p = render_pipeline(b, e->pipeline, e->task, cls);
    if (!p) { b->failed_draws++; return 0; }
    if (p != e->bound) {
        vkCmdBindPipeline(e->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdBindDescriptorSets(e->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b->layout, 0, 1, &b->heap, 0, NULL);
        e->bound = p;
    }
    PvgRenderPipeline *rp = &e->pipeline->rp;
    for (uint32_t i = 0; rp->vertex.present && i < PVG_MAX_VERTEX; i++) {
        if (!rp->vertex.layouts[i].used) continue;
        Obj *o = b->vertex_buffers[i];
        if (!o) { complain("vertex buffer %u not bound", i); return 0; }
        VkDeviceSize off = o->offset + b->vertex_offsets[i];
        vkCmdBindVertexBuffers(e->cb, i, 1, &o->buffer, &off);
    }
    memcpy(b->table[TABLE_FRAGMENT] + TABLE_BLEND_COLOR, e->blend, 16);
    dynamic_state(e, primitive, indexed);
    push_tables(e, VK_PIPELINE_BIND_POINT_GRAPHICS, NULL);
    b->draws++;
    return 1;
}

static int bind_index(Enc *e, uint32_t ref, uint64_t offset, uint32_t type) {
    Obj *o = object(e->b, ref, e->task);
    if (!o || o->kind != K_BUFFER) return 0;
    vkCmdBindIndexBuffer(e->cb, o->buffer, o->offset + offset, type ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    return 1;
}

static void render(Enc *e, uint32_t tag, Rec r) {
    Backend *b = e->b;
    uint32_t task = e->task;
    switch (tag) {
    case 116: e->pipeline = object(b, U32(0), task); if (e->pipeline && e->pipeline->kind != K_RENDER) e->pipeline = NULL; break;
    case 125: case 110: case 157: { /* vertex, fragment, tile buffers: {index, count}, {ref, offset}... */
        uint32_t index = U32(0), count = U32(4);
        if (tag == 157) { complain("tile shaders are not drawn"); break; }
        for (uint32_t i = 0; i < count && i < 31 && 8 + i * 12 + 12 <= r.size; i++)
            set_buffer(b, tag == 125 ? TABLE_PRIMARY : TABLE_FRAGMENT, index + i, object(b, U32(8 + i * 12), task), U64(12 + i * 12));
        break;
    }
    case 165: { /* vertex buffers with strides: {index, count}, {ref, offset, stride} */
        uint32_t index = U32(0), count = U32(4);
        for (uint32_t i = 0; i < count && i < 31 && 8 + i * 20 + 20 <= r.size; i++)
            set_buffer(b, TABLE_PRIMARY, index + i, object(b, U32(8 + i * 20), task), U64(12 + i * 20));
        break;
    }
    case 126: case 111: case 166: { /* a new offset for one */
        int stage = tag == 111 ? TABLE_FRAGMENT : TABLE_PRIMARY;
        uint32_t i = U32(0);
        if (i >= 31) break;
        if (b->stage_buffers[stage][i]) set_buffer(b, stage, i, b->stage_buffers[stage][i], U64(4));
        break;
    }
    case 114: case 129: case 161: { /* fragment, vertex, tile textures */
        uint32_t index = U32(0), count = U32(4);
        if (tag == 161) break;
        for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= r.size; i++)
            set_texture(b, tag == 114 ? TABLE_FRAGMENT : TABLE_PRIMARY, index + i, object(b, U32(8 + i * 4), task));
        break;
    }
    case 112: case 127: case 113: case 128: { /* samplers (113, 128 with LOD clamps, which the sampler's own stand for) */
        uint32_t index = U32(0), count = U32(4);
        int stage = tag == 112 || tag == 113 ? TABLE_FRAGMENT : TABLE_PRIMARY;
        for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= r.size; i++) set_sampler(b, stage, index + i, object(b, U32(8 + i * 4), task));
        break;
    }
    case 117: { /* scissor: MTLScissorRect, NSUInteger x, y, w, h */
        uint64_t s[4];
        memcpy(s, r.p, sizeof s);
        e->scissor = (VkRect2D){{(int32_t)s[0], (int32_t)s[1]}, {(uint32_t)s[2], (uint32_t)s[3]}};
        break;
    }
    case 118: { /* scissors: the first */
        uint32_t head = r.size % 32;
        if (r.size >= head + 32) {
            uint64_t s[4];
            memcpy(s, r.p + head, sizeof s);
            e->scissor = (VkRect2D){{(int32_t)s[0], (int32_t)s[1]}, {(uint32_t)s[2], (uint32_t)s[3]}};
        }
        break;
    }
    case 130: case 131: { /* MTLViewport: double x, y, w, h, near, far */
        uint32_t head = tag == 131 ? r.size % 48 : 0;
        if (r.size < head + 48) break;
        double v[6];
        memcpy(v, r.p + head, sizeof v);
        e->viewport = (VkViewport){(float)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5]};
        break;
    }
    case 104: e->depth_stencil = object(b, U32(0), task); if (e->depth_stencil && e->depth_stencil->kind != K_DEPTH_STENCIL) e->depth_stencil = NULL; break;
    case 107: e->cull = U32(0); break;
    case 115: e->winding = U32(0); break;
    case 108: e->depth_bias[0] = F32(0), e->depth_bias[1] = F32(4), e->depth_bias[2] = F32(8); break;
    case 119: e->stencil_front = U32(0), e->stencil_back = U32(4); break;
    case 101: memcpy(e->blend, r.p, 16); break;
    case 109: case 124: break; /* depth clip mode, fill mode: not yet */
    case 24: case 25: case 27: case 134: case 135: case 137: break; /* fences, residency: every pass is ordered here */
    case 1: if (prepare_draw(e, U32(0), 0)) vkCmdDraw(e->cb, U16(6), 1, U16(4), 0); break;
    case 0: if (prepare_draw(e, U32(0), 0)) vkCmdDraw(e->cb, (uint32_t)U64(12), 1, (uint32_t)U64(4), 0); break;
    case 3: if (prepare_draw(e, U16(6), 0)) vkCmdDraw(e->cb, U16(2), U16(4), U16(0), 0); break;
    case 2: if (prepare_draw(e, U16(24), 0)) vkCmdDraw(e->cb, (uint32_t)U64(8), (uint32_t)U64(16), (uint32_t)U64(0), 0); break;
    case 4: if (prepare_draw(e, U16(32), 0)) vkCmdDraw(e->cb, (uint32_t)U64(8), (uint32_t)U64(16), (uint32_t)U64(0), (uint32_t)U64(24)); break;
    case 5: if (prepare_draw(e, U16(8), 0)) vkCmdDraw(e->cb, U16(2), U16(4), U16(0), U16(6)); break;
    case 7: /* {primitive, index type, ref, count 16, offset 16} */
        if (bind_index(e, U32(4), U16(10), U16(2)) && prepare_draw(e, U16(0), 1)) vkCmdDrawIndexed(e->cb, U16(8), 1, 0, 0, 0);
        break;
    case 6:
        if (bind_index(e, U32(4), U64(16), U16(2)) && prepare_draw(e, U16(0), 1)) vkCmdDrawIndexed(e->cb, (uint32_t)U64(8), 1, 0, 0, 0);
        break;
    case 9:
        if (bind_index(e, U32(4), U16(10), U16(2)) && prepare_draw(e, U16(0), 1)) vkCmdDrawIndexed(e->cb, U16(8), U16(12), 0, 0, 0);
        break;
    case 8:
        if (bind_index(e, U32(4), U64(16), U16(2)) && prepare_draw(e, U16(0), 1)) vkCmdDrawIndexed(e->cb, (uint32_t)U64(8), (uint32_t)U64(24), 0, 0, 0);
        break;
    case 10: case 28:
        if (bind_index(e, U32(4), U64(8), U16(2)) && prepare_draw(e, U16(0), 1))
            vkCmdDrawIndexed(e->cb, (uint32_t)U64(16), (uint32_t)U64(24), 0, (int32_t)U64(32), (uint32_t)U64(40));
        break;
    case 11: case 29:
        if (bind_index(e, U32(4), U16(8), U16(2)) && prepare_draw(e, U16(0), 1))
            vkCmdDrawIndexed(e->cb, U16(10), U16(12), 0, tag == 29 ? (int16_t)U16(14) : (int32_t)U16(14), U16(16));
        break;
    case 16: { /* {ref, primitive, offset 64} */
        Obj *o = object(b, U32(0), task);
        if (o && o->kind == K_BUFFER && prepare_draw(e, U16(4), 0)) vkCmdDrawIndirect(e->cb, o->buffer, o->offset + U64(8), 1, 16);
        break;
    }
    case 17: { /* {primitive, index type, index ref, indirect ref, index offset, indirect offset} */
        Obj *o = object(b, U32(8), task);
        if (o && o->kind == K_BUFFER && bind_index(e, U32(4), U64(12), U16(2)) && prepare_draw(e, U16(0), 1))
            vkCmdDrawIndexedIndirect(e->cb, o->buffer, o->offset + U64(20), 1, 20);
        break;
    }
    case 12: case 13: case 14: case 15: case 18: case 19: complain("tessellation draws are not drawn"); break;
    case 155: case 162: case 164: complain("tile dispatches are not run"); break;
    case 122: case 123: case 154: break;
    default: complain("render command %u (%u bytes) skipped", tag, r.size); break;
    }
}

static void dispatch(Enc *e, const uint32_t groups[3], const uint32_t threads[3], const uint32_t grid[3]) {
    Backend *b = e->b;
    if (!e->pipeline || e->pipeline->kind != K_COMPUTE) return;
    VkPipeline p = compute_pipeline(b, e->pipeline, e->task, threads);
    if (!p) return;
    vkCmdBindPipeline(e->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(e->cb, VK_PIPELINE_BIND_POINT_COMPUTE, b->layout, 0, 1, &b->heap, 0, NULL);
    push_tables(e, VK_PIPELINE_BIND_POINT_COMPUTE, grid);
    vkCmdDispatch(e->cb, groups[0], groups[1], groups[2]);
    b->dispatches++;
}

static void compute(Enc *e, uint32_t tag, Rec r) {
    Backend *b = e->b;
    uint32_t task = e->task;
    switch (tag) {
    case 208: e->pipeline = object(b, U32(0), task); break;
    case 203: {
        uint32_t index = U32(0), count = U32(4);
        for (uint32_t i = 0; i < count && i < 31 && 8 + i * 12 + 12 <= r.size; i++) set_buffer(b, TABLE_PRIMARY, index + i, object(b, U32(8 + i * 12), task), U64(12 + i * 12));
        break;
    }
    case 207: if (U32(0) < 31 && b->vertex_buffers[U32(0)]) set_buffer(b, TABLE_PRIMARY, U32(0), b->vertex_buffers[U32(0)], U64(4)); break;
    case 206: {
        uint32_t index = U32(0), count = U32(4);
        for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= r.size; i++) set_texture(b, TABLE_PRIMARY, index + i, object(b, U32(8 + i * 4), task));
        break;
    }
    case 204: {
        uint32_t index = U32(0), count = U32(4);
        for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= r.size; i++) set_sampler(b, TABLE_PRIMARY, index + i, object(b, U32(8 + i * 4), task));
        break;
    }
    case 211: if (U32(8) == 0) e->threadgroup[0] = (uint32_t)U64(0); break;
    case 200: case 202: { /* threadgroups (200) or threads (202), then threads per threadgroup; NSUInteger each */
        uint64_t a[3], t[3];
        memcpy(a, r.p, 24), memcpy(t, r.p + 24, 24);
        uint32_t threads[3] = {(uint32_t)t[0], (uint32_t)t[1], (uint32_t)t[2]}, groups[3], grid[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            if (!threads[i]) threads[i] = 1;
            if (tag == 200) groups[i] = (uint32_t)a[i];
            else groups[i] = (uint32_t)((a[i] + threads[i] - 1) / threads[i]), grid[i] = (uint32_t)a[i];
        }
        dispatch(e, groups, threads, grid);
        break;
    }
    case 214: case 215: barrier(e->cb); break;
    case 212: case 213: case 219: case 209: case 216: break;
    default: complain("compute command %u (%u bytes) skipped", tag, r.size); break;
    }
}

static uint32_t texel_bytes(Obj *t) {
    const Format *f = format_of(t->mtl_format);
    return f && f->bytes ? f->bytes : 4;
}

static void blit(Enc *e, uint32_t tag, Rec r) {
    Backend *b = e->b;
    uint32_t task = e->task;
    switch (tag) {
    case 300: { /* buffer to texture */
        Obj *s = object(b, U32(0), task), *t = object(b, U32(4), task);
        if (!s || s->kind != K_BUFFER || !t || t->kind != K_IMAGE) break;
        uint64_t size[3], origin[3];
        memcpy(size, r.p + 0x20, 24), memcpy(origin, r.p + 0x38, 24);
        uint32_t bpp = texel_bytes(t);
        VkBufferImageCopy c = {.bufferOffset = s->offset + U64(8), .bufferRowLength = (uint32_t)(U64(0x10) / bpp),
                               .bufferImageHeight = U64(0x10) ? (uint32_t)(U64(0x18) / U64(0x10)) : 0,
                               .imageSubresource = {aspects(t->format), U16(0x52), U16(0x50), 1},
                               .imageOffset = {(int32_t)origin[0], (int32_t)origin[1], (int32_t)origin[2]},
                               .imageExtent = {(uint32_t)size[0], (uint32_t)size[1], (uint32_t)size[2] ? (uint32_t)size[2] : 1}};
        if (aspects(t->format) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(e->cb, s->buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        note_drawn(e, t);
        break;
    }
    case 301: { /* buffer to buffer */
        Obj *s = object(b, U32(0), task), *d = object(b, U32(4), task);
        if (!s || s->kind != K_BUFFER || !d || d->kind != K_BUFFER || !U64(0x18)) break;
        VkBufferCopy c = {s->offset + U64(8), d->offset + U64(0x10), U64(0x18)};
        vkCmdCopyBuffer(e->cb, s->buffer, d->buffer, 1, &c);
        break;
    }
    case 302: { /* texture to buffer */
        Obj *t = object(b, U32(0), task), *d = object(b, U32(4), task);
        if (!t || t->kind != K_IMAGE || !d || d->kind != K_BUFFER) break;
        uint64_t origin[3], size[3];
        memcpy(origin, r.p + 8, 24), memcpy(size, r.p + 0x20, 24);
        uint32_t bpp = texel_bytes(t);
        VkBufferImageCopy c = {.bufferOffset = d->offset + U64(0x38), .bufferRowLength = (uint32_t)(U64(0x40) / bpp),
                               .bufferImageHeight = U64(0x40) ? (uint32_t)(U64(0x48) / U64(0x40)) : 0,
                               .imageSubresource = {aspects(t->format), U16(0x52), U16(0x50), 1},
                               .imageOffset = {(int32_t)origin[0], (int32_t)origin[1], (int32_t)origin[2]},
                               .imageExtent = {(uint32_t)size[0], (uint32_t)size[1], (uint32_t)size[2] ? (uint32_t)size[2] : 1}};
        if (aspects(t->format) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImageToBuffer(e->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, d->buffer, 1, &c);
        break;
    }
    case 303: { /* texture to texture */
        Obj *s = object(b, U32(0), task), *d = object(b, U32(4), task);
        if (!s || s->kind != K_IMAGE || !d || d->kind != K_IMAGE) break;
        uint64_t from[3], size[3], to[3];
        memcpy(from, r.p + 8, 24), memcpy(size, r.p + 0x20, 24), memcpy(to, r.p + 0x38, 24);
        VkImageCopy c = {.srcSubresource = {aspects(s->format), U16(0x52), U16(0x50), 1}, .srcOffset = {(int32_t)from[0], (int32_t)from[1], (int32_t)from[2]},
                         .dstSubresource = {aspects(d->format), U16(0x56), U16(0x54), 1}, .dstOffset = {(int32_t)to[0], (int32_t)to[1], (int32_t)to[2]},
                         .extent = {(uint32_t)size[0], (uint32_t)size[1], (uint32_t)size[2] ? (uint32_t)size[2] : 1}};
        vkCmdCopyImage(e->cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        note_drawn(e, d);
        break;
    }
    case 306: { /* fill: {ref, location, length, value} */
        Obj *o = object(b, U32(0), task);
        if (!o || o->kind != K_BUFFER) break;
        uint64_t at = o->offset + U64(4), n = U64(12);
        uint32_t v = r.p[20] * 0x01010101u;
        if (!(at & 3) && !(n & 3)) vkCmdFillBuffer(e->cb, o->buffer, at, n, v);
        else complain("unaligned buffer fill");
        break;
    }
    case 307: { /* mipmaps: each level from the one above */
        Obj *t = object(b, U32(0), task);
        if (!t || t->kind != K_IMAGE) break;
        for (uint32_t l = 1; l < t->levels; l++) {
            VkImageBlit bl = {.srcSubresource = {aspects(t->format), l - 1, 0, t->layers},
                              .srcOffsets = {{0, 0, 0}, {(int32_t)(t->width >> (l - 1) ? t->width >> (l - 1) : 1), (int32_t)(t->height >> (l - 1) ? t->height >> (l - 1) : 1), 1}},
                              .dstSubresource = {aspects(t->format), l, 0, t->layers},
                              .dstOffsets = {{0, 0, 0}, {(int32_t)(t->width >> l ? t->width >> l : 1), (int32_t)(t->height >> l ? t->height >> l : 1), 1}}};
            vkCmdBlitImage(e->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            barrier(e->cb);
        }
        break;
    }
    case 308: case 309: case 310: case 311: case 314: case 315: case 316: case 317: break;
    default: complain("blit command %u (%u bytes) skipped", tag, r.size); break;
    }
}

/* Writes into a buffer object's guest memory, and its device copy. */
static void write_object(Backend *b, uint32_t task, Obj *o, uint64_t offset, const void *data, uint32_t n) {
    Obj *base = o->parent ? o->parent : o;
    uint64_t at = (o->parent ? o->offset : 0) + offset;
    if (at + n > base->length) return;
    uint8_t *g = task_ptr(b, task, ((uint64_t)base->vpage << 14) + at, n);
    if (g) memcpy(g, data, n);
    if (base->map) memcpy(base->map + at, data, n);
}

static void info(Enc *e, uint32_t tag, Rec r) {
    Backend *b = e->b;
    uint32_t task = e->task, ref = U32(0), bufref = U32(4);
    uint64_t off = U64(8);
    if (tag == 451 || tag == 469) { /* how big a texture would be in a heap: {description, buffer ref, offset} */
        uint32_t at = tag == 451 ? 0x20 : 0x28;
        if (r.size < at + 12) return;
        TexDesc t;
        if (tag == 451) tex_v1(r.p, &t);
        else tex_v2(r.p, &t);
        Obj *to = object(b, U32(at), task);
        const Format *f = format_of(t.format);
        uint64_t v[2] = {0, 256};
        if (f) {
            Obj tmp = {.format = f->vk};
            if (!make_image(b, &tmp, t.type == 7 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, t.w, t.h, t.d, t.levels, t.layers ? t.layers : 1, t.samples, 0)) {
                VkMemoryRequirements req;
                vkGetImageMemoryRequirements(b->device, tmp.image, &req);
                v[0] = req.size, v[1] = req.alignment;
                vkDestroyImage(b->device, tmp.image, NULL), vkFreeMemory(b->device, tmp.memory, NULL);
            }
        }
        if (to && to->kind == K_BUFFER) write_object(b, task, to, U64(at + 4), v, 16);
        return;
    }
    if (tag == 460) bufref = U32(8), off = U64(12);
    Obj *to = object(b, bufref, task), *obj = object(b, ref, task);
    if (!to || to->kind != K_BUFFER || !obj) { complain("info %u: ref %u into buffer %u: missing", tag, ref, bufref); return; }
    uint8_t out[28] = {0};
    uint32_t n = 16;
    switch (tag) {
    case 460: memcpy(out, &obj->id, 8), n = 8; break;
    case 461: case 463: { uint64_t a = obj->kind == K_BUFFER ? obj->address : 0; memcpy(out, &a, 8), n = 8; break; }
    case 462: { uint64_t id = obj->kind == K_IMAGE ? texture_id(obj) : 0; memcpy(out, &id, 8); break; }
    case 464: memcpy(out, &obj->sampler_id, 8); break;
    case 466: case 467: case 468: memcpy(out, &obj->id, 8); break;
    case 457: { uint32_t v[2] = {1024, 0}; memcpy(out, v, 8), n = 10; break; }
    case 450: { uint64_t v[3] = {1024, 64, 0}; memcpy(out, v, 24), n = 25; break; }
    default: complain("info command %u (%u bytes) skipped", tag, r.size); return;
    }
    write_object(b, task, to, off, out, n);
}

/* The guest's copy of each resource the exec uses into its device copy, where it changed. */
static void upload(Enc *e, Obj *o) {
    Backend *b = e->b;
    if (!o) return;
    if (o->kind == K_BUFFER) {
        Obj *base = o->parent ? o->parent : o;
        if (!base->guest_backed || !base->map) return;
        uint32_t c = task_crc(b, e->task, base->vpage, base->length);
        if (c == base->guest_crc) return;
        base->guest_crc = c;
        task_copy(b, e->task, base->vpage, base->length, base->map, 0);
    } else if (o->kind == K_IMAGE) {
        if (o->surface) surface_upload(b, e->cb, o->surface);
        if (o->guest_backed && o->map && o->tex_layout) {
            uint32_t crc = task_crc(b, e->task, o->vpage, o->length);
            if (crc != o->guest_crc) {
                o->guest_crc = crc;
                task_copy(b, e->task, o->vpage, o->length, o->map, 0);
                const Format *f = format_of(o->mtl_format);
                if (!f || !f->bytes) complain("compressed guest texture uploads are not implemented");
                else for (uint32_t layer = 0; layer < o->layers; layer++)
                    for (uint32_t level = 0; level < o->tex_mips && level < o->levels; level++) {
                        const uint8_t *mip = o->tex_layout + level * 36;
                        uint64_t off, row;
                        uint32_t w, h, d;
                        memcpy(&off, mip, 8); memcpy(&row, mip + 16, 8);
                        memcpy(&w, mip + 24, 4); memcpy(&h, mip + 28, 4); memcpy(&d, mip + 32, 4);
                        off += o->tex_base + o->tex_stride * layer;
                        if (!w || !h || !d || off + row * h * d > o->length) continue;
                        VkBufferImageCopy copy = {.bufferOffset = off, .bufferRowLength = (uint32_t)(row / f->bytes),
                            .imageSubresource = {aspects(o->format), level, layer, 1}, .imageExtent = {w, h, d}};
                        vkCmdCopyBufferToImage(e->cb, o->buffer, o->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                    }
            }
        }
        Obj *buf = o->parent && o->parent->kind == K_BUFFER ? o->parent : NULL;
        if (buf && o->linear_row && !o->surface) { /* a texture over a buffer: from the buffer, each time */
            upload(e, buf);
            VkBufferImageCopy c = {.bufferOffset = buf->offset + o->linear_offset, .bufferRowLength = (uint32_t)(o->linear_row / texel_bytes(o)),
                                   .imageSubresource = {aspects(o->format), 0, 0, 1}, .imageExtent = {o->width, o->height, 1}};
            vkCmdCopyBufferToImage(e->cb, buf->buffer, o->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
}

static void download(Backend *b, uint32_t task, Obj *o) {
    if (!o || o->kind != K_BUFFER) return;
    Obj *base = o->parent ? o->parent : o;
    if (!base->guest_backed || !base->map) return;
    task_copy(b, task, base->vpage, base->length, base->map, 1);
    base->guest_crc = task_crc(b, task, base->vpage, base->length);
}

static void exec(Backend *b, const uint32_t *a, uint32_t len) {
    uint32_t task = a[0], nres = a[1], nbuf = a[2];
    if (task >= MAX_TASKS || 12 + nres * 8ull + nbuf * 16ull > len) { complain("exec of %u bytes dropped", len); return; }
    const uint32_t *res = a + 3;
    const uint8_t *bufs = (const uint8_t *)(res + nres * 2);
    Enc e = {.b = b, .task = task, .encoder = -1};
    /* Materialize bindings before recording transfers. A command's resource list may
       name a buffer while its texture view is only named by the draw stream. */
    for (uint32_t k = 0; k < nbuf; k++) {
        uint64_t off, n;
        memcpy(&off, bufs + k * 16, 8); memcpy(&n, bufs + k * 16 + 8, 8);
        if (n < 8 || n > (64u << 20)) continue;
        uint8_t *copy = malloc(n);
        if (hv_pvg_lite_task_read(b->lite, task, off, copy, (uint32_t)n)) { free(copy); continue; }
        uint32_t pos = 8;
        uint32_t encoder; memcpy(&encoder, copy + 4, 4);
        if ((encoder & 255) == 5) pos += 16;
        while (pos + 8 <= n) {
            uint32_t tag, size;
            memcpy(&tag, copy + pos, 4); memcpy(&size, copy + pos + 4, 4);
            if (size < 8 || pos + size > n) break;
            if ((tag == 114 || tag == 129 || tag == 206) && size >= 16) {
                uint32_t count; memcpy(&count, copy + pos + 12, 4);
                for (uint32_t i = 0; i < count && 16 + i * 4 + 4 <= size; i++) {
                    uint32_t ref; memcpy(&ref, copy + pos + 16 + i * 4, 4);
                    object(b, ref, task);
                }
            }
            pos += size;
        }
        free(copy);
    }
    e.cb = begin_once(b);
    b->ring_used = 0;
    for (uint32_t i = 0; i < nres; i++) upload(&e, object(b, res[i * 2], task));
    for (uint32_t i = 0; i < b->tasks[task].n; i++) {
        Obj *o = b->tasks[task].refs[i];
        if (o && o->kind == K_IMAGE) upload(&e, o);
    }
    barrier(e.cb);
    for (uint32_t k = 0; k < nbuf; k++) {
        uint64_t off, blen;
        memcpy(&off, bufs + k * 16, 8), memcpy(&blen, bufs + k * 16 + 8, 8);
        if (blen > (64u << 20) || blen < 8) continue;
        uint8_t *copy = malloc(blen);
        if (hv_pvg_lite_task_read(b->lite, task, off, copy, (uint32_t)blen)) { free(copy); complain("command buffer unreadable"); continue; }
        const uint8_t *p = copy, *end = p + blen;
        uint32_t seglen, info_word;
        memcpy(&seglen, p, 4), memcpy(&info_word, p + 4, 4);
        p += 8;
        if ((info_word & 0xff) == 5) {
            p += 8;
            if (p + 8 > end) { free(copy); continue; }
            memcpy(&seglen, p, 4), memcpy(&info_word, p + 4, 4);
            p += 8;
        }
        uint32_t type = info_word & 0xff, cont = (info_word >> 8) & 0xff, more = (info_word >> 16) & 0xff;
        if (p + seglen < end) end = p + seglen;
        if (!cont && type != 4) end_encoder(&e);
        if (type <= 2 && e.encoder < 0 && type != 0) e.encoder = (int)type, e.pipeline = NULL;
        while (p + 8 <= end) {
            uint32_t tag, size;
            memcpy(&tag, p, 4), memcpy(&size, p + 4, 4);
            if (size < 8 || p + size > end) break;
            Rec r = {p + 8, size - 8};
            if (type == 4) info(&e, tag, r);
            else if (type == 0 && tag == 26) begin_pass(&e, r);
            else if (type == 0 && tag >= 30 && tag <= 36) { /* describe the pass that follows: samples, tiles */ }
            else if (type == 0) render(&e, tag, r);
            else if (type == 1) compute(&e, tag, r);
            else if (type == 2) blit(&e, tag, r);
            else complain("encoder type %u", type);
            p += size;
        }
        if (!more && type != 4) end_encoder(&e);
        free(copy);
    }
    end_encoder(&e);
    for (int i = 0; i < e.ndrawn; i++) surface_download_record(e.cb, e.drawn[i]);
    end_once(b, e.cb);
    for (int i = 0; i < e.ntemp; i++) vkDestroyImageView(b->device, e.temp_views[i], NULL);
    for (int i = 0; i < e.ndrawn; i++) {
        surface_copy(b, e.drawn[i], 1);
        e.drawn[i]->guest_crc = surface_crc(b, e.drawn[i]);
    }
    for (uint32_t i = 0; i < nres; i++)
        if (res[i * 2 + 1] & 2) download(b, task, object(b, res[i * 2], task));
    b->execs++;
}

/* ---- The display ---- */

static void present(Backend *b, uint64_t mapping) {
    b->on_screen = mapping;
    b->gpu_frames++;
}

static void set_window(Backend *b, void *window) {
    (void)b;
    if (window) complain("window presentation is not implemented");
}

static void cursor_glyph(Backend *b, const uint32_t *a, uint32_t n) {
    if (n < 0x2c) return;
    uint64_t off, stride;
    memcpy(&off, a + 2, 8), memcpy(&stride, a + 6, 8);
    uint16_t w, h, hx, hy;
    memcpy(&w, (const uint8_t *)a + 0x20, 2), memcpy(&h, (const uint8_t *)a + 0x22, 2);
    memcpy(&hx, (const uint8_t *)a + 0x24, 2), memcpy(&hy, (const uint8_t *)a + 0x26, 2);
    if (!w || !h || w > 512 || h > 512 || stride < (uint64_t)w * 4 || stride > 8192) return;
    uint8_t *bits = malloc((size_t)stride * h);
    if (hv_pvg_lite_task_read(b->lite, 0, off, bits, (uint32_t)(stride * h))) { free(bits); return; }
    free(b->cursor);
    b->cursor = bits, b->cursor_w = w, b->cursor_h = h, b->cursor_stride = (uint32_t)stride, b->cursor_hot_x = hx, b->cursor_hot_y = hy;
}

/* ---- The worker ---- */

static void run(Backend *b, Cmd *c) {
    if (c->kind == 1) {
        uint32_t m = c->mapping;
        if (m < b->nsurfaces && b->surfaces[m]) {
            vkDeviceWaitIdle(b->device);
            for (int t = 0; t < MAX_TASKS; t++) /* textures over it go with it */
                for (uint32_t i = 0; i < b->tasks[t].n; i++)
                    if (b->tasks[t].refs[i] && b->tasks[t].refs[i]->surface == b->surfaces[m]) free_object(b, b->tasks[t].refs[i]), b->tasks[t].refs[i] = NULL;
            surface_free(b, b->surfaces[m]);
            b->surfaces[m] = NULL;
        }
        return;
    }
    if (c->kind == 2) { if (b->on_screen) present(b, b->on_screen); return; }
    if (c->kind == 3) { set_window(b, c->layer); return; }
    uint32_t *cmd = c->words, len = c->len;
    uint32_t op = cmd[0] & 0xffff, skip = (cmd[0] >> 16) * 8;
    const uint32_t *args = (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
    uint32_t n = len > 12 + skip ? len - 12 - skip : 0;
    if ((op == 0x43 || op == 0x45) && n >= 12) exec(b, args, n);
    else if (op == 0x37 && n >= 12) { /* macOS 13's exec: 24-byte resource entries */
        uint32_t nres = args[1], nbuf = args[2];
        if (12 + nres * 24ull + nbuf * 16ull <= n && nres < 65536 && nbuf < 65536) {
            uint32_t l = 12 + nres * 8 + nbuf * 16, *a = malloc(l);
            a[0] = args[0], a[1] = nres, a[2] = nbuf;
            for (uint32_t i = 0; i < nres; i++) a[3 + i * 2] = args[3 + i * 6], a[4 + i * 2] = args[4 + i * 6];
            memcpy(a + 3 + nres * 2, args + 3 + nres * 6, nbuf * 16ull);
            exec(b, a, l);
            free(a);
        }
    } else if ((op == 0x25 || op == 0x28) && n >= 8) {
        uint32_t task = args[0], ref = op == 0x25 ? args[1] : (n >= 16 ? args[3] : 0);
        if (task < MAX_TASKS && ref < b->tasks[task].n) free_object(b, b->tasks[task].refs[ref]), b->tasks[task].refs[ref] = NULL;
    } else if (op == 0x20 && n >= 4) {
        uint32_t task = args[0] >> 1;
        if (task < MAX_TASKS) {
            for (uint32_t i = 0; i < b->tasks[task].n; i++) free_object(b, b->tasks[task].refs[i]);
            free(b->tasks[task].refs), b->tasks[task].refs = NULL, b->tasks[task].n = 0;
        }
    } else if (op == 0x34 || op == 0x35 || op == 0x3e || op == 0x3f) {
        /* invalidate, synchronize, purgeable: buffers and surfaces are copied back after every exec that writes them */
    } else {
        hv_pvg_lite_command(b->lite, cmd, len);
        if (op == 8 && n >= 16) present(b, (uint64_t)args[3] << 32 | args[2]);
        if (op == 4) cursor_glyph(b, args, n);
        if (op == 5 && n >= 8) b->cursor_shown = args[1] & 1;
    }
    hv_pvg_lite_complete(b->lite, c->channel, cmd[2]);
}

static void *worker(void *arg) {
    Backend *b = arg;
    pthread_mutex_lock(&b->mu);
    for (;;) {
        while (!b->head && !b->stopping) pthread_cond_wait(&b->cv, &b->mu);
        if (!b->head) break;
        Cmd *c = b->head;
        b->head = c->next;
        if (!b->head) b->tail = NULL;
        b->busy = 1;
        pthread_mutex_unlock(&b->mu);
        run(b, c);
        free(c->words);
        free(c);
        pthread_mutex_lock(&b->mu);
        b->busy = 0;
        if (!b->head) pthread_cond_broadcast(&b->idle_cv);
    }
    pthread_mutex_unlock(&b->mu);
    return NULL;
}

static void enqueue(Backend *b, Cmd *c) {
    pthread_mutex_lock(&b->mu);
    if (b->tail) b->tail->next = c;
    else b->head = c;
    b->tail = c;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
}

static void wait_idle(Backend *b) {
    pthread_mutex_lock(&b->mu);
    while (b->head || b->busy) pthread_cond_wait(&b->idle_cv, &b->mu);
    pthread_mutex_unlock(&b->mu);
}

/* ---- Making the backend ---- */

static int init_vulkan(Backend *b) {
    const char *iext[4];
    uint32_t niext = 0;
    VkInstanceCreateFlags flags = 0;
#if defined(__APPLE__)
    iext[niext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Menhir", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = flags, .pApplicationInfo = &app,
                                .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext};
    if (vkCreateInstance(&ici, NULL, &b->instance)) { fprintf(stderr, "hengevm: pvg-vulkan: no Vulkan instance\n"); return -1; }
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(b->instance, &n, &b->physical) < 0 || !n) { fprintf(stderr, "hengevm: pvg-vulkan: no GPU\n"); return -1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(b->physical, &props);
    vkGetPhysicalDeviceMemoryProperties(b->physical, &b->memory_props);
    /* The features the shaders need, of those the device has. */
    VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
    vkGetPhysicalDeviceFeatures2(b->physical, &have);
    if (!h12.bufferDeviceAddress || !h12.descriptorIndexing || !h12.runtimeDescriptorArray || !h13.dynamicRendering) {
        fprintf(stderr, "hengevm: pvg-vulkan: %s lacks buffer device addresses, descriptor indexing or dynamic rendering\n", props.deviceName);
        return -1;
    }
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = 1,
                                            .synchronization2 = h13.synchronization2, .maintenance4 = h13.maintenance4,
                                            .shaderDemoteToHelperInvocation = h13.shaderDemoteToHelperInvocation};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
#define COPY(x) f12.x = h12.x
    COPY(bufferDeviceAddress), COPY(descriptorIndexing), COPY(runtimeDescriptorArray), COPY(descriptorBindingPartiallyBound);
    COPY(descriptorBindingSampledImageUpdateAfterBind), COPY(descriptorBindingStorageImageUpdateAfterBind);
    COPY(descriptorBindingUniformTexelBufferUpdateAfterBind), COPY(descriptorBindingStorageTexelBufferUpdateAfterBind);
    COPY(shaderSampledImageArrayNonUniformIndexing), COPY(shaderStorageImageArrayNonUniformIndexing), COPY(shaderFloat16), COPY(shaderInt8);
    COPY(storageBuffer8BitAccess), COPY(vulkanMemoryModel), COPY(vulkanMemoryModelDeviceScope), COPY(scalarBlockLayout), COPY(shaderOutputLayer);
    COPY(timelineSemaphore), COPY(uniformAndStorageBuffer8BitAccess), COPY(shaderBufferInt64Atomics), COPY(samplerMirrorClampToEdge);
#undef COPY
    no_memory_model = !h12.vulkanMemoryModel;

    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12,
                                            .storageBuffer16BitAccess = h11.storageBuffer16BitAccess, .shaderDrawParameters = h11.shaderDrawParameters};
    VkPhysicalDeviceFeatures2 feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
#define COPY(x) feat.features.x = have.features.x
    COPY(shaderInt64), COPY(shaderInt16), COPY(fragmentStoresAndAtomics), COPY(vertexPipelineStoresAndAtomics);
    COPY(shaderStorageImageReadWithoutFormat), COPY(shaderStorageImageWriteWithoutFormat), COPY(imageCubeArray), COPY(sampleRateShading);
    COPY(shaderClipDistance), COPY(independentBlend), COPY(dualSrcBlend), COPY(samplerAnisotropy), COPY(textureCompressionBC);
    COPY(depthClamp), COPY(depthBiasClamp), COPY(fillModeNonSolid), COPY(multiDrawIndirect), COPY(drawIndirectFirstInstance);
    COPY(fullDrawIndexUint32), COPY(imageCubeArray), COPY(alphaToOne);
#undef COPY
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(b->physical, &nq, qf);
    for (b->queue_family = 0; b->queue_family < nq && !(qf[b->queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT); b->queue_family++);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = b->queue_family, .queueCount = 1, .pQueuePriorities = &prio};
    const char *dext[4];
    uint32_t ndext = 0, nde = 0;
    vkEnumerateDeviceExtensionProperties(b->physical, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde, sizeof *de);
    vkEnumerateDeviceExtensionProperties(b->physical, NULL, &nde, de);
    for (uint32_t i = 0; i < nde; i++) {
        if (!strcmp(de[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (!strcmp(de[i].extensionName, "VK_KHR_portability_subset")) dext[ndext++] = "VK_KHR_portability_subset";
    }
    free(de);
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &feat, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext};
    VkResult r = vkCreateDevice(b->physical, &dci, NULL, &b->device);
    if (r) { fprintf(stderr, "hengevm: pvg-vulkan: no device (%d)\n", r); return -1; }
    vkGetDeviceQueue(b->device, b->queue_family, 0, &b->queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = b->queue_family};
    vkCreateCommandPool(b->device, &pci, NULL, &b->pool);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(b->device, &fci, NULL, &b->fence);
    VkPipelineCacheCreateInfo pcci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(b->device, &pcci, NULL, &b->cache);

    /* The descriptor heap: set 0, as Stela's shaders index it (metal_abi.h STELA_METAL_HEAP_*). */
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(b->physical, &p2);
    static const VkDescriptorType types[HEAPS] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                                  VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    uint32_t limits[HEAPS] = {p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindStorageImages,
                              p12.maxDescriptorSetUpdateAfterBindSamplers, p12.maxDescriptorSetUpdateAfterBindSampledImages,
                              p12.maxDescriptorSetUpdateAfterBindStorageImages};
    VkDescriptorSetLayoutBinding bind[HEAPS];
    VkDescriptorBindingFlags bf[HEAPS];
    VkDescriptorPoolSize sizes[HEAPS];
    for (int i = 0; i < HEAPS; i++) {
        uint32_t count = heap_size[i] < limits[i] / 2 ? heap_size[i] : limits[i] / 2;
        bind[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = types[i], .descriptorCount = count, .stageFlags = VK_SHADER_STAGE_ALL};
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        sizes[i] = (VkDescriptorPoolSize){types[i], count};
        b->heap_capacity[i] = count;
        b->next_index[i] = 1; /* 0 is no texture or sampler */
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = HEAPS, .pBindingFlags = bf};
    VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = HEAPS, .pBindings = bind};
    if ((r = vkCreateDescriptorSetLayout(b->device, &sli, NULL, &b->set_layout))) { fprintf(stderr, "hengevm: pvg-vulkan: no descriptor heap (%d)\n", r); return -1; }
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                      .maxSets = 1, .poolSizeCount = HEAPS, .pPoolSizes = sizes};
    vkCreateDescriptorPool(b->device, &dpi, NULL, &b->descriptor_pool);
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = b->descriptor_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &b->set_layout};
    if ((r = vkAllocateDescriptorSets(b->device, &dsai, &b->heap))) { fprintf(stderr, "hengevm: pvg-vulkan: no descriptor set (%d)\n", r); return -1; }
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_ALL, .size = PUSH_BYTES};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &b->set_layout,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    vkCreatePipelineLayout(b->device, &pli, NULL, &b->layout);
    b->ring_size = 16u << 20;
    if (make_buffer(b, b->ring_size, 0, &b->ring, &b->ring_memory, &b->ring_map, &b->ring_address)) return -1;
    fprintf(stderr, "hengevm: pvg-vulkan: drawing on %s (Vulkan %u.%u, %s memory model)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion), no_memory_model ? "GLSL450" : "Vulkan");
    return 0;
}

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    Backend *b = calloc(1, sizeof *b);
    b->lite = lite;
    if (init_vulkan(b)) { free(b); return NULL; }
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    pthread_cond_init(&b->idle_cv, NULL);
    pthread_create(&b->thread, NULL, worker, b);
    return b;
}

void hv_pvg_metal_destroy(HvPvgMetal *m) {
    Backend *b = m;
    if (!b) return;
    pthread_mutex_lock(&b->mu);
    b->stopping = 1;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
    pthread_join(b->thread, NULL);
    vkDeviceWaitIdle(b->device);

    for (int t = 0; t < MAX_TASKS; t++) {
        for (uint32_t i = 0; i < b->tasks[t].n; i++) free_object(b, b->tasks[t].refs[i]);
        free(b->tasks[t].refs);
    }
    for (uint32_t i = 0; i < b->nsurfaces; i++)
        if (b->surfaces[i]) surface_free(b, b->surfaces[i]);
    for (unsigned i = 0; i < b->nconstexpr; i++) free_object(b, b->constexpr_samplers[i].object);
    vkDestroyBuffer(b->device, b->ring, NULL);
    vkFreeMemory(b->device, b->ring_memory, NULL);
    vkDestroyPipelineCache(b->device, b->cache, NULL);
    vkDestroyPipelineLayout(b->device, b->layout, NULL);
    vkDestroyDescriptorPool(b->device, b->descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(b->device, b->set_layout, NULL);
    vkDestroyFence(b->device, b->fence, NULL);
    vkDestroyCommandPool(b->device, b->pool, NULL);
    for (int i = 0; i < HEAPS; i++) free(b->free_index[i]);
    pthread_cond_destroy(&b->cv);
    pthread_cond_destroy(&b->idle_cv);
    pthread_mutex_destroy(&b->mu);
    vkDestroyDevice(b->device, NULL);
    vkDestroyInstance(b->instance, NULL);
    free(b->surfaces), free(b->cursor), free(b);
}

void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t ch, uint32_t *cmd, uint32_t len) {
    (void)lite;
    Cmd *c = calloc(1, sizeof *c);
    c->channel = ch, c->len = len, c->words = cmd;
    enqueue(m, c);
}

void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) {
    Cmd *c = calloc(1, sizeof *c);
    c->kind = 1, c->mapping = mapping;
    enqueue(m, c);
}

void hv_pvg_metal_cursor_moved(HvPvgMetal *m) {
    Backend *b = m;
    if (!b->window) return;
    Cmd *c = calloc(1, sizeof *c);
    c->kind = 2;
    enqueue(m, c);
}

/* Window presentation is outside the off-screen replay milestone. */
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) {
    (void)m;
    if (layer) complain("window presentation is not implemented");
}

uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { return m ? ((Backend *)m)->gpu_frames : 0; }

/* The shown surface, small, BGRA: read from the guest's memory, where every exec that draws it leaves it. */
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t w, uint32_t h) {
    Backend *b = m;
    if (!b || !b->on_screen || !w || !h) return -1;
    Surface *s = b->on_screen < b->nsurfaces ? b->surfaces[b->on_screen] : NULL;
    if (!s) return -1;
    int half = s->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint64_t sx = (uint64_t)x * s->width / w, sy = (uint64_t)y * s->height / h;
            uint64_t at = s->first + sy * s->row + sx * (half ? 8 : 4);
            uint32_t pi = (uint32_t)(at / PAGE);
            const uint8_t *p = pi < s->npages && s->pages[pi] ? hv_pvg_lite_phys(b->lite, ((uint64_t)s->pages[pi] << 14) + at % PAGE, 8) : NULL;
            uint8_t *o = bgra + ((size_t)y * w + x) * 4;
            if (!p) { memset(o, 0, 4); continue; }
            if (half) {
                for (int c = 0; c < 3; c++) {
                    uint16_t hv;
                    memcpy(&hv, p + c * 2, 2);
                    float v = half_bits(hv);
                    o[2 - c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f);
                }
            } else memcpy(o, p, 3);
            o[3] = 255;
        }
    return 0;
}

int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **pixels, uint32_t *w, uint32_t *h) {
    (void)m, (void)pixels, (void)w, (void)h;
    return -1;
}

/* Instant resume. Buffers and surfaces are in guest memory after every exec; private textures are not (to do). */
void hv_pvg_metal_quiesce(HvPvgMetal *m) {
    Backend *b = m;
    wait_idle(b);
    vkDeviceWaitIdle(b->device);
}

typedef struct {
    uint64_t on_screen;
    uint32_t cursor_w, cursor_h, cursor_stride, cursor_shown;
    int32_t hot_x, hot_y;
} DisplayState;

size_t hv_pvg_metal_display_state(HvPvgMetal *m, void *out, size_t cap) {
    Backend *b = m;
    wait_idle(b);
    size_t bits = b->cursor ? (size_t)b->cursor_stride * b->cursor_h : 0, need = sizeof(DisplayState) + bits;
    if (out && cap >= need) {
        DisplayState d = {b->on_screen, bits ? b->cursor_w : 0, bits ? b->cursor_h : 0, bits ? b->cursor_stride : 0, (uint32_t)b->cursor_shown,
                          b->cursor_hot_x, b->cursor_hot_y};
        memcpy(out, &d, sizeof d);
        if (bits) memcpy((uint8_t *)out + sizeof d, b->cursor, bits);
    }
    return need;
}

void hv_pvg_metal_set_display_state(HvPvgMetal *m, const void *in, size_t len) {
    Backend *b = m;
    if (!b || len < sizeof(DisplayState)) return;
    wait_idle(b);
    DisplayState d;
    memcpy(&d, in, sizeof d);
    size_t bits = (size_t)d.cursor_stride * d.cursor_h;
    if (bits && len >= sizeof d + bits && d.cursor_w <= 512 && d.cursor_h <= 512) {
        free(b->cursor);
        b->cursor = malloc(bits);
        memcpy(b->cursor, (const uint8_t *)in + sizeof d, bits);
        b->cursor_w = d.cursor_w, b->cursor_h = d.cursor_h, b->cursor_stride = d.cursor_stride, b->cursor_hot_x = d.hot_x, b->cursor_hot_y = d.hot_y;
    }
    b->cursor_shown = (int)d.cursor_shown;
    b->on_screen = d.on_screen;
}
