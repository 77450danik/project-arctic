/*
 * Arctic desktop composition engine: composition on the GPU
 *
 * The frames of every monitor are drawn with Vulkan, as the Windows desktop
 * is drawn with Direct3D: on the card dwm drives, into images that card
 * scans out, flipped to the monitor without a copy. Windows drawn with the
 * GPU (Vulkan, Direct3D through DXVK) are sampled straight from their
 * buffers; windows drawn by GDI are uploaded as textures when they change.
 * The effects of Windows 11 are drawn here: rounded corners, the acrylic
 * backdrop (the desktop behind a window blurred, tinted and grained),
 * windows zoomed and faded while they come and go, and live thumbnails for
 * DwmRegisterThumbnail.
 *
 * A card whose images cannot be scanned out, or no Vulkan driver for the
 * card at all (a virtual machine): the frames are drawn the same way, by
 * that card or by the software rasterizer, and copied into the card's dumb
 * buffers. Only without any Vulkan does the compositor compose on the CPU.
 *
 * Everything is drawn in step with the caller: a frame is recorded, given
 * to the GPU and waited for. Nothing of a frame outlives the call that drew
 * it, so what a frame used may be freed as soon as the call returns.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-server.h>

#include <vulkan/vulkan.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "wine/debug.h"

#include "compositor.h"
#include "vk_shaders.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

#define BLUR_LEVELS   4     /* halvings of the dual Kawase blur, at most */
#define BLUR_DEFAULT  3     /* ... and by default */
#define BLUR_OFFSET   2.0f
#define BLUR_MARGIN   64    /* what lies this far outside a window still blurs into its backdrop */
#define BLUR_CHAINS   4     /* backdrops of that many sizes keep their images */
#define MAX_FRAMES    32
#define OUTPUT_FRAMES 3     /* on screen, about to be, being drawn */
#define FORMAT        VK_FORMAT_B8G8R8A8_UNORM  /* XRGB8888 and ARGB8888 of DRM and of GDI */
#define WAIT_NS       5000000000ull

enum { SAMPLER_LINEAR, SAMPLER_NEAREST, SAMPLER_MIP, SAMPLER_COUNT };
enum { PIPE_TEXTURE, PIPE_COLOR, PIPE_ACRYLIC, PIPE_SHADOW, PIPE_OUTLINE, PIPE_DOWN, PIPE_UP, PIPE_COUNT };

/* how a monitor gets its frames, from the best way down */
enum scanout
{
    SCANOUT_BEST,     /* images laid out as the driver likes, flipped to the monitor */
    SCANOUT_LINEAR,   /* ... laid out row after row, which every card shows */
    SCANOUT_COPY,     /* drawn into an image of ours and copied into a dumb buffer */
};

/* shaders/common.glsl */
struct push
{
    float proj[4], rect[4], uv[4], clip[4], color[4];
    float radius, opacity, mode, pad;
    float half_px[2], pad2[2];
};

struct texture
{
    struct texture  *next, **link;        /* every image of the device */
    VkImage          image;
    VkDeviceMemory   memory;
    VkImageView      view;                /* all its levels, to sample */
    VkImageView      level0;              /* the first one, to draw into: view itself when it is the only one */
    VkFramebuffer    framebuffer;
    VkDescriptorSet  sets[SAMPLER_COUNT];
    VkDescriptorPool set_pools[SAMPLER_COUNT];
    VkImageLayout    layout;
    int              width, height;
    uint32_t         levels;
    bool             mips_valid;          /* the smaller levels show what the first one does */
    bool             foreign;             /* a client's buffer */
    bool             acquired;            /* ... taken from the client for this frame */
    bool             transitioned;        /* ... and not for the first time */
    int              fd;                  /* ... the client's, not ours to close */
};

struct staging
{
    struct staging *next;
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    uint8_t        *map;
    VkDeviceSize    size, used;
};

struct set_pool
{
    struct set_pool *next;
    VkDescriptorPool pool;
};

static VkInstance       instance;
static VkPhysicalDevice physical;
static VkDevice         device;
static VkQueue          queue;
static uint32_t         queue_family;
static VkCommandPool    command_pool;
static VkCommandBuffer  setup_cmd, draw_cmd;  /* uploads of a frame, then its drawing */
static VkFence          fence;
static VkRenderPass     render_pass;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipeline_layout;
static VkPipeline       pipelines[PIPE_COUNT];
static VkSampler        samplers[SAMPLER_COUNT];
static VkPhysicalDeviceMemoryProperties memory_props;
static struct texture  *textures;
static struct staging  *stagings;
static struct set_pool *set_pools;
static struct texture **garbage;          /* freed while a frame that may use them is recorded */
static uint32_t         garbage_count, garbage_size;

static uint32_t   card_generation;        /* of the card the device is for */
static uint32_t   generation;             /* moves with every device: older images are gone */
static bool       failed;                 /* no device for that card */
static bool       compositing;            /* frames are composed here */
static bool       hardware;               /* the device is the card dwm drives */
static bool       has_dmabuf;             /* buffers of other processes can be taken */
static VkImageUsageFlags import_usage;
static enum scanout scanout;
static int        max_size;
static char       device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

static VkResult (*get_memory_fd)( VkDevice, const VkMemoryGetFdInfoKHR *, int * );
static VkResult (*get_memory_fd_properties)( VkDevice, VkExternalMemoryHandleTypeFlagBits, int,
                                             VkMemoryFdPropertiesKHR * );
static VkResult (*get_image_modifier)( VkDevice, VkImage, VkImageDrmFormatModifierPropertiesEXT * );

/* the frame being recorded */
static bool            in_frame;
static bool            pass_open;
static int             bound_pipeline;
static struct texture *acquired[256];
static uint32_t        acquired_count;

/* where the frame is drawn: a monitor's image, or a window's */
static struct
{
    struct texture *target;
    int             width, height;
    float           proj[4];              /* screen to clip space: xy * proj.xy + proj.zw */
} pass;

/* the frames of one monitor */
struct gpu_frame
{
    struct gpu_output *out;
    struct texture    *texture;
    uint32_t           fb;
    uint32_t           fb_generation;     /* of the card the framebuffer is on */
    enum scanout       mode;              /* how it was laid out */
    bool               held;              /* with KMS: on screen or about to be */
    bool               dead;              /* its device is gone: only KMS still names it */
};

struct gpu_output
{
    struct output     *owner;             /* NULL once its monitor went */
    int                width, height;
    struct gpu_frame  *frames[OUTPUT_FRAMES];
    struct texture    *copy;              /* SCANOUT_COPY: what is drawn, then copied */
    int                locked;            /* frames KMS still holds */
    bool               tested;            /* the monitor takes frames laid out this way */
    bool               dead;
    struct gpu_output *next;
};

static struct gpu_output *gpu_outputs;
static struct gpu_frame  *held_frames[MAX_FRAMES];

/* the levels of a backdrop's blur, kept for the sizes that come again */
static struct blur_chain
{
    int             width, height;
    struct texture *level[BLUR_LEVELS + 1];  /* from 1: the first halving */
    uint32_t        used;
} blur_chains[BLUR_CHAINS];
static uint32_t blur_clock;

static struct texture *cursor_texture, *capture_target;
static unsigned        cursor_texture_serial;

/**********************************************************************
 *          The first start of a build
 *
 * A driver that hangs or takes dwm.exe down with it leaves a desktop nobody
 * can use, at every start. So a start is marked on the disk until frames
 * have been coming for a while; a start that finds the mark of its own
 * build composes on the CPU, and takes the mark away for the next one.
 */

static char     trial_path[512];
static uint64_t trial_since;

static bool trial_begin(void)
{
    static const char build[] = __DATE__ " " __TIME__;
    static bool done, allowed = true;
    const char *prefix = getenv( "WINEPREFIX" );
    char text[64] = "";
    FILE *f;

    if (done) return allowed;
    done = true;
    if (!prefix) return true;
    snprintf( trial_path, sizeof(trial_path), "%s/dosdevices/c:/ProgramData/Arctic/dwm-gpu-trial", prefix );
    if ((f = fopen( trial_path, "r" )))
    {
        if (!fgets( text, sizeof(text), f )) text[0] = 0;
        fclose( f );
        unlink( trial_path );
        if (!strcmp( text, build ))
        {
            MESSAGE( "dwm: composing on the GPU did not live through the last start: not this time\n" );
            trial_path[0] = 0;
            allowed = false;
            return false;
        }
    }
    if ((f = fopen( trial_path, "w" )))
    {
        fputs( build, f );
        fclose( f );
        trial_since = kms_now();
    }
    else trial_path[0] = 0;
    return true;
}

/* frames are shown, and have been for a while */
static void trial_passed(void)
{
    if (!trial_path[0] || kms_now() - trial_since < 20000000) return;
    unlink( trial_path );
    trial_path[0] = 0;
}

/**********************************************************************
 *          Memory and images
 */

