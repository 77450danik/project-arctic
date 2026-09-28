/*
 * Arctic desktop composition engine: reading GPU buffers
 *
 * A window that draws with the GPU (Vulkan, OpenGL, Direct3D through DXVK)
 * hands over a buffer in video memory. On a discrete card the CPU cannot
 * map it at all (amdgpu refuses buffers made without CPU access), and where
 * it can, reading video memory from the CPU is slow. So the buffer is read
 * through the GPU: EGL imports it, and a framebuffer read copies it into the
 * memory the CPU composition works in. The GPU waits for the client's
 * rendering on its own (implicit fences), as a scanout would.
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
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "dwmcore_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

static struct gbm_device *gbm;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static GLuint framebuffer, texture;
static uint32_t generation;       /* of the card the context is on */
static bool failed;               /* on that card */

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

static bool has_extension( const char *list, const char *name )
{
    size_t len = strlen( name );

    for (const char *p = list; p && (p = strstr( p, name )); p += len)
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || !p[len])) return true;
    return false;
}

static void shutdown_gpu(void)
{
    if (display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        if (context != EGL_NO_CONTEXT) eglDestroyContext( display, context );
        eglTerminate( display );
    }
    if (gbm) gbm_device_destroy( gbm );
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    gbm = NULL;
    framebuffer = texture = 0;
}

/* a GLES context on the card dwm drives, made again when the card changes */
static bool init_gpu(void)
{
    static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    const char *extensions;

    if (generation == kms.generation)
    {
        if (failed) return false;
        if (display != EGL_NO_DISPLAY) return eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, context );
    }
    shutdown_gpu();
    generation = kms.generation;
    failed = true;

    if (kms.fd < 0 || !(gbm = gbm_create_device( kms.fd )))
    {
        ERR( "no GBM device on the card\n" );
        return false;
    }
    get_platform_display = (void *)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
    display = get_platform_display ? get_platform_display( EGL_PLATFORM_GBM_KHR, gbm, NULL ) : EGL_NO_DISPLAY;
    if (display == EGL_NO_DISPLAY || !eglInitialize( display, NULL, NULL ))
    {
        ERR( "no EGL display on the card\n" );
        display = EGL_NO_DISPLAY;
        return false;
    }
    extensions = eglQueryString( display, EGL_EXTENSIONS );
    if (!has_extension( extensions, "EGL_EXT_image_dma_buf_import" ) ||
        !has_extension( extensions, "EGL_KHR_surfaceless_context" ) ||
        !has_extension( extensions, "EGL_KHR_no_config_context" ))
    {
        ERR( "EGL of the card cannot import buffers: %s\n", extensions );
        return false;
    }
    eglBindAPI( EGL_OPENGL_ES_API );
    context = eglCreateContext( display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs );
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, context ))
    {
        ERR( "no GLES context on the card: %#x\n", eglGetError() );
        return false;
    }
    create_image = (void *)eglGetProcAddress( "eglCreateImageKHR" );
    destroy_image = (void *)eglGetProcAddress( "eglDestroyImageKHR" );
    image_target_texture = (void *)eglGetProcAddress( "glEGLImageTargetTexture2DOES" );
    if (!create_image || !destroy_image || !image_target_texture ||
        !has_extension( (const char *)glGetString( GL_EXTENSIONS ), "GL_EXT_read_format_bgra" ))
    {
        ERR( "GLES of the card cannot read buffers back\n" );
        return false;
    }
    glGenFramebuffers( 1, &framebuffer );
    glGenTextures( 1, &texture );
    TRACE( "GPU reads on %s\n", (const char *)glGetString( GL_RENDERER ) );
    failed = false;
    return true;
}

/* Copies a buffer through the GPU into pixels (width x height, packed);
 * false if this card cannot, and the CPU tries instead. */
bool gpu_read_dmabuf( struct dmabuf *buffer, uint32_t *pixels )
{
    EGLint attribs[32], n = 0;
    EGLImageKHR image;
    bool ok;

    if (!init_gpu()) return false;

    attribs[n++] = EGL_WIDTH;                      attribs[n++] = buffer->width;
    attribs[n++] = EGL_HEIGHT;                     attribs[n++] = buffer->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;       attribs[n++] = buffer->format;
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;      attribs[n++] = buffer->fd;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;  attribs[n++] = buffer->offset;
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;   attribs[n++] = buffer->stride;
    if (buffer->modifier != DRM_FORMAT_MOD_INVALID)
    {
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attribs[n++] = buffer->modifier & 0xffffffff;
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attribs[n++] = buffer->modifier >> 32;
    }
    attribs[n++] = EGL_NONE;

    image = create_image( display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs );
    if (image == EGL_NO_IMAGE_KHR)
    {
        static unsigned int logged;
        if (logged++ < 8) ERR( "cannot import a %ux%u buffer: %#x\n", buffer->width, buffer->height, eglGetError() );
        return false;
    }

    glBindTexture( GL_TEXTURE_2D, texture );
    image_target_texture( GL_TEXTURE_2D, image );
    glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
    ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
    {
        /* row 0 of the buffer is row 0 of the texture: no flip */
        glPixelStorei( GL_PACK_ALIGNMENT, 4 );
        glReadPixels( 0, 0, buffer->width, buffer->height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels );
        ok = glGetError() == GL_NO_ERROR;
    }
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    glBindTexture( GL_TEXTURE_2D, 0 );
    destroy_image( display, image );
    if (!ok)
    {
        static unsigned int logged;
        if (logged++ < 8) ERR( "cannot read a %ux%u buffer back\n", buffer->width, buffer->height );
    }
    return ok;
}
