/*
 * Arctic web engine: the way a painted page reaches the screen
 *
 * The page goes to the window through a Vulkan swapchain, the way a game
 * does. On Windows that is the GPU's own driver; in Arctic it is vulkan-1 ->
 * winevulkan -> the ICD -> the display driver -> dwm.exe, which is itself a
 * Vulkan compositor (docs/M3-gpu.md). Arctic's Wine is built --without-opengl,
 * so Vulkan is the only accelerated path there is.
 *
 * Until the glyph atlas and the pipelines of stage 3 the rasterizer runs on
 * the processor, so what the swapchain gets is one upload of the whole
 * page. That is already the hard part of the way: the pipelines replace the
 * upload later without the rest of the engine knowing.
 *
 * Anything that fails here is not fatal: the engine falls back to BitBlt,
 * as dwm falls back to pixman, and ARCTIC_HTML_CPU=1 asks for that path.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "arctichtml_private.h"

/* Khronos' own headers (runtime/browser/deps.txt): vulkan-1.dll is loaded at
 * run time, so a machine without a Vulkan driver still starts the engine */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#define INSTANCE_FUNCTIONS \
    USE(vkEnumeratePhysicalDevices) \
    USE(vkGetPhysicalDeviceProperties) \
    USE(vkGetPhysicalDeviceQueueFamilyProperties) \
    USE(vkGetPhysicalDeviceMemoryProperties) \
    USE(vkGetPhysicalDeviceSurfaceSupportKHR) \
    USE(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    USE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    USE(vkCreateWin32SurfaceKHR) \
    USE(vkDestroySurfaceKHR) \
    USE(vkCreateDevice) \
    USE(vkGetDeviceProcAddr)

#define DEVICE_FUNCTIONS \
    USE(vkGetDeviceQueue) \
    USE(vkCreateSwapchainKHR) \
    USE(vkDestroySwapchainKHR) \
    USE(vkGetSwapchainImagesKHR) \
    USE(vkAcquireNextImageKHR) \
    USE(vkQueuePresentKHR) \
    USE(vkCreateCommandPool) \
    USE(vkDestroyCommandPool) \
    USE(vkAllocateCommandBuffers) \
    USE(vkBeginCommandBuffer) \
    USE(vkEndCommandBuffer) \
    USE(vkResetCommandBuffer) \
    USE(vkCmdPipelineBarrier) \
    USE(vkCmdCopyBufferToImage) \
    USE(vkQueueSubmit) \
    USE(vkCreateSemaphore) \
    USE(vkDestroySemaphore) \
    USE(vkCreateFence) \
    USE(vkDestroyFence) \
    USE(vkWaitForFences) \
    USE(vkResetFences) \
    USE(vkDeviceWaitIdle) \
    USE(vkCreateBuffer) \
    USE(vkDestroyBuffer) \
    USE(vkGetBufferMemoryRequirements) \
    USE(vkAllocateMemory) \
    USE(vkFreeMemory) \
    USE(vkBindBufferMemory) \
    USE(vkMapMemory)

static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
#define USE(name) static PFN_##name p_##name;
INSTANCE_FUNCTIONS
DEVICE_FUNCTIONS
#undef USE

/* one instance, one device for the process: the views differ only in their
 * window, and a second device on the same card buys nothing */
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static BOOL vulkan_ready, vulkan_failed;

struct present
{
    HWND            hwnd;
    BOOL            gpu;
    VkSurfaceKHR    surface;
    VkSwapchainKHR  swapchain;
    VkImage         images[8];
    uint32_t        image_count;
    VkFormat        format;
    BOOL            swizzle;        /* the swapchain wants RGBA, the surface is BGRA */
    int             width, height;
    VkCommandPool   pool;
    VkCommandBuffer commands;
    VkSemaphore     acquired, drawn;
    VkFence         idle;
    VkBuffer        staging;
    VkDeviceMemory  staging_memory;
    void           *staging_bits;
    VkDeviceSize    staging_size;
};

static BOOL load_vulkan(void)
{
    static const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "arctichtml",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                  .enabledExtensionCount = ARRAY_SIZE(extensions),
                                  .ppEnabledExtensionNames = extensions };
    HMODULE module;
    VkResult res;

    if (!(module = LoadLibraryW( L"vulkan-1.dll" )) ||
        !(p_vkGetInstanceProcAddr = (void *)GetProcAddress( module, "vkGetInstanceProcAddr" )))
    {
        WARN( "no vulkan-1.dll: the page goes to the window through GDI\n" );
        return FALSE;
    }
    if (!(p_vkCreateInstance = (void *)p_vkGetInstanceProcAddr( NULL, "vkCreateInstance" ))) return FALSE;
    if ((res = p_vkCreateInstance( &info, NULL, &instance )))
    {
        WARN( "no Vulkan instance: %d\n", res );
        return FALSE;
    }
#define USE(name) if (!(p_##name = (void *)p_vkGetInstanceProcAddr( instance, #name ))) return FALSE;
    INSTANCE_FUNCTIONS
#undef USE
    return TRUE;
}

