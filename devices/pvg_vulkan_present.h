/* Android WSI owns a separate device and thread. A one-frame mailbox replaces pending display
   updates when presentation is busy; the guest command worker never waits for vsync. */
#if defined(__ANDROID__)
#include <android/native_window.h>

typedef struct {
    pthread_mutex_t lock, mailbox;
    pthread_cond_t wake;
    pthread_t thread;
    int stop, pending;
    ANativeWindow *window;
    uint8_t *pixels, *next_pixels;
    uint32_t next_w, next_h;
    uint32_t w, h;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swap;
    VkImage *images;
    uint32_t count;
    VkExtent2D extent;
    VkSurfaceTransformFlagBitsKHR transform;
    VkFormat format;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkSemaphore acquired, ready;
    VkFence fence;
    VkBuffer upload;
    VkDeviceMemory memory;
    uint8_t *map;
} Display;

static void display_drop_swap(Display *d) {
    if (!d->device) return;
    vkDeviceWaitIdle(d->device);
    if (d->map) vkUnmapMemory(d->device, d->memory);
    if (d->upload) vkDestroyBuffer(d->device, d->upload, NULL);
    if (d->memory) vkFreeMemory(d->device, d->memory, NULL);
    if (d->swap) vkDestroySwapchainKHR(d->device, d->swap, NULL);
    free(d->images);
    d->images = NULL; d->swap = VK_NULL_HANDLE;
    d->upload = VK_NULL_HANDLE; d->memory = VK_NULL_HANDLE; d->map = NULL;
}

static void display_detach(Display *d) {
    display_drop_swap(d);
    if (d->surface) vkDestroySurfaceKHR(d->instance, d->surface, NULL);
    d->surface = VK_NULL_HANDLE;
}

static int display_swap(Display *d) {
    VkBool32 supported = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(d->physical, d->family, d->surface, &supported);
    if (!supported) return -1;
    VkSurfaceCapabilitiesKHR cap;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, d->surface, &cap)) return -1;
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return -1;
    d->extent = cap.currentExtent;
    if (d->extent.width == UINT32_MAX) {
        d->extent.width = ANativeWindow_getWidth(d->window);
        d->extent.height = ANativeWindow_getHeight(d->window);
    }
    if (!d->extent.width || !d->extent.height) return -1;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, d->surface, &n, NULL);
    VkSurfaceFormatKHR *formats = calloc(n, sizeof *formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->physical, d->surface, &n, formats);
    VkSurfaceFormatKHR chosen = {0};
    for (uint32_t i = 0; i < n; i++)
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = formats[i]; break; }
    free(formats);
    if (!chosen.format) return -1; /* display-encoded bytes must not be encoded again */
    d->format = chosen.format;
    d->transform = cap.currentTransform;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & alpha)) alpha = cap.supportedCompositeAlpha & -cap.supportedCompositeAlpha;
    uint32_t count = cap.minImageCount + 1;
    if (cap.maxImageCount && count > cap.maxImageCount) count = cap.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface=d->surface, .minImageCount=count, .imageFormat=chosen.format, .imageColorSpace=chosen.colorSpace,
        .imageExtent=d->extent, .imageArrayLayers=1, .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE, .preTransform=d->transform,
        .compositeAlpha=alpha, .presentMode=VK_PRESENT_MODE_FIFO_KHR, .clipped=VK_TRUE};
    if (vkCreateSwapchainKHR(d->device, &ci, NULL, &d->swap)) return -1;
    vkGetSwapchainImagesKHR(d->device, d->swap, &d->count, NULL);
    d->images = calloc(d->count, sizeof *d->images);
    vkGetSwapchainImagesKHR(d->device, d->swap, &d->count, d->images);
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=(VkDeviceSize)d->extent.width*d->extent.height*4, .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(d->device, &bi, NULL, &d->upload)) goto fail;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d->device, d->upload, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(d->physical, &mp);
    uint32_t type;
    for (type=0; type<mp.memoryTypeCount; type++)
        if ((req.memoryTypeBits & (1u<<type)) &&
            (mp.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (type == mp.memoryTypeCount) goto fail;
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=req.size, .memoryTypeIndex=type};
    if (vkAllocateMemory(d->device, &ai, NULL, &d->memory) || vkBindBufferMemory(d->device,d->upload,d->memory,0) ||
        vkMapMemory(d->device,d->memory,0,VK_WHOLE_SIZE,0,(void **)&d->map)) goto fail;
    fprintf(stderr,"pvg-vulkan: window %ux%u, transform %u, format %u\n",d->extent.width,d->extent.height,d->transform,d->format);
    return 0;
fail:
    display_drop_swap(d); return -1;
}