static uint32_t memory_type( uint32_t bits, VkMemoryPropertyFlags flags )
{
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

static void texture_link( struct texture *t )
{
    t->next = textures;
    t->link = &textures;
    if (textures) textures->link = &t->next;
    textures = t;
}

static void texture_destroy( struct texture *t )
{
    for (int i = 0; i < SAMPLER_COUNT; i++)
        if (t->sets[i]) vkFreeDescriptorSets( device, t->set_pools[i], 1, &t->sets[i] );
    if (t->framebuffer) vkDestroyFramebuffer( device, t->framebuffer, NULL );
    if (t->level0 && t->level0 != t->view) vkDestroyImageView( device, t->level0, NULL );
    if (t->view) vkDestroyImageView( device, t->view, NULL );
    if (t->image) vkDestroyImage( device, t->image, NULL );
    if (t->memory) vkFreeMemory( device, t->memory, NULL );
    if (t->link)
    {
        *t->link = t->next;
        if (t->next) t->next->link = t->link;
    }
    free( t );
}

/* gone now, or once the frame that may draw with it is with the GPU */
static void texture_free( struct texture *t )
{
    if (!t) return;
    if (!in_frame)
    {
        texture_destroy( t );
        return;
    }
    if (garbage_count == garbage_size)
    {
        uint32_t size = garbage_size ? garbage_size * 2 : 64;
        struct texture **list = realloc( garbage, size * sizeof(*list) );

        if (!list) return;  /* it stays until the device goes */
        garbage = list;
        garbage_size = size;
    }
    garbage[garbage_count++] = t;
}

static VkImageView image_view( VkImage image, uint32_t levels )
{
    VkImageViewCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = FORMAT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
    };
    VkImageView view = VK_NULL_HANDLE;

    if (vkCreateImageView( device, &info, NULL, &view ) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

/* the image has its memory: what is left to sample it and to draw into it */
static bool texture_views( struct texture *t )
{
    if (!(t->view = image_view( t->image, t->levels ))) return false;
    t->level0 = t->levels > 1 ? image_view( t->image, 1 ) : t->view;
    return t->level0 != VK_NULL_HANDLE;
}

/* an image of ours: to upload to, to draw into, to sample and to read back */
static struct texture *texture_create( int width, int height, bool mips )
{
    VkImageCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements reqs;
    struct texture *t;

    if (width <= 0 || height <= 0 || width > max_size || height > max_size) return NULL;
    if (!(t = calloc( 1, sizeof(*t) ))) return NULL;
    if (mips) for (int m = max( width, height ); m > 1; m >>= 1) info.mipLevels++;
    t->width = width;
    t->height = height;
    t->levels = info.mipLevels;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->fd = -1;
    if (vkCreateImage( device, &info, NULL, &t->image ) != VK_SUCCESS) goto failed;
    vkGetImageMemoryRequirements( device, t->image, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, 0 );
    if (alloc.memoryTypeIndex == UINT32_MAX) goto failed;
    if (vkAllocateMemory( device, &alloc, NULL, &t->memory ) != VK_SUCCESS) goto failed;
    if (vkBindImageMemory( device, t->image, t->memory, 0 ) != VK_SUCCESS) goto failed;
    if (!texture_views( t )) goto failed;
    texture_link( t );
    return t;

failed:
    {
        static unsigned int logged;
        if (logged++ < 8) ERR( "no %dx%d image\n", width, height );
    }
    texture_destroy( t );
    return NULL;
}

/* the image with one of the samplers, as a shader is given it */
static VkDescriptorSet texture_set( struct texture *t, int sampler )
{
    VkDescriptorSetAllocateInfo alloc =
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkDescriptorImageInfo image =
    {
        .sampler = samplers[sampler],
        .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write =
    {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image,
    };
    struct set_pool *p;

    if (t->sets[sampler]) return t->sets[sampler];
    for (p = set_pools; p; p = p->next)
    {
        alloc.descriptorPool = p->pool;
        if (vkAllocateDescriptorSets( device, &alloc, &t->sets[sampler] ) == VK_SUCCESS) break;
        t->sets[sampler] = VK_NULL_HANDLE;
    }
    if (!p)
    {
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 };
        VkDescriptorPoolCreateInfo info =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = 256,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };

        if (!(p = calloc( 1, sizeof(*p) ))) return VK_NULL_HANDLE;
        if (vkCreateDescriptorPool( device, &info, NULL, &p->pool ) != VK_SUCCESS)
        {
            free( p );
            return VK_NULL_HANDLE;
        }
        p->next = set_pools;
        set_pools = p;
        alloc.descriptorPool = p->pool;
        if (vkAllocateDescriptorSets( device, &alloc, &t->sets[sampler] ) != VK_SUCCESS)
        {
            t->sets[sampler] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    t->set_pools[sampler] = p->pool;
    write.dstSet = t->sets[sampler];
    vkUpdateDescriptorSets( device, 1, &write, 0, NULL );
    return t->sets[sampler];
}

static VkAccessFlags layout_access( VkImageLayout layout )
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:     return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:     return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    default: return 0;
    }
}

static void barrier( VkCommandBuffer cmd, struct texture *t, uint32_t level, uint32_t levels, VkImageLayout from,
                     VkImageLayout to, uint32_t from_family, uint32_t to_family )
{
    VkImageMemoryBarrier b =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = layout_access( from ),
        .dstAccessMask = layout_access( to ),
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = from_family,
        .dstQueueFamilyIndex = to_family,
        .image = t->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1 },
    };

    vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, NULL, 0, NULL, 1, &b );
}

