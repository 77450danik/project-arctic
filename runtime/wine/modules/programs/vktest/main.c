/*
 * Vulkan test window
 *
 * Draws a changing colour through a Vulkan swapchain, to see that the path
 * from a Windows program to the display works: vulkan-1 -> winevulkan ->
 * the ICD -> the display driver -> dwm.exe. Prints the device it runs on.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "wine/vulkan.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vktest);

#define FRAMES 3

static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_vkGetPhysicalDeviceSurfaceSupportKHR;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_vkGetPhysicalDeviceSurfaceFormatsKHR;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
static PFN_vkCreateWin32SurfaceKHR p_vkCreateWin32SurfaceKHR;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkCreateSwapchainKHR p_vkCreateSwapchainKHR;
static PFN_vkGetSwapchainImagesKHR p_vkGetSwapchainImagesKHR;
static PFN_vkAcquireNextImageKHR p_vkAcquireNextImageKHR;
static PFN_vkQueuePresentKHR p_vkQueuePresentKHR;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdClearColorImage p_vkCmdClearColorImage;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkCreateSemaphore p_vkCreateSemaphore;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkResetFences p_vkResetFences;
static PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle;

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage images[8];
static uint32_t image_count;
static VkCommandBuffer commands[FRAMES];
static VkSemaphore acquired[FRAMES], drawn[FRAMES];
static VkFence idle[FRAMES];
static uint32_t queue_family;

#define GET_INSTANCE(name) p_##name = (void *)p_vkGetInstanceProcAddr( instance, #name ); if (!p_##name) return FALSE
#define GET_DEVICE(name) p_##name = (void *)p_vkGetDeviceProcAddr( device, #name ); if (!p_##name) return FALSE

static BOOL load_vulkan(void)
{
    HMODULE module = LoadLibraryW( L"vulkan-1.dll" );

    if (!module || !(p_vkGetInstanceProcAddr = (void *)GetProcAddress( module, "vkGetInstanceProcAddr" )))
    {
        ERR( "vulkan-1.dll is missing\n" );
        return FALSE;
    }
    GET_INSTANCE(vkCreateInstance);
    return TRUE;
}

static BOOL create_instance(void)
{
    static const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktest",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                  .enabledExtensionCount = ARRAY_SIZE(extensions),
                                  .ppEnabledExtensionNames = extensions };
    VkResult res;

    if ((res = p_vkCreateInstance( &info, NULL, &instance )))
    {
        ERR( "no Vulkan instance: %d\n", res );
        return FALSE;
    }
    GET_INSTANCE(vkEnumeratePhysicalDevices);
    GET_INSTANCE(vkGetPhysicalDeviceProperties);
    GET_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    GET_INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
    GET_INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);
    GET_INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    GET_INSTANCE(vkCreateWin32SurfaceKHR);
    GET_INSTANCE(vkCreateDevice);
    GET_INSTANCE(vkGetDeviceProcAddr);
    return TRUE;
}

static BOOL pick_device(void)
{
    VkPhysicalDevice devices[8];
    VkQueueFamilyProperties families[16];
    uint32_t count = ARRAY_SIZE(devices), family_count;
    VkPhysicalDeviceProperties props;

    if (p_vkEnumeratePhysicalDevices( instance, &count, devices ) < 0 || !count)
    {
        ERR( "no Vulkan device\n" );
        return FALSE;
    }
    for (uint32_t i = 0; i < count; i++)
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
            p_vkGetPhysicalDeviceProperties( gpu, &props );
            MESSAGE( "vktest: %s\n", props.deviceName );
            return TRUE;
        }
    }
    ERR( "no device can present to the window\n" );
    return FALSE;
}

static BOOL create_device(void)
{
    static const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = queue_family, .queueCount = 1,
                                           .pQueuePriorities = &priority };
    VkDeviceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue_info,
                                .enabledExtensionCount = ARRAY_SIZE(extensions),
                                .ppEnabledExtensionNames = extensions };
    VkResult res;

    if ((res = p_vkCreateDevice( gpu, &info, NULL, &device )))
    {
        ERR( "no Vulkan device: %d\n", res );
        return FALSE;
    }
    GET_DEVICE(vkGetDeviceQueue);
    GET_DEVICE(vkCreateSwapchainKHR);
    GET_DEVICE(vkGetSwapchainImagesKHR);
    GET_DEVICE(vkAcquireNextImageKHR);
    GET_DEVICE(vkQueuePresentKHR);
    GET_DEVICE(vkCreateCommandPool);
    GET_DEVICE(vkAllocateCommandBuffers);
    GET_DEVICE(vkBeginCommandBuffer);
    GET_DEVICE(vkEndCommandBuffer);
    GET_DEVICE(vkCmdClearColorImage);
    GET_DEVICE(vkCmdPipelineBarrier);
    GET_DEVICE(vkQueueSubmit);
    GET_DEVICE(vkCreateSemaphore);
    GET_DEVICE(vkCreateFence);
    GET_DEVICE(vkWaitForFences);
    GET_DEVICE(vkResetFences);
    GET_DEVICE(vkDeviceWaitIdle);
    p_vkGetDeviceQueue( device, queue_family, 0, &queue );
    return TRUE;
}

static BOOL create_swapchain( HWND hwnd )
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[16];
    uint32_t format_count = ARRAY_SIZE(formats);
    VkSwapchainCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                      .imageArrayLayers = 1,
                                      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = queue_family };
    VkCommandBufferAllocateInfo buffer_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = FRAMES };
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkCommandPool pool;
    RECT rect;
    VkResult res;

    if (p_vkGetPhysicalDeviceSurfaceFormatsKHR( gpu, surface, &format_count, formats ) < 0 || !format_count)
        return FALSE;
    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( gpu, surface, &caps ) < 0) return FALSE;

    GetClientRect( hwnd, &rect );
    info.minImageCount = max( caps.minImageCount, 2 );
    info.imageFormat = formats[0].format;
    info.imageColorSpace = formats[0].colorSpace;
    info.imageExtent.width = caps.currentExtent.width == ~0u ? (uint32_t)rect.right : caps.currentExtent.width;
    info.imageExtent.height = caps.currentExtent.height == ~0u ? (uint32_t)rect.bottom : caps.currentExtent.height;
    info.preTransform = caps.currentTransform;

    if ((res = p_vkCreateSwapchainKHR( device, &info, NULL, &swapchain )))
    {
        ERR( "no swapchain: %d\n", res );
        return FALSE;
    }
    image_count = ARRAY_SIZE(images);
    if (p_vkGetSwapchainImagesKHR( device, swapchain, &image_count, images ) < 0) return FALSE;

    if (p_vkCreateCommandPool( device, &pool_info, NULL, &pool )) return FALSE;
    buffer_info.commandPool = pool;
    if (p_vkAllocateCommandBuffers( device, &buffer_info, commands )) return FALSE;
    for (int i = 0; i < FRAMES; i++)
    {
        if (p_vkCreateSemaphore( device, &semaphore_info, NULL, &acquired[i] )) return FALSE;
        if (p_vkCreateSemaphore( device, &semaphore_info, NULL, &drawn[i] )) return FALSE;
        if (p_vkCreateFence( device, &fence_info, NULL, &idle[i] )) return FALSE;
    }
    return TRUE;
}

/* clears the image to the colour and leaves it ready to present */
static void record( VkCommandBuffer commands, VkImage image, const VkClearColorValue *color )
{
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image,
                                     .subresourceRange = range,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED };

    p_vkBeginCommandBuffer( commands, &begin );

    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p_vkCmdPipelineBarrier( commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 0, NULL, 1, &barrier );

    p_vkCmdClearColorImage( commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, color, 1, &range );

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    p_vkCmdPipelineBarrier( commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, NULL, 0, NULL, 1, &barrier );

    p_vkEndCommandBuffer( commands );
}