static void display_draw(Display *d) {
    if (!d->window || !d->pixels) return;
    if (!d->surface) {
        VkAndroidSurfaceCreateInfoKHR si = {.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window=d->window};
        if (vkCreateAndroidSurfaceKHR(d->instance,&si,NULL,&d->surface)) return;
    }
    if (!d->swap && display_swap(d)) return;
    uint32_t image;
    VkResult r = vkAcquireNextImageKHR(d->device,d->swap,0,d->acquired,VK_NULL_HANDLE,&image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { display_drop_swap(d); return; }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { display_detach(d); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    uint32_t W=d->extent.width,H=d->extent.height;
    int rotated = d->transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                  d->transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    float logical_w = rotated ? H : W, logical_h = rotated ? W : H;
    float scale = fminf(logical_w / d->w, logical_h / d->h);
    float left = (logical_w - d->w * scale) * .5f;
    float top = (logical_h - d->h * scale) * .5f;
    for (uint32_t y=0;y<H;y++) for (uint32_t x=0;x<W;x++) {
        /* Rotate into the native orientation; SurfaceFlinger applies preTransform. */
        float u=(x+.5f)/W,v=(y+.5f)/H,s=u,t=v;
        switch (d->transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: s=1-v; t=u; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: s=1-u; t=1-v; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: s=v; t=1-u; break;
        default: break;
        }
        float fx = (s * logical_w - left) / scale, fy = (t * logical_h - top) / scale;
        uint8_t *o=d->map+((size_t)y*W+x)*4;
        if (fx < 0 || fy < 0 || fx >= d->w || fy >= d->h) { memset(o,0,3); o[3]=255; continue; }
        uint32_t sx=(uint32_t)fx,sy=(uint32_t)fy;
        const uint8_t *p=d->pixels+((size_t)sy*d->w+sx)*4;
        if (d->format==VK_FORMAT_R8G8B8A8_UNORM) o[0]=p[2],o[1]=p[1],o[2]=p[0],o[3]=255;
        else memcpy(o,p,4);
    }
    vkResetCommandPool(d->device,d->pool,0);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(d->cb,&begin);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=d->images[image],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(d->cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
    vkCmdCopyBufferToImage(d->cb,d->upload,d->images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(d->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    vkEndCommandBuffer(d->cb);
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&d->acquired,
        .pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&d->cb,.signalSemaphoreCount=1,.pSignalSemaphores=&d->ready};
    vkResetFences(d->device,1,&d->fence);
    if (vkQueueSubmit(d->queue,1,&submit,d->fence)) { display_detach(d); return; }
    VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&d->ready,
        .swapchainCount=1,.pSwapchains=&d->swap,.pImageIndices=&image};
    r=vkQueuePresentKHR(d->queue,&pi);
    vkWaitForFences(d->device,1,&d->fence,VK_TRUE,UINT64_MAX);
    /* Also retire the presentation wait before reusing its binary semaphore. */
    vkQueueWaitIdle(d->queue);
    if (r==VK_ERROR_SURFACE_LOST_KHR) display_detach(d);
    else if (r==VK_ERROR_OUT_OF_DATE_KHR || r==VK_SUBOPTIMAL_KHR) display_drop_swap(d);
}

static void *display_worker(void *arg) {
    Display *d=arg;
    pthread_mutex_lock(&d->mailbox);
    while (!d->stop) {
        while (!d->pending && !d->stop) pthread_cond_wait(&d->wake,&d->mailbox);
        if (d->stop) break;
        d->pending=0;
        if (d->next_pixels) {
            free(d->pixels); d->pixels=d->next_pixels; d->next_pixels=NULL;
            d->w=d->next_w; d->h=d->next_h;
        }
        pthread_mutex_unlock(&d->mailbox);
        pthread_mutex_lock(&d->lock);
        display_draw(d);
        pthread_mutex_unlock(&d->lock);
        pthread_mutex_lock(&d->mailbox);
    }
    pthread_mutex_unlock(&d->mailbox); return NULL;
}

static Display *display_create(void) {
    Display *d=calloc(1,sizeof *d);
    const char *ext[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=2,.ppEnabledExtensionNames=ext};
    if (vkCreateInstance(&ici,NULL,&d->instance)) goto fail;
    uint32_t n=1; if (vkEnumeratePhysicalDevices(d->instance,&n,&d->physical) || !n) goto fail;
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(d->physical,&nq,NULL);
    VkQueueFamilyProperties *q=calloc(nq,sizeof *q); vkGetPhysicalDeviceQueueFamilyProperties(d->physical,&nq,q);
    for (d->family=0;d->family<nq && !(q[d->family].queueFlags&VK_QUEUE_GRAPHICS_BIT);d->family++);
    free(q); if (d->family==nq) goto fail;
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=d->family,.queueCount=1,.pQueuePriorities=&priority};
    const char *swap=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&swap};
    if (vkCreateDevice(d->physical,&di,NULL,&d->device)) goto fail;
    vkGetDeviceQueue(d->device,d->family,0,&d->queue);
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=d->family};
    if (vkCreateCommandPool(d->device,&pool,NULL,&d->pool)) goto fail;
    VkCommandBufferAllocateInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=d->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if (vkAllocateCommandBuffers(d->device,&cb,&d->cb)) goto fail;
    VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateSemaphore(d->device,&sem,NULL,&d->acquired) || vkCreateSemaphore(d->device,&sem,NULL,&d->ready) || vkCreateFence(d->device,&fence,NULL,&d->fence)) goto fail;
    pthread_mutex_init(&d->lock,NULL); pthread_mutex_init(&d->mailbox,NULL); pthread_cond_init(&d->wake,NULL);
    pthread_create(&d->thread,NULL,display_worker,d); return d;