/* the card that can show something in this window, and a device on it */
static BOOL create_device( VkSurfaceKHR surface )
{
    static const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDevice devices[8];
    VkQueueFamilyProperties families[16];
    uint32_t count = ARRAY_SIZE(devices), family_count;
    VkPhysicalDeviceProperties props;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue_info,
                                .enabledExtensionCount = ARRAY_SIZE(extensions),
                                .ppEnabledExtensionNames = extensions };
    VkResult res;

    if (p_vkEnumeratePhysicalDevices( instance, &count, devices ) < 0 || !count)
    {
        WARN( "no Vulkan device\n" );
        return FALSE;
    }
    for (uint32_t i = 0; i < count && !gpu; i++)
    {
        family_count = ARRAY_SIZE(families);
        p_vkGetPhysicalDeviceQueueFamilyProperties( devices[i], &family_count, families );
        for (uint32_t f = 0; f < family_count; f++)
        {
            VkBool32 present = FALSE;

            if (!(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            p_vkGetPhysicalDeviceSurfaceSupportKHR( devices[i], f, surface, &present );
            if (!present) continue;
            gpu = devices[i];
            queue_family = f;
            break;
        }
    }
    if (!gpu)
    {
        WARN( "no card can present to the window\n" );
        return FALSE;
    }
    p_vkGetPhysicalDeviceProperties( gpu, &props );
    TRACE( "page on %s\n", props.deviceName );

    queue_info.queueFamilyIndex = queue_family;
    if ((res = p_vkCreateDevice( gpu, &info, NULL, &device )))
    {
        WARN( "no Vulkan device: %d\n", res );
        gpu = VK_NULL_HANDLE;
        return FALSE;
    }
#define USE(name) if (!(p_##name = (void *)p_vkGetDeviceProcAddr( device, #name ))) return FALSE;
    DEVICE_FUNCTIONS
#undef USE
    p_vkGetDeviceQueue( device, queue_family, 0, &queue );
    return TRUE;
}

static void destroy_staging( struct present *present )
{
    if (present->staging) p_vkDestroyBuffer( device, present->staging, NULL );
    if (present->staging_memory) p_vkFreeMemory( device, present->staging_memory, NULL );
    present->staging = VK_NULL_HANDLE;
    present->staging_memory = VK_NULL_HANDLE;
    present->staging_bits = NULL;
    present->staging_size = 0;
}

static BOOL create_staging( struct present *present, VkDeviceSize size )
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceMemoryProperties memory;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type = ~0u;

    destroy_staging( present );
    if (p_vkCreateBuffer( device, &info, NULL, &present->staging )) return FALSE;
    p_vkGetBufferMemoryRequirements( device, present->staging, &requirements );
    p_vkGetPhysicalDeviceMemoryProperties( gpu, &memory );

    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
    {
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        if (!(requirements.memoryTypeBits & (1u << i))) continue;
        if ((memory.memoryTypes[i].propertyFlags & wanted) != wanted) continue;
        type = i;
        break;
    }
    if (type == ~0u)
    {
        WARN( "no memory the processor can write and the card can read\n" );
        destroy_staging( present );
        return FALSE;
    }
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (p_vkAllocateMemory( device, &allocate, NULL, &present->staging_memory ) ||
        p_vkBindBufferMemory( device, present->staging, present->staging_memory, 0 ) ||
        p_vkMapMemory( device, present->staging_memory, 0, size, 0, &present->staging_bits ))
    {
        destroy_staging( present );
        return FALSE;
    }
    present->staging_size = size;
    return TRUE;
}

/* An image can be acquired and its semaphore left signalled when a frame is
 * given up on, and a signalled semaphore must not be waited on again: the
 * semaphores go with the swapchain. */
static void destroy_swapchain( struct present *present )
{
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    if (!device) return;
    p_vkDeviceWaitIdle( device );
    if (present->swapchain) p_vkDestroySwapchainKHR( device, present->swapchain, NULL );
    present->swapchain = VK_NULL_HANDLE;
    present->image_count = 0;

    if (present->acquired)
    {
        p_vkDestroySemaphore( device, present->acquired, NULL );
        p_vkDestroySemaphore( device, present->drawn, NULL );
        present->acquired = present->drawn = VK_NULL_HANDLE;
        if (p_vkCreateSemaphore( device, &semaphore_info, NULL, &present->acquired ) ||
            p_vkCreateSemaphore( device, &semaphore_info, NULL, &present->drawn ))
        {
            ERR( "no semaphores left: the page goes to the window through GDI\n" );
            present->gpu = FALSE;
        }
    }
}