/* the whole image, for what comes next in cmd */
static void set_image_layout( VkCommandBuffer cmd, struct texture *t, VkImageLayout layout )
{
    if (t->layout == layout) return;
    barrier( cmd, t, 0, t->levels, t->layout, layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
    t->layout = layout;
}

/**********************************************************************
 *          Memory the CPU and the GPU both see: uploads and what is read back
 */

static void staging_destroy( struct staging *s )
{
    if (s->map) vkUnmapMemory( device, s->memory );
    if (s->buffer) vkDestroyBuffer( device, s->buffer, NULL );
    if (s->memory) vkFreeMemory( device, s->memory, NULL );
    free( s );
}

/* size bytes of a buffer, until the next frame begins */
static struct staging *staging_alloc( VkDeviceSize size, VkDeviceSize *offset )
{
    VkBufferCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements reqs;
    struct staging *s;
    void *map;

    size = (size + 15) & ~(VkDeviceSize)15;
    for (s = stagings; s; s = s->next)
    {
        if (s->size - s->used < size) continue;
        *offset = s->used;
        s->used += size;
        return s;
    }
    if (!(s = calloc( 1, sizeof(*s) ))) return NULL;
    info.size = s->size = max( size, (VkDeviceSize)16 << 20 );
    if (vkCreateBuffer( device, &info, NULL, &s->buffer ) != VK_SUCCESS) goto failed;
    vkGetBufferMemoryRequirements( device, s->buffer, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
    if (alloc.memoryTypeIndex == UINT32_MAX)
        alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    if (alloc.memoryTypeIndex == UINT32_MAX) goto failed;
    if (vkAllocateMemory( device, &alloc, NULL, &s->memory ) != VK_SUCCESS) goto failed;
    if (vkBindBufferMemory( device, s->buffer, s->memory, 0 ) != VK_SUCCESS) goto failed;
    if (vkMapMemory( device, s->memory, 0, VK_WHOLE_SIZE, 0, &map ) != VK_SUCCESS) goto failed;
    s->map = map;
    s->next = stagings;
    stagings = s;
    *offset = 0;
    s->used = size;
    return s;

failed:
    ERR( "no %u MB of memory shared with the GPU\n", (unsigned int)(s->size >> 20) );
    staging_destroy( s );
    return NULL;
}

/* a new frame: all of it is free again, and of several buffers the largest stays */
static void staging_reset(void)
{
    struct staging *keep = stagings, *s, *next;

    for (s = stagings; s; s = s->next) if (s->size > keep->size) keep = s;
    for (s = stagings; s; s = next)
    {
        next = s->next;
        if (s != keep) staging_destroy( s );
    }
    if ((stagings = keep))
    {
        keep->next = NULL;
        keep->used = 0;
    }
}

/**********************************************************************
 *          Frames: what is recorded, given to the GPU and waited for
 */

/* the card cannot show what we draw, or the device is lost: the CPU composes from now on */
static void give_up( const char *why )
{
    MESSAGE( "dwm: %s: composing on the CPU\n", why );
    compositing = false;
}

static void end_pass(void)
{
    if (pass_open) vkCmdEndRenderPass( draw_cmd );
    pass_open = false;
}

static bool begin_frame(void)
{
    VkCommandBufferBeginInfo begin =
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    if (in_frame)
    {
        /* a fault took dwm out of the last one (see guarded() in compositor.c) */
        ERR( "the last frame was never finished: dropped\n" );
        for (uint32_t i = 0; i < acquired_count; i++) acquired[i]->acquired = false;
        vkResetCommandBuffer( setup_cmd, 0 );
        vkResetCommandBuffer( draw_cmd, 0 );
        for (uint32_t i = 0; i < garbage_count; i++) texture_destroy( garbage[i] );
        garbage_count = 0;
        in_frame = false;
    }
    staging_reset();
    if (vkBeginCommandBuffer( setup_cmd, &begin ) != VK_SUCCESS) return false;
    if (vkBeginCommandBuffer( draw_cmd, &begin ) != VK_SUCCESS)
    {
        vkEndCommandBuffer( setup_cmd );
        return false;
    }
    in_frame = true;
    pass_open = false;
    bound_pipeline = -1;
    acquired_count = 0;
    memset( &pass, 0, sizeof(pass) );
    return true;
}

/* the frame goes to the GPU; true once all of it is drawn */
static bool end_frame(void)
{
    VkCommandBuffer cmds[2] = { setup_cmd, draw_cmd };
    VkSubmitInfo submit =
    {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 2,
        .pCommandBuffers = cmds,
    };
    VkResult res;

    end_pass();
    /* the clients get their buffers back */
    for (uint32_t i = 0; i < acquired_count; i++)
    {
        struct texture *t = acquired[i];

        barrier( draw_cmd, t, 0, 1, t->layout, VK_IMAGE_LAYOUT_GENERAL, queue_family, VK_QUEUE_FAMILY_FOREIGN_EXT );
        t->layout = VK_IMAGE_LAYOUT_GENERAL;
        t->acquired = false;
    }
    acquired_count = 0;
    vkEndCommandBuffer( setup_cmd );
    vkEndCommandBuffer( draw_cmd );
    in_frame = false;

    if ((res = vkQueueSubmit( queue, 1, &submit, fence )) == VK_SUCCESS)
        res = vkWaitForFences( device, 1, &fence, VK_TRUE, WAIT_NS );
    if (res == VK_SUCCESS) vkResetFences( device, 1, &fence );
    else
    {
        ERR( "the GPU did not draw a frame: %d\n", res );
        vkDeviceWaitIdle( device );
        vkResetFences( device, 1, &fence );
        if (compositing) give_up( "the GPU does not draw" );
    }
    vkResetCommandBuffer( setup_cmd, 0 );
    vkResetCommandBuffer( draw_cmd, 0 );
    for (uint32_t i = 0; i < garbage_count; i++) texture_destroy( garbage[i] );
    garbage_count = 0;
    return res == VK_SUCCESS;
}

/* drawing begins, or goes on, in the image the frame is drawn into now */
static bool open_pass(void)
{
    struct texture *t = pass.target;
    VkViewport viewport = { 0, 0, pass.width, pass.height, 0, 1 };
    VkRect2D area = { { 0, 0 }, { pass.width, pass.height } };
    VkRenderPassBeginInfo begin =
    {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .renderArea = area,
    };

    if (pass_open) return true;
    if (!t) return false;
    if (!t->framebuffer)
    {
        VkFramebufferCreateInfo info =
        {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = &t->level0,
            .width = t->width,
            .height = t->height,
            .layers = 1,
        };

        if (vkCreateFramebuffer( device, &info, NULL, &t->framebuffer ) != VK_SUCCESS)
        {
            t->framebuffer = VK_NULL_HANDLE;
            return false;
        }
    }
    set_image_layout( draw_cmd, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
    begin.framebuffer = t->framebuffer;
    vkCmdBeginRenderPass( draw_cmd, &begin, VK_SUBPASS_CONTENTS_INLINE );
    vkCmdSetViewport( draw_cmd, 0, 1, &viewport );
    vkCmdSetScissor( draw_cmd, 0, 1, &area );
    pass_open = true;
    bound_pipeline = -1;
    return true;
}

/* screen coordinates from left,top on to clip space; the top row comes first everywhere */
static void set_target( struct texture *t, float left, float top, float width, float height )
{
    end_pass();
    pass.target = t;
    pass.width = t->width;
    pass.height = t->height;
    pass.proj[0] = 2 / width;
    pass.proj[1] = 2 / height;
    pass.proj[2] = -1 - 2 * left / width;
    pass.proj[3] = -1 - 2 * top / height;
}

static void clear( float r, float g, float b, float a )
{
    VkClearAttachment attachment =
    {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .clearValue = { .color = { .float32 = { r, g, b, a } } },
    };
    VkClearRect rect = { { { 0, 0 }, { pass.width, pass.height } }, 0, 1 };

    if (open_pass()) vkCmdClearAttachments( draw_cmd, 1, &attachment, 1, &rect );
}

static void push_init( struct push *pc, const struct clip *clip, float opacity )
{
    memset( pc, 0, sizeof(*pc) );
    memcpy( pc->proj, pass.proj, sizeof(pc->proj) );
    pc->uv[2] = pc->uv[3] = 1;
    pc->opacity = opacity;
    pc->radius = -1;
    if (!clip) return;
    pc->clip[0] = clip->rect.left;
    pc->clip[1] = clip->rect.top;
    pc->clip[2] = clip->rect.right;
    pc->clip[3] = clip->rect.bottom;
    pc->radius = clip->radius;
}

static void push_rect( struct push *pc, float x0, float y0, float x1, float y1 )
{
    pc->rect[0] = x0;
    pc->rect[1] = y0;
    pc->rect[2] = x1;
    pc->rect[3] = y1;
}

static void push_uv( struct push *pc, float u0, float v0, float u1, float v1 )
{
    pc->uv[0] = u0;
    pc->uv[1] = v0;
    pc->uv[2] = u1;
    pc->uv[3] = v1;
}

static void push_color( struct push *pc, float r, float g, float b, float a )
{
    pc->color[0] = r;
    pc->color[1] = g;
    pc->color[2] = b;
    pc->color[3] = a;
}

/* one rectangle; t is what its shader samples, NULL if nothing */
static void draw( int pipeline, struct texture *t, int sampler, const struct push *pc )
{
    VkDescriptorSet set = VK_NULL_HANDLE;

    if (t && !(set = texture_set( t, sampler ))) return;
    if (!open_pass()) return;
    if (bound_pipeline != pipeline)
    {
        vkCmdBindPipeline( draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[pipeline] );
        bound_pipeline = pipeline;
    }
    if (set) vkCmdBindDescriptorSets( draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, NULL );
    vkCmdPushConstants( draw_cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(*pc), pc );
    vkCmdDraw( draw_cmd, 4, 1, 0, 0 );
}

/* the image as the CPU reads it, top row first, once the frame has ended and until the next begins */
static const uint8_t *read_texture( struct texture *t )
{
    VkBufferImageCopy region =
    {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { t->width, t->height, 1 },
    };
    struct staging *s;

    if (!(s = staging_alloc( (VkDeviceSize)t->width * t->height * 4, &region.bufferOffset ))) return NULL;
    end_pass();
    set_image_layout( draw_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
    vkCmdCopyImageToBuffer( draw_cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->buffer, 1, &region );
    return s->map + region.bufferOffset;
}

/* width x height pixels, packed, into the image before anything of the frame is drawn */
static bool upload( struct texture *t, const uint32_t *pixels )
{
    VkBufferImageCopy region =
    {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { t->width, t->height, 1 },
    };
    size_t size = (size_t)t->width * t->height * 4;
    struct staging *s;

    if (!(s = staging_alloc( size, &region.bufferOffset ))) return false;
    memcpy( s->map + region.bufferOffset, pixels, size );
    set_image_layout( setup_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
    vkCmdCopyBufferToImage( setup_cmd, s->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
    set_image_layout( setup_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
    return true;
}

/* images are shrunk into thumbnails: smoothly, from levels each half the one before */
static void generate_mips( struct texture *t )
{
    int width = t->width, height = t->height;

    if (t->levels < 2) return;
    end_pass();
    set_image_layout( draw_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
    for (uint32_t i = 1; i < t->levels; i++)
    {
        VkImageBlit blit =
        {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { width, height, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
        };

        width = max( 1, width / 2 );
        height = max( 1, height / 2 );
        blit.dstOffsets[1].x = width;
        blit.dstOffsets[1].y = height;
        blit.dstOffsets[1].z = 1;
        barrier( draw_cmd, t, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
        vkCmdBlitImage( draw_cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR );
    }
    barrier( draw_cmd, t, t->levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED );
    t->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t->mips_valid = true;
}

/**********************************************************************
 *          The device
 */

static void free_output( struct gpu_output *out );

static void shutdown_vk(void)
{
    struct gpu_output *out, *next_out;
    struct staging *s, *next_s;
    struct set_pool *p, *next_p;

    /* frames KMS still holds are named when it lets them go: they stay, empty */
    for (out = gpu_outputs; out; out = next_out)
    {
        next_out = out->next;
        if (out->owner) out->owner->gpu = NULL;
        out->owner = NULL;
        out->copy = NULL;
        for (int i = 0; i < OUTPUT_FRAMES; i++)
        {
            struct gpu_frame *frame = out->frames[i];

            out->frames[i] = NULL;
            if (!frame) continue;
            frame->texture = NULL;
            frame->fb = 0;
            if (frame->held) frame->dead = true;
            else free( frame );
        }
        if (out->locked) out->dead = true;
        else free( out );
    }
    gpu_outputs = NULL;
    memset( blur_chains, 0, sizeof(blur_chains) );
    cursor_texture = capture_target = NULL;
    in_frame = pass_open = false;
    acquired_count = 0;

    if (device)
    {
        vkDeviceWaitIdle( device );
        garbage_count = 0;  /* they are on the list of all */
        while (textures) texture_destroy( textures );
        for (s = stagings; s; s = next_s)
        {
            next_s = s->next;
            staging_destroy( s );
        }
        for (p = set_pools; p; p = next_p)
        {
            next_p = p->next;
            vkDestroyDescriptorPool( device, p->pool, NULL );
            free( p );
        }
        for (int i = 0; i < PIPE_COUNT; i++) if (pipelines[i]) vkDestroyPipeline( device, pipelines[i], NULL );
        for (int i = 0; i < SAMPLER_COUNT; i++) if (samplers[i]) vkDestroySampler( device, samplers[i], NULL );
        if (pipeline_layout) vkDestroyPipelineLayout( device, pipeline_layout, NULL );
        if (set_layout) vkDestroyDescriptorSetLayout( device, set_layout, NULL );
        if (render_pass) vkDestroyRenderPass( device, render_pass, NULL );
        if (fence) vkDestroyFence( device, fence, NULL );
        if (command_pool) vkDestroyCommandPool( device, command_pool, NULL );
        vkDestroyDevice( device, NULL );
    }
    if (instance) vkDestroyInstance( instance, NULL );
    stagings = NULL;
    set_pools = NULL;
    memset( pipelines, 0, sizeof(pipelines) );
    memset( samplers, 0, sizeof(samplers) );
    pipeline_layout = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
    command_pool = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    compositing = false;
    has_dmabuf = false;
    generation++;
}

static bool has_extension( const VkExtensionProperties *list, uint32_t count, const char *name )
{
    for (uint32_t i = 0; i < count; i++) if (!strcmp( list[i].extensionName, name )) return true;
    return false;
}

/* The device that draws the frames: the card dwm drives, so that its frames
 * are shown without a copy. Without a Vulkan driver for that card the
 * software rasterizer draws them, not another card: one that only games use
 * is not kept awake for the desktop. */
static bool choose_device( VkExtensionProperties **extensions, uint32_t *extension_count )
{
    VkPhysicalDevice devices[16];
    uint32_t count = ARRAY_SIZE(devices);
    int best = 0;

    *extensions = NULL;
    *extension_count = 0;
    if (vkEnumeratePhysicalDevices( instance, &count, devices ) < 0 || !count) return false;
    for (uint32_t i = 0; i < count; i++)
    {
        VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        VkExtensionProperties *list;
        uint32_t list_count = 0;
        int score = 1;

        vkGetPhysicalDeviceProperties( devices[i], &props.properties );
        if (props.properties.apiVersion < VK_API_VERSION_1_1) continue;
        if (vkEnumerateDeviceExtensionProperties( devices[i], NULL, &list_count, NULL ) < 0) continue;
        if (!(list = calloc( list_count + 1, sizeof(*list) ))) continue;
        if (vkEnumerateDeviceExtensionProperties( devices[i], NULL, &list_count, list ) < 0)
        {
            free( list );
            continue;
        }
        if (has_extension( list, list_count, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME ))
        {
            props.pNext = &drm;
            vkGetPhysicalDeviceProperties2( devices[i], &props );
            if (drm.hasPrimary && kms.devnum == makedev( drm.primaryMajor, drm.primaryMinor )) score = 3;
        }
        if (score < 3 && props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score = 2;
        TRACE( "%s: %d\n", props.properties.deviceName, score );
        if (score <= best)
        {
            free( list );
            continue;
        }
        best = score;
        physical = devices[i];
        free( *extensions );
        *extensions = list;
        *extension_count = list_count;
        hardware = score == 3;
        max_size = props.properties.limits.maxImageDimension2D;
        strcpy( device_name, props.properties.deviceName );
    }
    return best > 0;
}

/* an image with that layout can be made of a buffer of another process, or given to one */
static bool modifier_usable( uint64_t modifier, VkImageUsageFlags usage, bool export, int width, int height )
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo external =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &mod,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external,
        .format = FORMAT,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props };
    VkExternalMemoryFeatureFlags want = export ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT
                                               : VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;

    if (vkGetPhysicalDeviceImageFormatProperties2( physical, &info, &props ) != VK_SUCCESS) return false;
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures & want)) return false;
    return props.imageFormatProperties.maxExtent.width >= width && props.imageFormatProperties.maxExtent.height >= height;
}

static VkPipeline create_pipeline( VkShaderModule vertex, const uint32_t *code, size_t size, bool blend )
{
    VkShaderModuleCreateInfo module_info =
    {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };
    VkPipelineShaderStageCreateInfo stages[2] =
    {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vertex, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo viewport =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1,
    };
    VkPipelineMultisampleStateCreateInfo multisample =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* everything is premultiplied */
    VkPipelineColorBlendAttachmentState attachment =
    {
        .blendEnable = blend,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blending =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_SIZE(dynamic_states),
        .pDynamicStates = dynamic_states,
    };
    VkGraphicsPipelineCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blending,
        .pDynamicState = &dynamic,
        .layout = pipeline_layout,
        .renderPass = render_pass,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule fragment;

    if (vkCreateShaderModule( device, &module_info, NULL, &fragment ) != VK_SUCCESS) return VK_NULL_HANDLE;
    stages[1].module = fragment;
    if (vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline ) != VK_SUCCESS)
        pipeline = VK_NULL_HANDLE;
    vkDestroyShaderModule( device, fragment, NULL );
    return pipeline;
}

/* what every frame is drawn with */
static bool init_drawing(void)
{
    static const struct { const uint32_t *code; size_t size; bool blend; } shaders[PIPE_COUNT] =
    {
        [PIPE_TEXTURE] = { shader_texture_frag, sizeof(shader_texture_frag), true },
        [PIPE_COLOR]   = { shader_color_frag, sizeof(shader_color_frag), true },
        [PIPE_ACRYLIC] = { shader_acrylic_frag, sizeof(shader_acrylic_frag), true },
        [PIPE_SHADOW]  = { shader_shadow_frag, sizeof(shader_shadow_frag), true },
        [PIPE_OUTLINE] = { shader_outline_frag, sizeof(shader_outline_frag), true },
        [PIPE_DOWN]    = { shader_down_frag, sizeof(shader_down_frag), false },
        [PIPE_UP]      = { shader_up_frag, sizeof(shader_up_frag), false },
    };
    /* what is in the image stays: a frame is drawn in several goes, around its backdrops */
    VkAttachmentDescription attachment =
    {
        .format = FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass =
    {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &reference,
    };
    VkRenderPassCreateInfo pass_info =
    {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };
    VkDescriptorSetLayoutBinding binding =
    {
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo set_info =
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct push) };
    VkPipelineLayoutCreateInfo layout_info =
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    VkSamplerCreateInfo sampler_info =
    {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkShaderModuleCreateInfo vertex_info =
    {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(shader_quad_vert),
        .pCode = shader_quad_vert,
    };
    VkShaderModule vertex;
    bool ok = true;

    if (vkCreateRenderPass( device, &pass_info, NULL, &render_pass ) != VK_SUCCESS) return false;
    if (vkCreateDescriptorSetLayout( device, &set_info, NULL, &set_layout ) != VK_SUCCESS) return false;
    if (vkCreatePipelineLayout( device, &layout_info, NULL, &pipeline_layout ) != VK_SUCCESS) return false;

    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler( device, &sampler_info, NULL, &samplers[SAMPLER_LINEAR] ) != VK_SUCCESS) return false;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler( device, &sampler_info, NULL, &samplers[SAMPLER_MIP] ) != VK_SUCCESS) return false;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.maxLod = 0;
    if (vkCreateSampler( device, &sampler_info, NULL, &samplers[SAMPLER_NEAREST] ) != VK_SUCCESS) return false;

    if (vkCreateShaderModule( device, &vertex_info, NULL, &vertex ) != VK_SUCCESS) return false;
    for (int i = 0; i < PIPE_COUNT && ok; i++)
        ok = (pipelines[i] = create_pipeline( vertex, shaders[i].code, shaders[i].size, shaders[i].blend )) != VK_NULL_HANDLE;
    vkDestroyShaderModule( device, vertex, NULL );
    return ok;
}

/* The Vulkan drivers of the card dwm drives, as the loader names their
 * manifests. Only they are loaded: every driver the loader opens wakes its
 * card and holds it while dwm runs, and the fast card of a laptop must sleep
 * while no game uses it, as Windows lets it. */
static const char *card_icd(void)
{
    static const struct { const char *driver, *icd; } icds[] =
    {
        { "i915", "*intel*" }, { "xe", "*intel*" }, { "amdgpu", "*radeon*" },
        { "nvidia-drm", "*nvidia*" }, { "nouveau", "*nouveau*" },
    };

    for (int i = 0; i < ARRAY_SIZE(icds); i++) if (!strcmp( kms.driver, icds[i].driver )) return icds[i].icd;
    return "*lvp*";  /* a card without Vulkan (a virtual machine): the software rasterizer */
}

static bool create_instance( const VkInstanceCreateInfo *info, const char *icd )
{
    const char *old = getenv( "VK_LOADER_DRIVERS_SELECT" );
    char *saved = old ? strdup( old ) : NULL;
    VkResult res;

    setenv( "VK_LOADER_DRIVERS_SELECT", icd, 1 );
    res = vkCreateInstance( info, NULL, &instance );
    if (saved) setenv( "VK_LOADER_DRIVERS_SELECT", saved, 1 );
    else unsetenv( "VK_LOADER_DRIVERS_SELECT" );
    free( saved );
    if (res == VK_SUCCESS) return true;
    WARN( "no Vulkan with %s: %d\n", icd, res );
    instance = VK_NULL_HANDLE;
    return false;
}

/* a device for the card dwm drives, made again when the card changes */
static bool init_device(void)
{
    static const char *const dmabuf_extensions[] =
    {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    VkApplicationInfo app =
    {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "dwm",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkQueueFamilyProperties families[16];
    uint32_t family_count = ARRAY_SIZE(families), extension_count, enabled_count = 0;
    VkExtensionProperties *extensions;
    const char *enabled[8], *copy = getenv( "ARCTIC_DWM_COPY" );
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info =
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info =
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .ppEnabledExtensionNames = enabled,
    };
    VkCommandPoolCreateInfo pool_info =
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandBufferAllocateInfo buffers_info =
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkCommandBuffer buffers[2];
    VkResult res;

    if (card_generation == kms.generation)
    {
        if (failed) return false;
        if (device) return true;
    }
    shutdown_vk();
    card_generation = kms.generation;
    failed = true;

    /* the driver of the card the monitors are on, then the software one */
    if (!create_instance( &instance_info, card_icd() ) || !choose_device( &extensions, &extension_count ))
    {
        if (instance) vkDestroyInstance( instance, NULL );
        instance = VK_NULL_HANDLE;
        if (!create_instance( &instance_info, "*lvp*" )) return false;
        if (!choose_device( &extensions, &extension_count ))
        {
            ERR( "no Vulkan device\n" );
            return false;
        }
    }
    vkGetPhysicalDeviceMemoryProperties( physical, &memory_props );
    vkGetPhysicalDeviceQueueFamilyProperties( physical, &family_count, families );
    for (queue_family = 0; queue_family < family_count; queue_family++)
        if (families[queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (queue_family == family_count)
    {
        ERR( "%s draws nothing\n", device_name );
        free( extensions );
        return false;
    }

    has_dmabuf = true;
    for (int i = 0; i < ARRAY_SIZE(dmabuf_extensions); i++)
        has_dmabuf &= has_extension( extensions, extension_count, dmabuf_extensions[i] );
    if (has_dmabuf)
    {
        for (int i = 0; i < ARRAY_SIZE(dmabuf_extensions); i++) enabled[enabled_count++] = dmabuf_extensions[i];
        /* part of Vulkan 1.2, asked for by name before it */
        if (has_extension( extensions, extension_count, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME ))
            enabled[enabled_count++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    }
    free( extensions );
    queue_info.queueFamilyIndex = queue_family;
    device_info.enabledExtensionCount = enabled_count;
    if ((res = vkCreateDevice( physical, &device_info, NULL, &device )) != VK_SUCCESS)
    {
        ERR( "no device on %s: %d\n", device_name, res );
        device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue( device, queue_family, 0, &queue );
    get_memory_fd = (void *)vkGetDeviceProcAddr( device, "vkGetMemoryFdKHR" );
    get_memory_fd_properties = (void *)vkGetDeviceProcAddr( device, "vkGetMemoryFdPropertiesKHR" );
    get_image_modifier = (void *)vkGetDeviceProcAddr( device, "vkGetImageDrmFormatModifierPropertiesEXT" );
    if (!get_memory_fd || !get_memory_fd_properties || !get_image_modifier) has_dmabuf = false;

    pool_info.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool( device, &pool_info, NULL, &command_pool ) != VK_SUCCESS) return false;
    buffers_info.commandPool = command_pool;
    if (vkAllocateCommandBuffers( device, &buffers_info, buffers ) != VK_SUCCESS) return false;
    setup_cmd = buffers[0];
    draw_cmd = buffers[1];
    if (vkCreateFence( device, &fence_info, NULL, &fence ) != VK_SUCCESS) return false;
    if (!init_drawing())
    {
        ERR( "%s does not take the shaders\n", device_name );
        return false;
    }

    /* windows of other programs: their buffers sampled, and read back for the CPU where it can be */
    import_usage = 0;
    if (has_dmabuf)
    {
        if (modifier_usable( DRM_FORMAT_MOD_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, 1, 1 ))
            import_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        else if (modifier_usable( DRM_FORMAT_MOD_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, false, 1, 1 ))
            import_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        else WARN( "%s takes no buffers of other processes\n", device_name );
    }
    scanout = hardware && has_dmabuf && !(copy && *copy == '1') ? SCANOUT_BEST : SCANOUT_COPY;
    failed = false;
    return true;
}

bool gpu_start(void)
{
    const char *cpu = getenv( "ARCTIC_DWM_CPU" );
    VkPhysicalDeviceProperties props;

    compositing = false;
    if (cpu && *cpu == '1') goto cpu;
    if (!trial_begin()) goto cpu;
    if (!init_device()) goto cpu;
    compositing = true;
    vkGetPhysicalDeviceProperties( physical, &props );
    MESSAGE( "dwm: composing on the GPU: %s (Vulkan %u.%u.%u)%s\n", device_name,
             VK_API_VERSION_MAJOR( props.apiVersion ), VK_API_VERSION_MINOR( props.apiVersion ),
             VK_API_VERSION_PATCH( props.apiVersion ),
             scanout == SCANOUT_COPY ? ", frames copied to the card" : "" );
    return true;

cpu:
    MESSAGE( "dwm: composing on the CPU\n" );
    return false;
}

bool gpu_active(void)
{
    /* a card that came after the last start */
    if (compositing && card_generation != kms.generation) gpu_start();
    return compositing;
}

/**********************************************************************
 *          Textures
 */

/* a client's GPU buffer as an image, without a copy */
static struct texture *import_dmabuf( const struct dmabuf *buffer )
{
    VkSubresourceLayout plane = { .offset = buffer->offset, .rowPitch = buffer->stride };
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        /* a client that names no layout was offered the linear one alone */
        .drmFormatModifier = buffer->modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : buffer->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane,
    };
    VkExternalMemoryImageCreateInfo external =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &explicit,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = { buffer->width, buffer->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = import_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    VkImportMemoryFdInfoKHR import =
    {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = -1,
    };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import };
    VkMemoryRequirements reqs;
    struct texture *t;

    if (!has_dmabuf || !import_usage) return NULL;
    if (buffer->format != DRM_FORMAT_XRGB8888 && buffer->format != DRM_FORMAT_ARGB8888) return NULL;
    if (!(t = calloc( 1, sizeof(*t) ))) return NULL;
    t->width = buffer->width;
    t->height = buffer->height;
    t->levels = 1;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->foreign = true;
    t->fd = buffer->fd;
    if (vkCreateImage( device, &info, NULL, &t->image ) != VK_SUCCESS) goto failed;
    if (get_memory_fd_properties( device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, buffer->fd, &fd_props ) != VK_SUCCESS)
        goto failed;
    vkGetImageMemoryRequirements( device, t->image, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits & fd_props.memoryTypeBits, 0 );
    if (alloc.memoryTypeIndex == UINT32_MAX) goto failed;
    dedicated.image = t->image;
    /* the memory keeps the descriptor it is made of */
    if ((import.fd = fcntl( buffer->fd, F_DUPFD_CLOEXEC, 0 )) < 0) goto failed;
    if (vkAllocateMemory( device, &alloc, NULL, &t->memory ) != VK_SUCCESS)
    {
        close( import.fd );
        t->memory = VK_NULL_HANDLE;
        goto failed;
    }
    if (vkBindImageMemory( device, t->image, t->memory, 0 ) != VK_SUCCESS) goto failed;
    if (!texture_views( t )) goto failed;
    texture_link( t );
    return t;

failed:
    {
        static unsigned int logged;
        if (logged++ < 8)
            ERR( "cannot take a %ux%u buffer, layout %#llx\n", buffer->width, buffer->height,
                 (unsigned long long)buffer->modifier );
    }
    texture_destroy( t );
    return NULL;
}

/* The client's buffer is ours for the frame. What the client's GPU still
 * draws into it is waited for, as a scanout would wait; a client that never
 * finishes does not hold the desktop for long. */
static bool acquire( struct texture *t )
{
    struct pollfd pfd = { .fd = t->fd, .events = POLLIN };

    if (t->acquired) return true;
    if (!in_frame || acquired_count == ARRAY_SIZE(acquired)) return false;
    poll( &pfd, 1, 100 );
    barrier( setup_cmd, t, 0, 1, t->transitioned ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, queue_family );
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->acquired = t->transitioned = true;
    acquired[acquired_count++] = t;
    return true;
}

static struct texture *dmabuf_texture( struct dmabuf *buffer )
{
    struct texture *t;

    if (buffer->texture_generation != generation)
    {
        buffer->texture = NULL;
        buffer->texture_failed = false;
        buffer->texture_generation = generation;
    }
    if (buffer->texture_failed) return NULL;
    if (!buffer->texture && !(buffer->texture = import_dmabuf( buffer )))
    {
        buffer->texture_failed = true;
        return NULL;
    }
    t = buffer->texture;
    return acquire( t ) ? t : NULL;
}

void gpu_dmabuf_gone( struct dmabuf *buffer )
{
    if (buffer->texture && buffer->texture_generation == generation && device) texture_free( buffer->texture );
    buffer->texture = NULL;
}

/* the pixels of a surface drawn by GDI, uploaded when they changed */
static struct texture *shm_texture( struct surface *s )
{
    struct texture *t;

    if (!s->pixels) return NULL;
    if (s->texture_generation != generation)
    {
        s->texture = NULL;
        s->texture_generation = generation;
    }
    t = s->texture;
    if (t && (t->width != s->width || t->height != s->height))
    {
        texture_free( t );
        t = s->texture = NULL;
    }
    if (!t)
    {
        if (!(t = s->texture = texture_create( s->width, s->height, false ))) return NULL;
        s->texture_stale = true;
    }
    if (s->texture_stale)
    {
        if (!upload( t, s->pixels )) return t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? t : NULL;
        s->texture_stale = false;
    }
    return t;
}

void gpu_surface_gone( struct surface *s )
{
    if (s->texture && s->texture_generation == generation && device) texture_free( s->texture );
    s->texture = NULL;
}

/* an image the size of a window, to draw it into */
static struct texture *image_alloc( struct window_image *image, int width, int height )
{
    struct texture *t;

    if (image->generation != generation)
    {
        image->texture = NULL;
        image->width = image->height = 0;
        image->generation = generation;
    }
    t = image->texture;
    if (t && (image->width != width || image->height != height))
    {
        texture_free( t );
        t = image->texture = NULL;
        image->width = image->height = 0;
    }
    if (!t && !(t = image->texture = texture_create( width, height, true ))) return NULL;
    image->width = width;
    image->height = height;
    return t;
}

void gpu_image_free( struct window_image *image )
{
    free( image->pixels );
    image->pixels = NULL;
    if (image->texture && image->generation == generation && device) texture_free( image->texture );
    image->texture = NULL;
    image->width = image->height = 0;
}

static struct texture *image_texture( const struct window_image *image )
{
    return image->generation == generation ? image->texture : NULL;
}

/**********************************************************************
 *          Drawing
 */

struct layer_ctx
{
    const struct window_draw *draw;
    const struct xform       *xform;
    const struct clip        *clip;
    float                     opacity;
};

static void draw_layer( struct surface *s, int x, int y, const struct layer_ctx *ctx )
{
    int sx0 = 0, sy0 = 0, sw = s->width, sh = s->height, dw, dh;
    enum layer_mode mode = ctx->draw->mode( s, ctx->draw );
    struct texture *t;
    struct push pc;

    if (s->dmabuf) t = dmabuf_texture( s->dmabuf );
    else t = shm_texture( s );
    if (!t) return;

    if (s->src_width > 0 && s->src_height > 0)
    {
        sx0 = s->src_x;
        sy0 = s->src_y;
        sw = min( s->src_width, s->width - sx0 );
        sh = min( s->src_height, s->height - sy0 );
        if (sw <= 0 || sh <= 0) return;
    }
    dw = s->dst_width ? s->dst_width : sw;
    dh = s->dst_height ? s->dst_height : sh;

    push_init( &pc, ctx->clip, ctx->opacity );
    pc.mode = mode;
    push_rect( &pc, x * ctx->xform->sx + ctx->xform->tx, y * ctx->xform->sy + ctx->xform->ty,
               (x + dw) * ctx->xform->sx + ctx->xform->tx, (y + dh) * ctx->xform->sy + ctx->xform->ty );
    push_uv( &pc, (float)sx0 / s->width, (float)sy0 / s->height,
             (float)(sx0 + sw) / s->width, (float)(sy0 + sh) / s->height );
    draw( PIPE_TEXTURE, t, SAMPLER_LINEAR, &pc );
}

static void draw_tree( struct surface *s, int x, int y, const struct layer_ctx *ctx, int depth )
{
    struct surface *child;

    if (s->pixels || s->dmabuf) draw_layer( s, x, y, ctx );
    if (depth >= 16) return;  /* a client gone wrong must not take dwm.exe down */
    wl_list_for_each( child, &s->children, child_link )
    {
        if (child->hwnd) continue;  /* a window of its own */
        draw_tree( child, x + child->sub_x, y + child->sub_y, ctx, depth + 1 );
    }
}

static void draw_tree_callback( struct surface *s, int x, int y, void *ctx )
{
    draw_tree( s, x, y, ctx, 0 );
}

static void draw_image( const struct window_image *image, const struct frect *dest, const RECT *source,
                        const struct clip *clip, float opacity )
{
    struct texture *t = image_texture( image );
    struct push pc;

    if (!t || t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    push_init( &pc, clip, opacity );
    pc.mode = LAYER_PREMULTIPLIED;
    push_rect( &pc, dest->left, dest->top, dest->right, dest->bottom );
    if (source)
        push_uv( &pc, (float)source->left / image->width, (float)source->top / image->height,
                 (float)source->right / image->width, (float)source->bottom / image->height );
    draw( PIPE_TEXTURE, t, t->mips_valid ? SAMPLER_MIP : SAMPLER_LINEAR, &pc );
}

static void count_layers( struct surface *s, int *count, int depth )
{
    struct surface *child;

    if (s->pixels || s->dmabuf) (*count)++;
    if (depth >= 16) return;
    wl_list_for_each( child, &s->children, child_link )
        if (!child->hwnd) count_layers( child, count, depth + 1 );
}

static void count_layer( struct surface *s, int x, int y, void *ctx )
{
    count_layers( s, ctx, 0 );
}

/* a window of one layer fades and zooms as it is; several layers are
 * drawn into an image first, so that they fade as one */
static bool needs_image( const struct window_draw *draw )
{
    int count = 0;

    scene_surfaces( draw, count_layer, &count );
    return count > 1;
}

/* the window as it is now, into an image of its size */
static bool render_window_image( const struct window_draw *draw, struct window_image *image, bool mipmaps )
{
    const struct dwm_window *w = draw->w;
    struct xform identity = { 1, 1, 0, 0 };
    struct layer_ctx ctx = { draw, &identity, NULL, 1 };
    int width = w->right - w->left, height = w->bottom - w->top;
    struct texture *t;

    end_pass();
    if (!(t = image_alloc( image, width, height ))) return false;
    {
        __typeof__(pass) saved = pass;

        set_target( t, w->left, w->top, width, height );
        clear( 0, 0, 0, 0 );
        scene_surfaces( draw, draw_tree_callback, &ctx );
        end_pass();
        t->mips_valid = false;
        /* it may be shrunk into a thumbnail */
        if (mipmaps) generate_mips( t );
        set_image_layout( draw_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
        pass = saved;
    }
    return true;
}

bool gpu_snapshot( struct window_state *ws, const struct dwm_window *w )
{
    struct window_draw draw;
    bool ok;

    if (!compositing || !init_device() || !begin_frame()) return false;
    scene_static_draw( &draw, w, ws );
    ok = render_window_image( &draw, &ws->snapshot, true );
    if (!end_frame() || !ok) return false;
    ws->snapshot_rect.left = w->left;
    ws->snapshot_rect.top = w->top;
    ws->snapshot_rect.right = w->right;
    ws->snapshot_rect.bottom = w->bottom;
    ws->snapshot_valid = true;
    return true;
}

/* the levels for a backdrop of that size: the ones it had, or those of the size not seen the longest */
static struct blur_chain *blur_chain( int width, int height )
{
    struct blur_chain *chain = &blur_chains[0];

    for (int i = 0; i < BLUR_CHAINS; i++)
    {
        if (blur_chains[i].level[1] && blur_chains[i].width == width && blur_chains[i].height == height)
        {
            blur_chains[i].used = ++blur_clock;
            return &blur_chains[i];
        }
        if (blur_chains[i].used < chain->used) chain = &blur_chains[i];
    }
    for (int level = 1; level <= BLUR_LEVELS; level++)
    {
        texture_free( chain->level[level] );
        chain->level[level] = texture_create( max( 1, width >> level ), max( 1, height >> level ), false );
    }
    for (int level = 1; level <= BLUR_LEVELS; level++)
    {
        if (chain->level[level]) continue;
        for (level = 1; level <= BLUR_LEVELS; level++)
        {
            texture_free( chain->level[level] );
            chain->level[level] = NULL;
        }
        chain->used = 0;
        return NULL;
    }
    chain->width = width;
    chain->height = height;
    chain->used = ++blur_clock;
    return chain;
}

/* one pass of the blur: the part uv of one image, over the whole of another */
static void blur_pass( int pipeline, struct texture *from, const float *uv, struct texture *to, float offset )
{
    struct push pc;

    set_target( to, 0, 0, to->width, to->height );
    push_init( &pc, NULL, 1 );
    push_rect( &pc, 0, 0, to->width, to->height );
    if (uv) push_uv( &pc, uv[0], uv[1], uv[2], uv[3] );
    pc.half_px[0] = 0.5f / from->width;
    pc.half_px[1] = 0.5f / from->height;
    pc.mode = offset;
    draw( pipeline, from, SAMPLER_LINEAR, &pc );
    end_pass();
    set_image_layout( draw_cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
}

/* DWMWA_ARCTIC_BLUR, 1-100, as halvings and how far each one samples: 50
 * is the default, 3 halvings 2 apart; 0 is the default too */
static void blur_strength( UINT32 strength, int *levels, float *offset )
{
    *levels = BLUR_DEFAULT;
    *offset = BLUR_OFFSET;
    if (!strength) return;
    if (strength <= 10) { *levels = 1; *offset = 1.0f + 2.0f * (strength - 1) / 9; }
    else if (strength <= 33) { *levels = 2; *offset = 1.0f + 2.0f * (strength - 11) / 22; }
    else if (strength <= 66) { *levels = 3; *offset = 1.0f + 2.0f * (strength - 34) / 32; }
    else { *levels = 4; *offset = 1.0f + 2.0f * (min( strength, 100 ) - 67) / 33; }
}

/* what lies behind the window on this monitor, blurred and tinted: acrylic */
static void draw_backdrop( const struct window_draw *draw_, int ox, int oy )
{
    const struct frect *r = &draw_->shown;
    float tint[4] =
    {
        ((draw_->tint >> 16) & 0xff) / 255.0f, ((draw_->tint >> 8) & 0xff) / 255.0f, (draw_->tint & 0xff) / 255.0f,
        (draw_->tint >> 24) / 255.0f
    };
    int x0, y0, x1, y1, width, height, levels;
    struct texture *target = pass.target;
    struct blur_chain *chain;
    struct push pc;
    float uv[4], offset;

    blur_strength( draw_->ws && draw_->ws->has_attr ? draw_->ws->attr.blur : 0, &levels, &offset );
    if (!draw_->blur)
    {
        push_init( &pc, &draw_->clip, draw_->opacity );
        push_color( &pc, tint[0] * tint[3], tint[1] * tint[3], tint[2] * tint[3], tint[3] );
        push_rect( &pc, r->left, r->top, r->right, r->bottom );
        draw( PIPE_COLOR, NULL, 0, &pc );
        return;
    }

    /* the part of this monitor's frame behind it, with a margin: output coordinates */
    x0 = max( 0, (int)floorf( r->left ) - ox - BLUR_MARGIN );
    y0 = max( 0, (int)floorf( r->top ) - oy - BLUR_MARGIN );
    x1 = min( pass.width, (int)ceilf( r->right ) - ox + BLUR_MARGIN );
    y1 = min( pass.height, (int)ceilf( r->bottom ) - oy + BLUR_MARGIN );
    if (x1 <= x0 || y1 <= y0 || !target) return;
    width = x1 - x0;
    height = y1 - y0;
    if (!(chain = blur_chain( width, height ))) return;

    {
        __typeof__(pass) saved = pass;

        /* the frame so far is what the first halving reads */
        end_pass();
        set_image_layout( draw_cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
        uv[0] = (float)x0 / target->width;
        uv[1] = (float)y0 / target->height;
        uv[2] = (float)x1 / target->width;
        uv[3] = (float)y1 / target->height;
        blur_pass( PIPE_DOWN, target, uv, chain->level[1], offset );
        for (int level = 2; level <= levels; level++)
            blur_pass( PIPE_DOWN, chain->level[level - 1], NULL, chain->level[level], offset );
        for (int level = levels - 1; level >= 1; level--)
            blur_pass( PIPE_UP, chain->level[level + 1], NULL, chain->level[level], offset );
        pass = saved;
    }

    push_init( &pc, &draw_->clip, draw_->opacity );
    push_color( &pc, tint[0], tint[1], tint[2], tint[3] );
    pc.mode = draw_->ws && draw_->ws->attr.accent == ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND ? 0.02f : 0.0f;
    push_rect( &pc, r->left, r->top, r->right, r->bottom );
    push_uv( &pc, (r->left - ox - x0) / width, (r->top - oy - y0) / height,
             (r->right - ox - x0) / width, (r->bottom - oy - y0) / height );
    draw( PIPE_ACRYLIC, chain->level[1], SAMPLER_LINEAR, &pc );
}

static void draw_thumbnail( const struct thumbnail_draw *t )
{
    struct window_state *ws = t->source_state;
    struct window_draw draw_;
    const struct window_image *image;

    if (!ws) return;
    if (t->from_snapshot) image = &ws->snapshot;
    else
    {
        scene_static_draw( &draw_, t->source, ws );
        if (!render_window_image( &draw_, &ws->scratch, true )) return;
        image = &ws->scratch;
    }
    draw_image( image, &t->dest, &t->source_rect, NULL, t->opacity );
}

#define SHADOW_OFFSET 10.0f   /* how far the wide shadow drops */
#define SHADOW_SIGMA  16.0f
#define SHADOW_SPREAD 64.0f   /* how far around the window it reaches */

static void draw_shadow( const struct window_draw *draw_ )
{
    const struct frect *r = &draw_->shown;
    struct clip clip = draw_->clip;
    struct push pc;

    clip.radius = max( clip.radius, 0.0f );
    push_init( &pc, &clip, draw_->opacity );
    push_color( &pc, SHADOW_OFFSET, SHADOW_SIGMA, 0.30f, 0.14f );
    push_rect( &pc, r->left - SHADOW_SPREAD, r->top - SHADOW_SPREAD, r->right + SHADOW_SPREAD,
               r->bottom + SHADOW_SPREAD + SHADOW_OFFSET );
    draw( PIPE_SHADOW, NULL, 0, &pc );
}

/* the thin line around a rectangle, on its outermost pixels */
static void draw_line( const struct window_draw *draw_, float r_, float g, float b, float a )
{
    const struct frect *r = &draw_->shown;
    struct clip clip = draw_->clip;
    struct push pc;

    clip.radius = max( clip.radius, 0.0f );
    push_init( &pc, &clip, draw_->opacity );
    push_color( &pc, r_, g, b, a );
    push_rect( &pc, r->left - 1, r->top - 1, r->right + 1, r->bottom + 1 );
    draw( PIPE_OUTLINE, NULL, 0, &pc );
}

static void draw_window( const struct window_draw *draw_, int ox, int oy )
{
    struct window_state *ws = draw_->ws;
    struct thumbnail_draw thumbs[32];
    uint32_t count;

    if (draw_->preview)
    {
        /* where a dragged window would snap: the light acrylic with a lighter
         * edge, white at a half, premultiplied */
        draw_backdrop( draw_, ox, oy );
        draw_line( draw_, 0.5f, 0.5f, 0.5f, 0.5f );
        return;
    }
    if (draw_->decorated) draw_shadow( draw_ );
    if (draw_->backdrop) draw_backdrop( draw_, ox, oy );

    if (draw_->from_snapshot)
    {
        if (ws && ws->snapshot_valid) draw_image( &ws->snapshot, &draw_->shown, NULL, &draw_->clip, draw_->opacity );
        if (draw_->live_opacity > 0)
        {
            /* the window as it is now fades in over what it showed before */
            struct layer_ctx ctx = { draw_, &draw_->live_xform, &draw_->clip, draw_->opacity * draw_->live_opacity };
            scene_surfaces( draw_, draw_tree_callback, &ctx );
        }
    }
    else if (draw_->as_image && ws && needs_image( draw_ ) && render_window_image( draw_, &ws->scratch, false ))
    {
        draw_image( &ws->scratch, &draw_->shown, NULL, &draw_->clip, draw_->opacity );
    }
    else
    {
        struct layer_ctx ctx = { draw_, &draw_->xform, draw_->clip.radius > 0 ? &draw_->clip : NULL, draw_->opacity };
        scene_surfaces( draw_, draw_tree_callback, &ctx );
    }
    /* black at a fifth: the light grey line of Windows 11 over a white frame */
    if (draw_->decorated) draw_line( draw_, 0, 0, 0, 0.2f );

    count = scene_thumbnails( draw_, thumbs, ARRAY_SIZE(thumbs) );
    for (uint32_t i = 0; i < count; i++) draw_thumbnail( &thumbs[i] );
}

static void draw_cursor(void)
{
    struct texture *t = cursor_texture;
    struct push pc;

    if (cursor_hidden) return;
    update_cursor_image();
    if (cursor_image.width <= 0 || cursor_image.height <= 0) return;
    if (t && (t->width != cursor_image.width || t->height != cursor_image.height))
    {
        texture_free( t );
        t = cursor_texture = NULL;
    }
    if (!t)
    {
        if (!(t = cursor_texture = texture_create( cursor_image.width, cursor_image.height, false ))) return;
        cursor_texture_serial = cursor_image.serial - 1;
    }
    if (cursor_texture_serial != cursor_image.serial)
    {
        if (!upload( t, cursor_image.pixels )) return;
        cursor_texture_serial = cursor_image.serial;
    }
    push_init( &pc, NULL, 1 );
    pc.mode = LAYER_PREMULTIPLIED;
    push_rect( &pc, cursor_x - cursor_image.hot_x, cursor_y - cursor_image.hot_y,
               cursor_x - cursor_image.hot_x + cursor_image.width, cursor_y - cursor_image.hot_y + cursor_image.height );
    draw( PIPE_TEXTURE, t, SAMPLER_NEAREST, &pc );
}

static enum layer_mode plain_mode( const struct surface *s, const struct window_draw *draw_ )
{
    return s->alpha ? LAYER_PREMULTIPLIED : LAYER_OPAQUE;
}

/* the desktop from ox,oy on, into an image of width x height */
static void draw_scene( struct texture *target, int ox, int oy, int width, int height )
{
    static struct window_draw draws[512];
    struct window_draw plain;
    struct xform identity = { 1, 1, 0, 0 };
    struct layer_ctx ctx = { &plain, &identity, NULL, 1 };
    uint32_t count;

    set_target( target, ox, oy, width, height );
    clear( ((background >> 16) & 0xff) / 255.0f, ((background >> 8) & 0xff) / 255.0f, (background & 0xff) / 255.0f, 1 );

    count = scene_windows( draws, ARRAY_SIZE(draws) );
    for (uint32_t i = 0; i < count; i++)
    {
        const struct window_draw *draw_ = &draws[i];

        if (draw_->shown.right <= ox || draw_->shown.bottom <= oy || draw_->shown.left >= ox + width ||
            draw_->shown.top >= oy + height)
            continue;  /* on another monitor */
        draw_window( draw_, ox, oy );
    }

    memset( &plain, 0, sizeof(plain) );
    plain.mode = plain_mode;
    scene_unowned_surfaces( draw_tree_callback, &ctx );
    draw_cursor();
}

/**********************************************************************
 *          Monitors
 */

static void destroy_frame( struct gpu_frame *frame )
{
    if (frame->fb && frame->fb_generation == kms.generation) kms_remove_dmabuf( frame->fb );
    texture_free( frame->texture );
    free( frame );
}

static void free_output( struct gpu_output *out )
{
    struct gpu_output **p;

    if (out->locked) return;  /* once KMS lets its last frame go */
    for (p = &gpu_outputs; *p; p = &(*p)->next) if (*p == out) { *p = out->next; break; }
    for (int i = 0; i < OUTPUT_FRAMES; i++) if (out->frames[i]) destroy_frame( out->frames[i] );
    texture_free( out->copy );
    free( out );
}

static struct gpu_output *output_frames( struct output *output, int width, int height )
{
    struct gpu_output *out = output->gpu;

    if (out && (out->width != width || out->height != height))
    {
        /* a new mode: frames of the new size once the old ones are off screen */
        if (out->locked) return NULL;
        out->owner = NULL;
        output->gpu = NULL;
        free_output( out );
        out = NULL;
    }
    if (out) return out;

    if (!(out = calloc( 1, sizeof(*out) ))) return NULL;
    out->owner = output;
    out->width = width;
    out->height = height;
    out->next = gpu_outputs;
    gpu_outputs = out;
    output->gpu = out;
    return out;
}

void gpu_output_gone( struct output *output )
{
    struct gpu_output *out = output->gpu;

    if (!out) return;
    output->gpu = NULL;
    out->owner = NULL;
    free_output( out );
}

/* the layouts an image we draw into and the card then shows may have: one plane, the way set now */
static uint32_t scanout_modifiers( uint64_t *modifiers, uint32_t max_count, int width, int height )
{
    VkDrmFormatModifierPropertiesEXT list[64];
    VkDrmFormatModifierPropertiesListEXT mods =
    {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        .drmFormatModifierCount = ARRAY_SIZE(list),
        .pDrmFormatModifierProperties = list,
    };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &mods };
    const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    uint32_t count = 0;

    vkGetPhysicalDeviceFormatProperties2( physical, FORMAT, &props );
    for (uint32_t i = 0; i < mods.drmFormatModifierCount && i < ARRAY_SIZE(list) && count < max_count; i++)
    {
        if (list[i].drmFormatModifierPlaneCount != 1) continue;
        if ((list[i].drmFormatModifierTilingFeatures & features) != features) continue;
        if (scanout == SCANOUT_LINEAR && list[i].drmFormatModifier != DRM_FORMAT_MOD_LINEAR) continue;
        if (!modifier_usable( list[i].drmFormatModifier, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              true, width, height ))
            continue;
        modifiers[count++] = list[i].drmFormatModifier;
    }
    return count;
}

/* an image the card scans out: drawn into here, a framebuffer there */
static struct gpu_frame *create_frame( struct gpu_output *out )
{
    uint64_t modifiers[64];
    VkImageDrmFormatModifierListCreateInfoEXT list =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pDrmFormatModifiers = modifiers,
    };
    VkExternalMemoryImageCreateInfo external =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FORMAT,
        .extent = { out->width, out->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    VkExportMemoryAllocateInfo export =
    {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export };
    VkMemoryGetFdInfoKHR get_fd =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageDrmFormatModifierPropertiesEXT modifier = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    VkImageSubresource plane = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout layout;
    VkMemoryRequirements reqs;
    struct gpu_frame *frame;
    struct texture *t;
    int fd = -1;

    if (!(list.drmFormatModifierCount = scanout_modifiers( modifiers, ARRAY_SIZE(modifiers), out->width, out->height )))
        return NULL;
    if (!(frame = calloc( 1, sizeof(*frame) ))) return NULL;
    if (!(t = calloc( 1, sizeof(*t) )))
    {
        free( frame );
        return NULL;
    }
    t->width = out->width;
    t->height = out->height;
    t->levels = 1;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->fd = -1;
    if (vkCreateImage( device, &info, NULL, &t->image ) != VK_SUCCESS) goto failed;
    vkGetImageMemoryRequirements( device, t->image, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, 0 );
    if (alloc.memoryTypeIndex == UINT32_MAX) goto failed;
    dedicated.image = t->image;
    if (vkAllocateMemory( device, &alloc, NULL, &t->memory ) != VK_SUCCESS) goto failed;
    if (vkBindImageMemory( device, t->image, t->memory, 0 ) != VK_SUCCESS) goto failed;
    if (!texture_views( t )) goto failed;
    if (get_image_modifier( device, t->image, &modifier ) != VK_SUCCESS) goto failed;
    vkGetImageSubresourceLayout( device, t->image, &plane, &layout );
    get_fd.memory = t->memory;
    if (get_memory_fd( device, &get_fd, &fd ) != VK_SUCCESS || fd < 0) goto failed;
    frame->fb = kms_add_dmabuf( fd, out->width, out->height, DRM_FORMAT_XRGB8888, layout.offset, layout.rowPitch,
                                modifier.drmFormatModifier );
    close( fd );
    if (!frame->fb) goto failed;
    TRACE( "%dx%d, layout %#llx, pitch %u\n", out->width, out->height,
           (unsigned long long)modifier.drmFormatModifier, (unsigned int)layout.rowPitch );
    texture_link( t );
    frame->out = out;
    frame->texture = t;
    frame->fb_generation = kms.generation;
    frame->mode = scanout;
    return frame;

failed:
    texture_destroy( t );
    free( frame );
    return NULL;
}

/* the card does not take frames the way they were made: the next way down */
static void scanout_down( const char *why )
{
    scanout++;
    MESSAGE( "dwm: %s: %s\n", why, scanout == SCANOUT_LINEAR ? "frames laid out row after row"
                                                             : "frames copied to the card" );
    for (struct gpu_output *out = gpu_outputs; out; out = out->next) out->tested = false;
}

/* A frame to draw into, NULL when the monitor has all of them (*busy) or
 * frames are copied from now on. */
static struct gpu_frame *next_frame( struct gpu_output *out, struct kms_output *o, bool *busy )
{
    *busy = false;
    for (;;)
    {
        int empty = -1;

        for (int i = 0; i < OUTPUT_FRAMES; i++)
        {
            struct gpu_frame *frame = out->frames[i];

            if (frame && !frame->held && frame->mode != scanout)
            {
                destroy_frame( frame );
                frame = out->frames[i] = NULL;
            }
            if (!frame) empty = i;
        }
        if (scanout == SCANOUT_COPY) return NULL;
        for (int i = 0; i < OUTPUT_FRAMES; i++)
            if (out->frames[i] && !out->frames[i]->held && out->frames[i]->mode == scanout) return out->frames[i];
        if (empty < 0)
        {
            *busy = true;
            return NULL;
        }
        /* without atomic modesetting nothing says beforehand what a flip takes */
        if (scanout == SCANOUT_BEST && !o->plane)
        {
            scanout_down( "the card flips the old way" );
            continue;
        }
        if (!(out->frames[empty] = create_frame( out )))
        {
            scanout_down( "the card takes no image of the GPU as a frame" );
            continue;
        }
        if (o->plane && !out->tested &&
            !kms_can_scanout( o, out->frames[empty]->fb, out->width, out->height ))
        {
            destroy_frame( out->frames[empty] );
            out->frames[empty] = NULL;
            scanout_down( "the monitor's plane does not show such an image" );
            continue;
        }
        out->tested = true;
        return out->frames[empty];
    }
}

static void hold_frame( struct gpu_frame *frame )
{
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        if (held_frames[i]) continue;
        held_frames[i] = frame;
        frame->held = true;
        frame->out->locked++;
        return;
    }
}

bool gpu_frame_released( void *buffer )
{
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        struct gpu_frame *frame = buffer;
        struct gpu_output *out;

        if (!buffer || held_frames[i] != buffer) continue;
        held_frames[i] = NULL;
        out = frame->out;
        frame->held = false;
        out->locked--;
        if (frame->dead)
        {
            /* of a device that is gone: only the names were left */
            free( frame );
            if (!out->locked && out->dead) free( out );
        }
        else if (!out->owner) free_output( out );
        return true;
    }
    return false;
}

/* a drawn frame, as it was read back, into a dumb buffer of the card */
static void copy_to_fb( const uint8_t *data, int width, int height, struct kms_fb *fb )
{
    const uint32_t *src = (const uint32_t *)data;

    if (!fb || !fb->pixels) return;
    if ((int)fb->width == width && (int)fb->height == height)
    {
        for (int y = 0; y < height; y++)
            memcpy( (uint8_t *)fb->pixels + (size_t)y * fb->pitch, src + (size_t)y * width, (size_t)width * 4 );
        return;
    }
    /* a mode the card scales: the buffer has the monitor's own size */
    for (uint32_t y = 0; y < fb->height; y++)
    {
        const uint32_t *row = src + (size_t)(y * height / fb->height) * width;
        uint32_t *dst = (uint32_t *)((uint8_t *)fb->pixels + (size_t)y * fb->pitch);

        for (uint32_t x = 0; x < fb->width; x++) dst[x] = row[x * width / fb->width];
    }
}

bool gpu_render( struct output *output, bool vrr )
{
    struct kms_output *o = output->kms;
    struct gpu_frame *frame = NULL;
    struct kms_fb *fb = NULL;
    const uint8_t *data = NULL;
    struct gpu_output *out;
    struct texture *target;
    int width, height;
    bool busy;

    if (!gpu_active() || !o) return false;
    width = o->modes[o->mode].width;   /* a scaled mode: the card scales the frame up */
    height = o->modes[o->mode].height;
    if (!(out = output_frames( output, width, height ))) return false;

    if (o->no_flip && scanout != SCANOUT_COPY)
    {
        scanout = SCANOUT_COPY - 1;
        scanout_down( "the driver cannot flip" );
    }
    if ((frame = next_frame( out, o, &busy ))) target = frame->texture;
    else if (busy) return false;
    else
    {
        if (!(fb = o->no_flip ? kms_front_buffer( o ) : kms_back_buffer( o ))) return false;
        if (!out->copy && !(out->copy = texture_create( width, height, false )))
        {
            give_up( "no memory of the GPU for a frame" );
            return false;
        }
        target = out->copy;
    }

    if (!begin_frame()) return false;
    draw_scene( target, o->x, o->y, width, height );
    if (frame)
    {
        /* the card reads it as its layout has it; the next frame draws it anew */
        end_pass();
        barrier( draw_cmd, target, 0, 1, target->layout, VK_IMAGE_LAYOUT_GENERAL, queue_family,
                 VK_QUEUE_FAMILY_FOREIGN_EXT );
        target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    else data = read_texture( target );
    if (!end_frame()) return false;

    /* the pointer is drawn into the frame */
    kms_show_cursor( o, false, 0, 0 );
    if (frame)
    {
        hold_frame( frame );
        if (!frame->held) return false;
        if (kms_present( o, frame->fb, width, height, vrr, frame, true ))
        {
            trial_passed();
            return true;
        }
        gpu_frame_released( frame );
        return false;  /* busy; or it cannot flip, and the next frame is copied */
    }

    if (!data) return false;
    copy_to_fb( data, width, height, fb );
    if (!o->no_flip)
    {
        if (kms_present( o, fb->fb_id, fb->width, fb->height, vrr, NULL, true ))
        {
            trial_passed();
            return true;
        }
        if (!o->no_flip) return false;  /* busy: the next tick tries again */
        copy_to_fb( data, width, height, kms_front_buffer( o ) );
    }
    /* a driver that cannot flip: the frame is drawn where it shows */
    kms_flush( o );
    compositor_frame_in_place( output );
    trial_passed();
    return true;
}

/**********************************************************************
 *          Screenshots
 *
 * The desktop as it is composed, without the pointer, as Windows has it on
 * PrintScreen and in BitBlt from the screen: drawn again into an image of
 * its own and read back, BGRA, top row first, opaque.
 */
bool gpu_capture( int x, int y, int width, int height, uint32_t *pixels )
{
    bool hidden = cursor_hidden, ok;
    size_t count = (size_t)width * height;
    const uint32_t *data;

    if (!compositing || !init_device()) return false;
    if (capture_target && (capture_target->width != width || capture_target->height != height))
    {
        texture_free( capture_target );
        capture_target = NULL;
    }
    if (!capture_target && !(capture_target = texture_create( width, height, false )))
    {
        ERR( "cannot draw a %dx%d screenshot\n", width, height );
        return false;
    }
    if (!begin_frame()) return false;
    cursor_hidden = true;
    draw_scene( capture_target, x, y, width, height );
    cursor_hidden = hidden;
    data = (const uint32_t *)read_texture( capture_target );
    ok = end_frame() && data;
    for (size_t i = 0; ok && i < count; i++) pixels[i] = data[i] | 0xff000000;
    return ok;
}

/**********************************************************************
 *          Reading GPU buffers for the CPU
 *
 * When the CPU composes, or copies a window for a program that asked for
 * its picture, a window that draws with the GPU hands over a buffer in video
 * memory that the CPU often cannot map at all (amdgpu refuses buffers made
 * without CPU access). It is copied through the GPU.
 */
bool gpu_read_dmabuf( struct dmabuf *buffer, uint32_t *pixels )
{
    const uint8_t *data = NULL;
    struct texture *t;
    bool ok;

    if (!init_device() || !(import_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return false;
    if (in_frame || !begin_frame()) return false;
    if ((t = dmabuf_texture( buffer )))
    {
        data = read_texture( t );
        set_image_layout( draw_cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
    }
    ok = end_frame() && data;
    if (ok) memcpy( pixels, data, (size_t)buffer->width * buffer->height * 4 );
    else
    {
        static unsigned int logged;
        if (logged++ < 8) ERR( "cannot read a %ux%u buffer back\n", buffer->width, buffer->height );
    }
    return ok;
}