fail:
    if(d->device) {
        if(d->acquired) vkDestroySemaphore(d->device,d->acquired,NULL);
        if(d->ready) vkDestroySemaphore(d->device,d->ready,NULL);
        if(d->fence) vkDestroyFence(d->device,d->fence,NULL);
        if(d->pool) vkDestroyCommandPool(d->device,d->pool,NULL);
        vkDestroyDevice(d->device,NULL);
    }
    if(d->instance) vkDestroyInstance(d->instance,NULL);
    free(d); return NULL;
}

static void display_window(Display *d, ANativeWindow *window) {
    if (!d) return;
    if (window) ANativeWindow_acquire(window);
    pthread_mutex_lock(&d->lock);
    display_detach(d);
    if (d->window) ANativeWindow_release(d->window);
    d->window=window;
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_lock(&d->mailbox); d->pending=!!window;
    pthread_cond_signal(&d->wake); pthread_mutex_unlock(&d->mailbox);
}

static void display_send(Display *d,const uint8_t *pixels,uint32_t w,uint32_t h) {
    if (!d) return;
    pthread_mutex_lock(&d->mailbox);
    uint8_t *p=realloc(d->next_pixels,(size_t)w*h*4);
    if(p) {
        d->next_pixels=p; memcpy(p,pixels,(size_t)w*h*4);
        d->next_w=w; d->next_h=h; d->pending=1; pthread_cond_signal(&d->wake);
    }
    pthread_mutex_unlock(&d->mailbox);
}

static void display_destroy(Display *d) {
    if (!d) return;
    pthread_mutex_lock(&d->mailbox); d->stop=1; pthread_cond_signal(&d->wake); pthread_mutex_unlock(&d->mailbox);
    pthread_join(d->thread,NULL); display_window(d,NULL);
    vkDestroySemaphore(d->device,d->acquired,NULL); vkDestroySemaphore(d->device,d->ready,NULL);
    vkDestroyFence(d->device,d->fence,NULL); vkDestroyCommandPool(d->device,d->pool,NULL);
    vkDestroyDevice(d->device,NULL); vkDestroyInstance(d->instance,NULL);
    pthread_mutex_destroy(&d->lock); pthread_mutex_destroy(&d->mailbox); pthread_cond_destroy(&d->wake);
    free(d->pixels); free(d->next_pixels); free(d);
}
#endif