static BOOL create_swapchain( struct present *present, int width, int height )
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    uint32_t format_count = ARRAY_SIZE(formats);
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                      .surface = present->surface, .imageArrayLayers = 1,
                                      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkResult res;

    destroy_swapchain( present );

    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( gpu, present->surface, &caps ) < 0) return FALSE;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        WARN( "the swapchain takes no copies: GDI then\n" );
        return FALSE;
    }
    if (p_vkGetPhysicalDeviceSurfaceFormatsKHR( gpu, present->surface, &format_count, formats ) < 0 ||
        !format_count)
        return FALSE;

    /* the surface is 0xAARRGGBB in memory, which is B8G8R8A8 to the card:
     * take that format when it is offered and swizzle only if it is not */
    present->format = formats[0].format;
    info.imageColorSpace = formats[0].colorSpace;
    for (uint32_t i = 0; i < format_count; i++)
    {
        if (formats[i].format != VK_FORMAT_B8G8R8A8_UNORM && formats[i].format != VK_FORMAT_B8G8R8A8_SRGB)
            continue;
        present->format = formats[i].format;
        info.imageColorSpace = formats[i].colorSpace;
        break;
    }
    present->swizzle = present->format != VK_FORMAT_B8G8R8A8_UNORM &&
                       present->format != VK_FORMAT_B8G8R8A8_SRGB;

    info.imageFormat = present->format;
    info.minImageCount = max( caps.minImageCount, 2 );
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
    info.imageExtent.width = caps.currentExtent.width == ~0u ? (uint32_t)width : caps.currentExtent.width;
    info.imageExtent.height = caps.currentExtent.height == ~0u ? (uint32_t)height : caps.currentExtent.height;
    info.preTransform = caps.currentTransform;
    if (!info.imageExtent.width || !info.imageExtent.height) return FALSE;

    if ((res = p_vkCreateSwapchainKHR( device, &info, NULL, &present->swapchain )))
    {
        WARN( "no swapchain: %d\n", res );
        return FALSE;
    }
    present->image_count = ARRAY_SIZE(present->images);
    if (p_vkGetSwapchainImagesKHR( device, present->swapchain, &present->image_count, present->images ) < 0)
    {
        destroy_swapchain( present );
        return FALSE;
    }
    present->width = info.imageExtent.width;
    present->height = info.imageExtent.height;

    if (!create_staging( present, (VkDeviceSize)present->width * present->height * 4 ))
    {
        destroy_swapchain( present );
        return FALSE;
    }
    TRACE( "swapchain %dx%d, format %d%s\n", present->width, present->height, present->format,
           present->swizzle ? ", swizzled" : "" );
    return TRUE;
}

static BOOL create_frame( struct present *present )
{
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = queue_family };
    VkCommandBufferAllocateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1 };
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT };

    if (p_vkCreateCommandPool( device, &pool_info, NULL, &present->pool )) return FALSE;
    buffer_info.commandPool = present->pool;
    if (p_vkAllocateCommandBuffers( device, &buffer_info, &present->commands )) return FALSE;
    if (p_vkCreateSemaphore( device, &semaphore_info, NULL, &present->acquired )) return FALSE;
    if (p_vkCreateSemaphore( device, &semaphore_info, NULL, &present->drawn )) return FALSE;
    if (p_vkCreateFence( device, &fence_info, NULL, &present->idle )) return FALSE;
    return TRUE;
}

struct present *present_create( HWND hwnd )
{
    VkWin32SurfaceCreateInfoKHR surface_info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                 .hwnd = hwnd };
    struct present *present;

    if (!(present = calloc( 1, sizeof(*present) ))) return NULL;
    present->hwnd = hwnd;

    if (GetEnvironmentVariableW( L"ARCTIC_HTML_CPU", NULL, 0 ))
    {
        TRACE( "ARCTIC_HTML_CPU: the page goes to the window through GDI\n" );
        return present;
    }
    if (vulkan_failed) return present;
    if (!vulkan_ready)
    {
        if (!load_vulkan())
        {
            vulkan_failed = TRUE;
            return present;
        }
        vulkan_ready = TRUE;
    }

    surface_info.hinstance = (HINSTANCE)GetWindowLongPtrW( hwnd, GWLP_HINSTANCE );
    if (p_vkCreateWin32SurfaceKHR( instance, &surface_info, NULL, &present->surface ))
    {
        WARN( "no Vulkan surface for the window\n" );
        return present;
    }
    if (!device && !create_device( present->surface ))
    {
        p_vkDestroySurfaceKHR( instance, present->surface, NULL );
        present->surface = VK_NULL_HANDLE;
        vulkan_failed = TRUE;
        return present;
    }
    if (!create_frame( present )) return present;

    present->gpu = TRUE;
    return present;
}

