/* Builds a Vulkan pipeline from each SPIR-V module the shader compiler made (aircc -o DIR), so the device's own
   compiler runs on it: the check that matters on Adreno, which spirv-val cannot make.

     spvcheck FILE.spv...

   A compute module gets a compute pipeline; a vertex module a pipeline with rasterization off; a fragment module a
   pipeline behind a vertex shader that writes only the position (its other inputs are undefined, which is enough
   to compile it). The layout is the backend's: set 0 the descriptor heap (devices/air/stela/metal_abi.h
   STELA_METAL_HEAP_*), 64 bytes of push constants. Exit status: the number that failed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

/* A vertex shader that writes only the position: position_vs.h, made by glslangValidator -V --vn position_vs from
   position.vert (gl_Position = vec4(0)). */
#include "position_vs.h"

static uint32_t *slurp(const char *path, size_t *words) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *words = (size_t)n / 4;
    return d;
}

/* The execution model of the module's first entry point: 0 vertex, 4 fragment, 5 compute, -1 none. */
static int model(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < n) return (int)w[i + 1];
        i += len;
    }
    return -1;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("no instance\n"); return 100; }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = 1,
                                            .shaderDemoteToHelperInvocation = 1, .synchronization2 = 1, .maintenance4 = 1};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
                                            .bufferDeviceAddress = 1, .descriptorIndexing = 1, .runtimeDescriptorArray = 1,
                                            .descriptorBindingPartiallyBound = 1, .descriptorBindingSampledImageUpdateAfterBind = 1,
                                            .descriptorBindingStorageImageUpdateAfterBind = 1,
                                            .descriptorBindingUniformTexelBufferUpdateAfterBind = 1,
                                            .descriptorBindingStorageTexelBufferUpdateAfterBind = 1,
                                            .shaderSampledImageArrayNonUniformIndexing = 1, .shaderStorageImageArrayNonUniformIndexing = 1,
                                            .shaderFloat16 = 1, .shaderInt8 = 1, .storageBuffer8BitAccess = 1, .vulkanMemoryModel = 1,
                                            .vulkanMemoryModelDeviceScope = 1, .scalarBlockLayout = 1, .shaderOutputLayer = 1,
                                            .timelineSemaphore = 1};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12,
                                            .storageBuffer16BitAccess = 1, .shaderDrawParameters = 1};
    VkPhysicalDeviceFeatures2 feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    feat.features.shaderInt64 = 1, feat.features.shaderInt16 = 1, feat.features.fragmentStoresAndAtomics = 1;
    feat.features.vertexPipelineStoresAndAtomics = 1, feat.features.shaderStorageImageReadWithoutFormat = 1;
    feat.features.shaderStorageImageWriteWithoutFormat = 1, feat.features.imageCubeArray = 1, feat.features.sampleRateShading = 1;
    feat.features.shaderClipDistance = 1, feat.features.independentBlend = 1, feat.features.dualSrcBlend = 1;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &feat, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    VkResult vr = vkCreateDevice(pd, &dci, NULL, &dev);
    if (vr) { printf("no device (%d)\n", vr); return 100; }

    const VkDescriptorType types[5] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    const uint32_t counts[5] = {65536, 8192, 4096, 8192, 8192};
    VkDescriptorSetLayoutBinding b[5];
    VkDescriptorBindingFlags bf[5];
    for (int i = 0; i < 5; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = types[i], .descriptorCount = counts[i], .stageFlags = VK_SHADER_STAGE_ALL};
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 5, .pBindingFlags = bf};
    VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = 5, .pBindings = b};
    VkDescriptorSetLayout sl;
    if ((vr = vkCreateDescriptorSetLayout(dev, &sli, NULL, &sl))) { printf("no set layout (%d)\n", vr); return 100; }
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_ALL, .size = 64};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout;
    vkCreatePipelineLayout(dev, &pli, NULL, &layout);
    VkShaderModuleCreateInfo vmi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof position_vs, .pCode = position_vs};
    VkShaderModule position;
    vkCreateShaderModule(dev, &vmi, NULL, &position);

    int failed = 0, done = 0;
    for (int a = 1; a < argc; a++) {
        size_t words;
        uint32_t *code = slurp(argv[a], &words);
        int m = code ? model(code, words) : -1;
        VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
        VkShaderModule mod = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        double t0 = now();
        vr = code ? vkCreateShaderModule(dev, &smi, NULL, &mod) : VK_ERROR_UNKNOWN;
        if (!vr && m == 5) {
            VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
            vr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
        } else if (!vr && (m == 0 || m == 4)) {
            VkPipelineShaderStageCreateInfo st[2] = {
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m == 0 ? mod : position, .pName = "main"},
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mod, .pName = "main"}};
            VkFormat colors[8];
            for (int i = 0; i < 8; i++) colors[i] = VK_FORMAT_R16G16B16A16_SFLOAT;
            VkPipelineColorBlendAttachmentState blends[8] = {{0}};
            for (int i = 0; i < 8; i++) blends[i].colorWriteMask = 0xf;
            VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 8, .pColorAttachmentFormats = colors};
            VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
            VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = m == 0, .lineWidth = 1};
            VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 8, .pAttachments = blends};
            VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
            VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = m == 0 ? 1 : 2,
                                               .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                               .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
                                               .pDynamicState = &ds, .layout = layout};
            vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &pipe);
        } else if (!vr) vr = VK_ERROR_FEATURE_NOT_PRESENT;
        double t1 = now();
        printf("%-48s %-8s %s (%d) %.1f ms\n", argv[a], m == 0 ? "vertex" : m == 4 ? "fragment" : m == 5 ? "compute" : "?",
               vr ? "FAILED" : "ok", vr, (t1 - t0) * 1000);
        failed += vr != 0, done++;
        if (pipe) vkDestroyPipeline(dev, pipe, NULL);
        if (mod) vkDestroyShaderModule(dev, mod, NULL);
        free(code);
    }
    printf("spvcheck: %d modules, %d failed\n", done, failed);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return failed;
}
