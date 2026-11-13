/* What the device's Vulkan driver offers that the graphics backend depends on (docs/android/graphics.md,
   section 9). Run on the device: adb push vkprobe /data/local/tmp/menhir-gfx/ && adb shell .../vkprobe [-x] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const struct { VkFormat f; const char *name; } formats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"}, {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM"}, {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM"},
    {VK_FORMAT_R8_UNORM, "R8_UNORM"}, {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM"}, {VK_FORMAT_R16_UNORM, "R16_UNORM"},
    {VK_FORMAT_R16G16_UNORM, "R16G16_UNORM"}, {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"}, {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT"}, {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "E5B9G9R9_UFLOAT"},
    {VK_FORMAT_B5G6R5_UNORM_PACK16, "B5G6R5_UNORM"}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5_UNORM"},
    {VK_FORMAT_B4G4R4A4_UNORM_PACK16, "B4G4R4A4_UNORM"}, {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
    {VK_FORMAT_D16_UNORM, "D16_UNORM"}, {VK_FORMAT_S8_UINT, "S8_UINT"}, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA"},
    {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"}, {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"}, {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4"},
    {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA8"}, {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "G8_B8R8_2PLANE_420"},
};

int main(int argc, char **argv) {
    int all_ext = argc > 1 && !strcmp(argv[1], "-x");
    uint32_t api = 0;
    vkEnumerateInstanceVersion(&api);
    printf("instance %u.%u.%u\n", VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api));
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3, .pApplicationName = "vkprobe"};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("no instance\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice devs[4];
    if (n > 4) n = 4;
    vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p12};
        drv.pNext = &p11;
        VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        vkGetPhysicalDeviceProperties2(devs[d], &pr);
        VkPhysicalDeviceProperties *p = &pr.properties;
        printf("device %s, API %u.%u.%u, driver %s %s (version %#x), vendor %#x device %#x\n", p->deviceName, VK_VERSION_MAJOR(p->apiVersion),
               VK_VERSION_MINOR(p->apiVersion), VK_VERSION_PATCH(p->apiVersion), drv.driverName, drv.driverInfo, p->driverVersion, p->vendorID, p->deviceID);
        VkPhysicalDeviceLimits *l = &p->limits;
        printf("limits: image2D %u, push constants %u, bound sets %u, sampler allocs %u, alloc count %u, storage buffer range %u, uniform range %u, "
               "per-stage samplers %u, per-stage sampled images %u, per-stage storage images %u, color attachments %u, viewports %u, "
               "compute shared %u, compute invocations %u, min storage align %llu, min uniform align %llu, timestamp period %.2f\n",
               l->maxImageDimension2D, l->maxPushConstantsSize, l->maxBoundDescriptorSets, l->maxSamplerAllocationCount, l->maxMemoryAllocationCount,
               l->maxStorageBufferRange, l->maxUniformBufferRange, l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorSampledImages,
               l->maxPerStageDescriptorStorageImages, l->maxColorAttachments, l->maxViewports, l->maxComputeSharedMemorySize,
               l->maxComputeWorkGroupInvocations, (unsigned long long)l->minStorageBufferOffsetAlignment,
               (unsigned long long)l->minUniformBufferOffsetAlignment, l->timestampPeriod);
        printf("subgroup size %u, stages %#x, ops %#x; update-after-bind sampled images %u, samplers %u, storage images %u\n", p11.subgroupSize,
               p11.subgroupSupportedStages, p11.subgroupSupportedOperations, p12.maxDescriptorSetUpdateAfterBindSampledImages,
               p12.maxDescriptorSetUpdateAfterBindSamplers, p12.maxDescriptorSetUpdateAfterBindStorageImages);
        printf("float controls: denorm independence %d, rounding independence %d, preserve denorm 16/32 %d/%d, flush denorm 16/32 %d/%d\n",
               p12.denormBehaviorIndependence, p12.roundingModeIndependence, p12.shaderDenormPreserveFloat16, p12.shaderDenormPreserveFloat32,
               p12.shaderDenormFlushToZeroFloat16, p12.shaderDenormFlushToZeroFloat32);

        VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
        VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
        vkGetPhysicalDeviceFeatures2(devs[d], &f);
#define F(s, x) printf("  %-44s %d\n", #x, (int)s.x)
        printf("features:\n");
        F(f.features, shaderInt64); F(f.features, shaderInt16); F(f.features, shaderFloat64); F(f.features, independentBlend);
        F(f.features, dualSrcBlend); F(f.features, logicOp); F(f.features, multiDrawIndirect); F(f.features, drawIndirectFirstInstance);
        F(f.features, depthClamp); F(f.features, depthBiasClamp); F(f.features, fillModeNonSolid); F(f.features, wideLines);
        F(f.features, multiViewport); F(f.features, samplerAnisotropy); F(f.features, textureCompressionBC); F(f.features, textureCompressionASTC_LDR);
        F(f.features, tessellationShader); F(f.features, geometryShader); F(f.features, fragmentStoresAndAtomics);
        F(f.features, vertexPipelineStoresAndAtomics); F(f.features, shaderStorageImageWriteWithoutFormat);
        F(f.features, shaderStorageImageReadWithoutFormat); F(f.features, imageCubeArray); F(f.features, sampleRateShading);
        F(f.features, shaderClipDistance); F(f.features, robustBufferAccess); F(f.features, fullDrawIndexUint32);
        F(f11, storageBuffer16BitAccess); F(f11, multiview); F(f11, shaderDrawParameters); F(f11, samplerYcbcrConversion);
        F(f12, bufferDeviceAddress); F(f12, descriptorIndexing); F(f12, runtimeDescriptorArray); F(f12, descriptorBindingPartiallyBound);
        F(f12, descriptorBindingSampledImageUpdateAfterBind); F(f12, descriptorBindingStorageImageUpdateAfterBind);
        F(f12, descriptorBindingVariableDescriptorCount); F(f12, shaderSampledImageArrayNonUniformIndexing);
        F(f12, shaderStorageImageArrayNonUniformIndexing); F(f12, timelineSemaphore); F(f12, shaderFloat16); F(f12, shaderInt8);
        F(f12, storageBuffer8BitAccess); F(f12, vulkanMemoryModel); F(f12, vulkanMemoryModelDeviceScope); F(f12, scalarBlockLayout);
        F(f12, shaderOutputLayer); F(f12, shaderOutputViewportIndex); F(f12, samplerMirrorClampToEdge); F(f12, hostQueryReset);
        F(f12, drawIndirectCount); F(f12, shaderBufferInt64Atomics); F(f12, separateDepthStencilLayouts); F(f12, imagelessFramebuffer);
        F(f13, dynamicRendering); F(f13, synchronization2); F(f13, maintenance4); F(f13, shaderDemoteToHelperInvocation);
        F(f13, subgroupSizeControl); F(f13, computeFullSubgroups); F(f13, inlineUniformBlock); F(f13, shaderIntegerDotProduct);

        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &ne, NULL);
        VkExtensionProperties *ext = calloc(ne, sizeof *ext);
        vkEnumerateDeviceExtensionProperties(devs[d], NULL, &ne, ext);
        static const char *wanted[] = {"VK_KHR_swapchain", "VK_EXT_external_memory_host", "VK_EXT_descriptor_buffer", "VK_EXT_extended_dynamic_state",
                                       "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3", "VK_EXT_custom_border_color",
                                       "VK_EXT_border_color_swizzle", "VK_EXT_image_view_min_lod", "VK_EXT_depth_clip_enable",
                                       "VK_EXT_shader_stencil_export", "VK_EXT_fragment_shader_interlock", "VK_KHR_fragment_shader_barycentric",
                                       "VK_EXT_rasterization_order_attachment_access", "VK_KHR_dynamic_rendering_local_read",
                                       "VK_ANDROID_external_memory_android_hardware_buffer", "VK_KHR_external_memory_fd",
                                       "VK_KHR_external_fence_fd", "VK_KHR_external_semaphore_fd", "VK_EXT_shader_atomic_float",
                                       "VK_KHR_shader_float_controls2", "VK_EXT_robustness2", "VK_EXT_4444_formats", "VK_KHR_maintenance5",
                                       "VK_EXT_line_rasterization", "VK_EXT_primitive_topology_list_restart", "VK_KHR_pipeline_library",
                                       "VK_EXT_graphics_pipeline_library", "VK_EXT_shader_object", "VK_EXT_mutable_descriptor_type",
                                       "VK_KHR_shader_subgroup_extended_types", "VK_EXT_index_type_uint8", "VK_KHR_push_descriptor",
                                       "VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor", "VK_EXT_pipeline_creation_cache_control"};
        printf("%u device extensions; of those the backend may use:\n", ne);
        for (size_t w = 0; w < sizeof wanted / sizeof *wanted; w++) {
            int have = 0;
            for (uint32_t i = 0; i < ne; i++)
                if (!strcmp(ext[i].extensionName, wanted[w])) have = 1;
            printf("  %-48s %s\n", wanted[w], have ? "yes" : "no");
        }
        if (all_ext)
            for (uint32_t i = 0; i < ne; i++) printf("ext %s %u\n", ext[i].extensionName, ext[i].specVersion);
        free(ext);
        printf("formats (optimal tiling: S sampled, F filter, C color attachment, B blend, D depth, W storage; buffer: V vertex):\n");
        for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(devs[d], formats[i].f, &fp);
            VkFormatFeatureFlags o = fp.optimalTilingFeatures;
            printf("  %-22s %c%c%c%c%c%c %c\n", formats[i].name, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? 'S' : '-',
                   o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? 'F' : '-', o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? 'C' : '-',
                   o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT ? 'B' : '-', o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'D' : '-',
                   o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? 'W' : '-', fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ? 'V' : '-');
        }
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[d], &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++) printf("heap %u: %llu MB flags %#x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf("memory type %u: heap %u flags %#x\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
    }
    vkDestroyInstance(inst, NULL);
    return 0;
}