void present_destroy( struct present *present )
{
    if (!present) return;
    if (device)
    {
        p_vkDeviceWaitIdle( device );
        destroy_swapchain( present );
        destroy_staging( present );
        if (present->idle) p_vkDestroyFence( device, present->idle, NULL );
        if (present->drawn) p_vkDestroySemaphore( device, present->drawn, NULL );
        if (present->acquired) p_vkDestroySemaphore( device, present->acquired, NULL );
        if (present->pool) p_vkDestroyCommandPool( device, present->pool, NULL );
    }
    if (present->surface && instance) p_vkDestroySurfaceKHR( instance, present->surface, NULL );
    free( present );
}

BOOL present_on_gpu( const struct present *present )
{
    return present && present->gpu;
}

static void copy_to_staging( struct present *present, const struct surface *surface )
{
    int width = min( surface->width, present->width ), height = min( surface->height, present->height );
    DWORD *out = present->staging_bits;

    memset( out, 0, (size_t)present->width * present->height * 4 );
    for (int y = 0; y < height; y++)
    {
        const DWORD *in = surface->bits + (size_t)y * surface->width;
        DWORD *row = out + (size_t)y * present->width;

        if (!present->swizzle) memcpy( row, in, (size_t)width * 4 );
        else for (int x = 0; x < width; x++)
            row[x] = (in[x] & 0xff00ff00) | ((in[x] & 0xff) << 16) | ((in[x] >> 16) & 0xff);
    }
}

static void record( struct present *present, VkImage image )
{
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1,
                                      .layerCount = 1 };
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image,
                                     .subresourceRange = range,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED };
    VkBufferImageCopy copy = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .layerCount = 1 },
                               .imageExtent = { present->width, present->height, 1 } };

    p_vkResetCommandBuffer( present->commands, 0 );
    p_vkBeginCommandBuffer( present->commands, &begin );

    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p_vkCmdPipelineBarrier( present->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );

    p_vkCmdCopyBufferToImage( present->commands, present->staging, image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    p_vkCmdPipelineBarrier( present->commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );

    p_vkEndCommandBuffer( present->commands );
}

BOOL present_surface( struct present *present, const struct surface *surface )
{
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                            .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                            .signalSemaphoreCount = 1 };
    VkPresentInfoKHR info = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                              .swapchainCount = 1 };
    uint32_t index = 0;
    VkResult res;

    if (!present || !present->gpu) return FALSE;

    if (!present->swapchain || present->width != surface->width || present->height != surface->height)
    {
        if (!create_swapchain( present, surface->width, surface->height ))
        {
            /* the window may simply have no size yet; keep the GPU path and
             * let the next paint try again, but say so once */
            WARN( "no swapchain for %dx%d\n", surface->width, surface->height );
            return FALSE;
        }
    }

    p_vkWaitForFences( device, 1, &present->idle, VK_TRUE, ~0ull );
    res = p_vkAcquireNextImageKHR( device, present->swapchain, ~0ull, present->acquired,
                                   VK_NULL_HANDLE, &index );
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
    {
        destroy_swapchain( present );
        return FALSE;
    }
    if (res < 0)
    {
        WARN( "the swapchain gives no image: %d\n", res );
        return FALSE;
    }
    p_vkResetFences( device, 1, &present->idle );

    copy_to_staging( present, surface );
    record( present, present->images[index] );
    submit.pWaitSemaphores = &present->acquired;
    submit.pCommandBuffers = &present->commands;
    submit.pSignalSemaphores = &present->drawn;
    if ((res = p_vkQueueSubmit( queue, 1, &submit, present->idle )))
    {
        /* nothing waits on the semaphore the acquire signalled any more, so
         * this path cannot be retried: finish the page through GDI */
        ERR( "the queue takes nothing: %d\n", res );
        p_vkResetFences( device, 1, &present->idle );
        present->gpu = FALSE;
        return FALSE;
    }
    info.pWaitSemaphores = &present->drawn;
    info.pSwapchains = &present->swapchain;
    info.pImageIndices = &index;
    if ((res = p_vkQueuePresentKHR( queue, &info )) < 0)
    {
        if (res == VK_ERROR_OUT_OF_DATE_KHR) destroy_swapchain( present );
        return FALSE;
    }
    return TRUE;
}