static void draw( int frame )
{
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkClearColorValue color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &acquired[frame % FRAMES], .pWaitDstStageMask = &stage,
                            .commandBufferCount = 1, .pCommandBuffers = &commands[frame % FRAMES],
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &drawn[frame % FRAMES] };
    VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &drawn[frame % FRAMES], .swapchainCount = 1,
                                 .pSwapchains = &swapchain };
    uint32_t index = 0;

    p_vkWaitForFences( device, 1, &idle[frame % FRAMES], VK_TRUE, ~0ull );
    p_vkResetFences( device, 1, &idle[frame % FRAMES] );
    if (p_vkAcquireNextImageKHR( device, swapchain, ~0ull, acquired[frame % FRAMES], VK_NULL_HANDLE, &index ) < 0)
        return;

    color.float32[0] = (float)((frame / 2) % 60) / 60.0f;
    color.float32[1] = 0.35f;
    color.float32[2] = 1.0f - color.float32[0];
    record( commands[frame % FRAMES], images[index], &color );
    p_vkQueueSubmit( queue, 1, &submit, idle[frame % FRAMES] );

    present.pImageIndices = &index;
    p_vkQueuePresentKHR( queue, &present );
}

static LRESULT WINAPI window_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_DESTROY) PostQuitMessage( 0 );
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI wWinMain( HINSTANCE instance_handle, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WNDCLASSW class = { .lpfnWndProc = window_proc, .hInstance = instance_handle,
                        .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ), .lpszClassName = L"vktest" };
    VkWin32SurfaceCreateInfoKHR surface_info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    int frame = 0;
    HWND hwnd;
    MSG msg;

    if (!load_vulkan() || !create_instance()) return 1;
    RegisterClassW( &class );
    hwnd = CreateWindowExW( 0, class.lpszClassName, L"Vulkan", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, 0, 0, instance_handle, NULL );
    if (!hwnd) return 1;

    surface_info.hinstance = instance_handle;
    surface_info.hwnd = hwnd;
    if (p_vkCreateWin32SurfaceKHR( instance, &surface_info, NULL, &surface ))
    {
        ERR( "no Vulkan surface for the window\n" );
        return 1;
    }
    if (!pick_device() || !create_device() || !create_swapchain( hwnd )) return 1;

    for (;;)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT)
            {
                p_vkDeviceWaitIdle( device );
                return 0;
            }
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        draw( frame++ );
    }
}
