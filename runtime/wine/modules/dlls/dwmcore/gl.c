/*
 * Arctic desktop composition engine: composition on the GPU
 *
 * Every monitor gets a GBM surface on the card dwm drives, and its frames
 * are drawn with OpenGL ES into that surface's buffers, which are flipped to
 * the monitor. Windows drawn with the GPU (Vulkan, Direct3D through DXVK)
 * are sampled straight from their buffers, without a copy; windows drawn by
 * GDI are uploaded as textures when they change. The effects of Windows 11
 * are drawn here: rounded corners, the acrylic backdrop (the desktop behind
 * a window blurred, tinted and grained), windows zoomed and faded while they
 * come and go, and live thumbnails for DwmRegisterThumbnail.
 *
 * Without a GPU (a virtual machine, a card Mesa has no driver for), Mesa's
 * software rasterizer draws the same frames into dumb buffers. Only when EGL
 * does not start at all does the compositor compose on the CPU.
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
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
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
#include "winbase.h"
#include "winuser.h"
#include "wine/debug.h"

#include "compositor.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

#define BLUR_LEVELS 3     /* halvings of the dual Kawase blur */
#define BLUR_OFFSET 2.0f
#define BLUR_MARGIN 48    /* what lies this far outside a window still blurs into its backdrop */
#define MAX_FRAMES  32

static struct gbm_device *gbm;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLConfig  config;
static uint32_t   card_generation;        /* of the card the context is on */
static uint32_t   gl_generation;          /* moves with every context: older textures are gone */
static bool       failed;                 /* no context on that card */
static bool       compositing;            /* frames are composed here */
static bool       gles3, has_bgra, has_unpack, has_external;
static GLint      max_texture;
static GLuint     read_framebuffer, read_texture;  /* gpu_read_dmabuf */
static GLuint     cursor_texture;

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
static PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_window_surface;

struct program
{
    GLuint id;
    GLint  proj, tex, opacity, clip, radius, mode, swizzle, color, tint, noise, half, offset;
};

static struct program prog_texture, prog_external, prog_color, prog_down, prog_up, prog_acrylic;
static GLfloat        proj[4];            /* screen to clip space: xy * proj.xy + proj.zw */

/* the frames of one monitor */
struct gl_output
{
    struct output      *owner;            /* NULL once its monitor went */
    struct gbm_surface *surface;
    EGLSurface          egl;
    int                 width, height;
    int                 locked;           /* buffers KMS still holds */
    struct gl_output   *next;
};

/* a buffer of a monitor's surface as a KMS framebuffer; it lives as long as the buffer */
struct gl_frame
{
    struct gl_output *out;
    struct gbm_bo    *bo;
    uint32_t          fb;
    bool              held;               /* with KMS: on screen or about to be */
};

static struct gl_output *gl_outputs;
static struct gl_frame  *held_frames[MAX_FRAMES];

/* the blur's levels: the copy of what lies behind, then each halving */
static struct { GLuint texture, framebuffer; int width, height; } blur[BLUR_LEVELS + 1];

static bool has_extension( const char *list, const char *name )
{
    size_t len = strlen( name );

    for (const char *p = list; p && (p = strstr( p, name )); p += len)
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || !p[len])) return true;
    return false;
}

/**********************************************************************
 *          Shaders
 */

static const char vertex_shader[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec4 u_proj;\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_pos;\n"
    "void main()\n"
    "{\n"
    "    v_uv = a_uv;\n"
    "    v_pos = a_pos;\n"
    "    gl_Position = vec4( a_pos * u_proj.xy + u_proj.zw, 0.0, 1.0 );\n"
    "}\n";

/* what lies outside the rounded rectangle u_clip is cut off, with a soft edge */
static const char fragment_common[] =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_pos;\n"
    "uniform vec4 u_clip;\n"
    "uniform float u_radius;\n"
    "uniform float u_opacity;\n"
    "float coverage()\n"
    "{\n"
    "    if (u_radius < 0.0) return 1.0;\n"
    "    vec2 c = (u_clip.xy + u_clip.zw) * 0.5;\n"
    "    vec2 h = (u_clip.zw - u_clip.xy) * 0.5;\n"
    "    vec2 q = abs( v_pos - c ) - h + vec2( u_radius );\n"
    "    float d = length( max( q, 0.0 ) ) + min( max( q.x, q.y ), 0.0 ) - u_radius;\n"
    "    return clamp( 0.5 - d, 0.0, 1.0 );\n"
    "}\n";

/* u_mode 0: opaque, 1: premultiplied, 2: GDI over a backdrop (see LAYER_KEYED) */
static const char fragment_texture[] =
    "uniform SAMPLER u_tex;\n"
    "uniform float u_mode;\n"
    "uniform float u_swizzle;\n"
    "void main()\n"
    "{\n"
    "    vec4 t = texture2D( u_tex, v_uv );\n"
    "    if (u_swizzle > 0.5) t = t.bgra;\n"
    "    if (u_mode < 0.5) t.a = 1.0;\n"
    "    else if (u_mode > 1.5)\n"
    "    {\n"
    "        if (t.a < 0.002) t.a = max( t.r, max( t.g, t.b ) );\n"
    "        t.rgb = min( t.rgb, vec3( t.a ) );\n"
    "    }\n"
    "    gl_FragColor = t * (u_opacity * coverage());\n"
    "}\n";

static const char fragment_color[] =
    "uniform vec4 u_color;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = u_color * (u_opacity * coverage());\n"
    "}\n";

/* the dual Kawase blur: a halving that blurs, then a doubling that blurs */
static const char fragment_down[] =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_half;\n"
    "uniform float u_offset;\n"
    "void main()\n"
    "{\n"
    "    vec2 o = u_half * u_offset;\n"
    "    vec4 s = texture2D( u_tex, v_uv ) * 4.0;\n"
    "    s += texture2D( u_tex, v_uv - o );\n"
    "    s += texture2D( u_tex, v_uv + o );\n"
    "    s += texture2D( u_tex, v_uv + vec2( o.x, -o.y ) );\n"
    "    s += texture2D( u_tex, v_uv - vec2( o.x, -o.y ) );\n"
    "    gl_FragColor = s / 8.0;\n"
    "}\n";

static const char fragment_up[] =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_half;\n"
    "uniform float u_offset;\n"
    "void main()\n"
    "{\n"
    "    vec2 o = u_half * u_offset;\n"
    "    vec4 s = texture2D( u_tex, v_uv + vec2( -o.x * 2.0, 0.0 ) );\n"
    "    s += texture2D( u_tex, v_uv + vec2( -o.x, o.y ) ) * 2.0;\n"
    "    s += texture2D( u_tex, v_uv + vec2( 0.0, o.y * 2.0 ) );\n"
    "    s += texture2D( u_tex, v_uv + vec2( o.x, o.y ) ) * 2.0;\n"
    "    s += texture2D( u_tex, v_uv + vec2( o.x * 2.0, 0.0 ) );\n"
    "    s += texture2D( u_tex, v_uv + vec2( o.x, -o.y ) ) * 2.0;\n"
    "    s += texture2D( u_tex, v_uv + vec2( 0.0, -o.y * 2.0 ) );\n"
    "    s += texture2D( u_tex, v_uv + vec2( -o.x, -o.y ) ) * 2.0;\n"
    "    gl_FragColor = s / 12.0;\n"
    "}\n";

/* the acrylic of Windows: the blur a little more saturated, the tint over
 * it, and a faint grain */
static const char fragment_acrylic[] =
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_noise;\n"
    "void main()\n"
    "{\n"
    "    vec3 c = texture2D( u_tex, v_uv ).rgb;\n"
    "    float l = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );\n"
    "    c = clamp( mix( vec3( l ), c, 1.25 ), 0.0, 1.0 );\n"
    "    c = mix( c, u_tint.rgb, u_tint.a );\n"
    "    float n = fract( sin( dot( floor( v_pos ), vec2( 12.9898, 78.233 ) ) ) * 43758.5453 );\n"
    "    c += (n - 0.5) * u_noise;\n"
    "    gl_FragColor = vec4( clamp( c, 0.0, 1.0 ), 1.0 ) * (u_opacity * coverage());\n"
    "}\n";

static GLuint compile( GLenum type, const char *const *sources, int count )
{
    GLuint shader = glCreateShader( type );
    GLint ok;

    glShaderSource( shader, count, sources, NULL );
    glCompileShader( shader );
    glGetShaderiv( shader, GL_COMPILE_STATUS, &ok );
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog( shader, sizeof(log), NULL, log );
        ERR( "shader: %s\n", log );
        glDeleteShader( shader );
        return 0;
    }
    return shader;
}

static bool link_program( struct program *p, const char *body, const char *prefix )
{
    const char *vs[] = { vertex_shader };
    const char *fs[] = { prefix, fragment_common, body };
    GLuint v, f;
    GLint ok;

    memset( p, 0, sizeof(*p) );
    if (!(v = compile( GL_VERTEX_SHADER, vs, 1 ))) return false;
    if (!(f = compile( GL_FRAGMENT_SHADER, fs, 3 )))
    {
        glDeleteShader( v );
        return false;
    }
    p->id = glCreateProgram();
    glAttachShader( p->id, v );
    glAttachShader( p->id, f );
    glBindAttribLocation( p->id, 0, "a_pos" );
    glBindAttribLocation( p->id, 1, "a_uv" );
    glLinkProgram( p->id );
    glDeleteShader( v );
    glDeleteShader( f );
    glGetProgramiv( p->id, GL_LINK_STATUS, &ok );
    if (!ok)
    {
        char log[1024];
        glGetProgramInfoLog( p->id, sizeof(log), NULL, log );
        ERR( "program: %s\n", log );
        glDeleteProgram( p->id );
        p->id = 0;
        return false;
    }
    p->proj = glGetUniformLocation( p->id, "u_proj" );
    p->tex = glGetUniformLocation( p->id, "u_tex" );
    p->opacity = glGetUniformLocation( p->id, "u_opacity" );
    p->clip = glGetUniformLocation( p->id, "u_clip" );
    p->radius = glGetUniformLocation( p->id, "u_radius" );
    p->mode = glGetUniformLocation( p->id, "u_mode" );
    p->swizzle = glGetUniformLocation( p->id, "u_swizzle" );
    p->color = glGetUniformLocation( p->id, "u_color" );
    p->tint = glGetUniformLocation( p->id, "u_tint" );
    p->noise = glGetUniformLocation( p->id, "u_noise" );
    p->half = glGetUniformLocation( p->id, "u_half" );
    p->offset = glGetUniformLocation( p->id, "u_offset" );
    return true;
}

static bool init_programs(void)
{
    static const char sampler_2d[] = "#define SAMPLER sampler2D\n";
    static const char sampler_external[] =
        "#extension GL_OES_EGL_image_external : require\n#define SAMPLER samplerExternalOES\n";

    if (!link_program( &prog_texture, fragment_texture, sampler_2d ) ||
        !link_program( &prog_color, fragment_color, "" ) ||
        !link_program( &prog_down, fragment_down, "" ) ||
        !link_program( &prog_up, fragment_up, "" ) ||
        !link_program( &prog_acrylic, fragment_acrylic, "" ))
        return false;
    if (has_external && !link_program( &prog_external, fragment_texture, sampler_external )) has_external = false;
    return true;
}

static void use_program( const struct program *p )
{
    glUseProgram( p->id );
    glUniform4fv( p->proj, 1, proj );
    if (p->tex >= 0) glUniform1i( p->tex, 0 );
}

/* screen coordinates to clip space; flipped: the top lands in the texture's first row */
static void set_projection( float left, float top, float width, float height, bool flipped )
{
    proj[0] = 2 / width;
    proj[2] = -1 - 2 * left / width;
    if (flipped)
    {
        proj[1] = 2 / height;
        proj[3] = -1 - 2 * top / height;
    }
    else
    {
        proj[1] = -2 / height;
        proj[3] = 1 + 2 * top / height;
    }
}

static void set_clip( const struct program *p, const struct clip *clip, float opacity )
{
    glUniform1f( p->opacity, opacity );
    if (!clip)
    {
        glUniform1f( p->radius, -1 );
        return;
    }
    glUniform4f( p->clip, clip->rect.left, clip->rect.top, clip->rect.right, clip->rect.bottom );
    glUniform1f( p->radius, clip->radius );
}

static void draw_quad( float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1 )
{
    const GLfloat v[] = { x0, y0, u0, v0,  x1, y0, u1, v0,  x0, y1, u0, v1,  x1, y1, u1, v1 };

    glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v );
    glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), v + 2 );
    glEnableVertexAttribArray( 0 );
    glEnableVertexAttribArray( 1 );
    glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
}

/**********************************************************************
 *          The context
 */

static void free_output( struct gl_output *out );

static void shutdown_gl(void)
{
    struct gl_output *out, *next;

    for (out = gl_outputs; out; out = next)
    {
        next = out->next;
        if (out->owner) out->owner->gl = NULL;
        out->owner = NULL;
        out->locked = 0;
        free_output( out );
    }
    gl_outputs = NULL;
    for (int i = 0; i < MAX_FRAMES; i++) held_frames[i] = NULL;
    memset( blur, 0, sizeof(blur) );
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
    read_framebuffer = read_texture = cursor_texture = 0;
    compositing = false;
    gl_generation++;
}

static bool choose_config(void)
{
    static const EGLint attribs[] =
    {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
    };
    EGLConfig configs[64];
    EGLint count = 0;

    if (!eglChooseConfig( display, attribs, configs, ARRAY_SIZE(configs), &count )) return false;
    for (EGLint i = 0; i < count; i++)
    {
        EGLint id;

        if (!eglGetConfigAttrib( display, configs[i], EGL_NATIVE_VISUAL_ID, &id ) || id != GBM_FORMAT_XRGB8888)
            continue;
        config = configs[i];
        return true;
    }
    return false;
}

/* a GLES context on the card dwm drives, made again when the card changes */
static bool init_context(void)
{
    static const EGLint gles3_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    static const EGLint gles2_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    const char *extensions, *gl_extensions;
    bool no_config;

    if (card_generation == kms.generation)
    {
        if (failed) return false;
        if (display != EGL_NO_DISPLAY) return true;
    }
    shutdown_gl();
    card_generation = kms.generation;
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
        !has_extension( extensions, "EGL_KHR_surfaceless_context" ))
    {
        ERR( "EGL of the card cannot import buffers: %s\n", extensions );
        return false;
    }
    no_config = has_extension( extensions, "EGL_KHR_no_config_context" ) ||
                has_extension( extensions, "EGL_MESA_configless_context" );
    eglBindAPI( EGL_OPENGL_ES_API );
    if (!choose_config()) WARN( "no EGL config for XRGB8888 surfaces: composing on the CPU\n" );

    gles3 = true;
    context = eglCreateContext( display, no_config ? EGL_NO_CONFIG_KHR : config, EGL_NO_CONTEXT, gles3_attribs );
    if (context == EGL_NO_CONTEXT)
    {
        gles3 = false;
        context = eglCreateContext( display, no_config ? EGL_NO_CONFIG_KHR : config, EGL_NO_CONTEXT, gles2_attribs );
    }
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, context ))
    {
        ERR( "no GLES context on the card: %#x\n", eglGetError() );
        return false;
    }
    create_image = (void *)eglGetProcAddress( "eglCreateImageKHR" );
    destroy_image = (void *)eglGetProcAddress( "eglDestroyImageKHR" );
    image_target_texture = (void *)eglGetProcAddress( "glEGLImageTargetTexture2DOES" );
    create_window_surface = (void *)eglGetProcAddress( "eglCreatePlatformWindowSurfaceEXT" );
    gl_extensions = (const char *)glGetString( GL_EXTENSIONS );
    if (!create_image || !destroy_image || !image_target_texture)
    {
        ERR( "GLES of the card cannot take buffers of other processes\n" );
        return false;
    }
    has_bgra = has_extension( gl_extensions, "GL_EXT_texture_format_BGRA8888" );
    has_unpack = gles3 || has_extension( gl_extensions, "GL_EXT_unpack_subimage" );
    has_external = has_extension( gl_extensions, "GL_OES_EGL_image_external" );
    glGetIntegerv( GL_MAX_TEXTURE_SIZE, &max_texture );
    glGenFramebuffers( 1, &read_framebuffer );
    glGenTextures( 1, &read_texture );
    gl_generation++;
    failed = false;
    return true;
}

static bool make_current(void)
{
    if (!init_context()) return false;
    return eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, context );
}

bool gl_start(void)
{
    const char *cpu = getenv( "ARCTIC_DWM_CPU" );

    compositing = false;
    if (cpu && *cpu == '1') return false;
    if (!make_current() || !config) goto cpu;
    if (!init_programs()) goto cpu;
    compositing = true;
    MESSAGE( "dwm: composing on the GPU: %s (OpenGL ES %d)\n", (const char *)glGetString( GL_RENDERER ),
             gles3 ? 3 : 2 );
    return true;

cpu:
    MESSAGE( "dwm: composing on the CPU\n" );
    return false;
}

bool gl_active(void)
{
    /* a card that came after the last start */
    if (compositing && card_generation != kms.generation) gl_start();
    return compositing;
}

/* the card cannot show what we draw: the CPU composes from now on */
static void give_up( const char *why )
{
    MESSAGE( "dwm: %s: composing on the CPU\n", why );
    compositing = false;
}

/**********************************************************************
 *          Textures
 */

static void texture_parameters( GLenum target, GLint min_filter )
{
    glTexParameteri( target, GL_TEXTURE_MIN_FILTER, min_filter );
    glTexParameteri( target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri( target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}

static EGLImageKHR import_dmabuf( const struct dmabuf *buffer )
{
    EGLint attribs[32], n = 0;

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
    return create_image( display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs );
}

/* a client's GPU buffer as a texture, imported once */
static GLuint dmabuf_texture( struct dmabuf *buffer, bool *external )
{
    if (buffer->texture_generation != gl_generation)
    {
        buffer->image = NULL;
        buffer->texture = 0;
        buffer->texture_failed = false;
        buffer->texture_generation = gl_generation;
    }
    if (buffer->texture_failed) return 0;
    if (!buffer->texture)
    {
        if (!(buffer->image = import_dmabuf( buffer )))
        {
            static unsigned int logged;
            if (logged++ < 8) ERR( "cannot import a %ux%u buffer: %#x\n", buffer->width, buffer->height, eglGetError() );
            buffer->texture_failed = true;
            return 0;
        }
        glGenTextures( 1, &buffer->texture );
        glBindTexture( GL_TEXTURE_2D, buffer->texture );
        texture_parameters( GL_TEXTURE_2D, GL_LINEAR );
        while (glGetError()) {}
        image_target_texture( GL_TEXTURE_2D, buffer->image );
        buffer->texture_external = false;
        if (glGetError() != GL_NO_ERROR)
        {
            /* some drivers sample such buffers only as external images */
            glDeleteTextures( 1, &buffer->texture );
            buffer->texture = 0;
            if (has_external)
            {
                glGenTextures( 1, &buffer->texture );
                glBindTexture( GL_TEXTURE_EXTERNAL_OES, buffer->texture );
                texture_parameters( GL_TEXTURE_EXTERNAL_OES, GL_LINEAR );
                image_target_texture( GL_TEXTURE_EXTERNAL_OES, buffer->image );
                buffer->texture_external = true;
                if (glGetError() != GL_NO_ERROR)
                {
                    glDeleteTextures( 1, &buffer->texture );
                    buffer->texture = 0;
                }
            }
            if (!buffer->texture)
            {
                destroy_image( display, buffer->image );
                buffer->image = NULL;
                buffer->texture_failed = true;
                return 0;
            }
        }
    }
    *external = buffer->texture_external;
    return buffer->texture;
}

void gl_dmabuf_gone( struct dmabuf *buffer )
{
    if (buffer->texture_generation != gl_generation || display == EGL_NO_DISPLAY) return;
    if (!make_current()) return;
    if (buffer->texture) glDeleteTextures( 1, &buffer->texture );
    if (buffer->image) destroy_image( display, buffer->image );
    buffer->texture = 0;
    buffer->image = NULL;
}

/* the pixels of a surface drawn by GDI, uploaded when they changed */
static GLuint shm_texture( struct surface *s )
{
    GLenum format = has_bgra ? GL_BGRA_EXT : GL_RGBA;

    if (!s->pixels) return 0;
    if (s->texture_generation != gl_generation)
    {
        s->texture = 0;
        s->texture_generation = gl_generation;
    }
    if (!s->texture)
    {
        glGenTextures( 1, &s->texture );
        glBindTexture( GL_TEXTURE_2D, s->texture );
        texture_parameters( GL_TEXTURE_2D, GL_LINEAR );
        s->texture_width = s->texture_height = 0;
        s->texture_stale = true;
    }
    else glBindTexture( GL_TEXTURE_2D, s->texture );
    if (!s->texture_stale) return s->texture;

    glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
    if (s->texture_width != s->width || s->texture_height != s->height)
    {
        glTexImage2D( GL_TEXTURE_2D, 0, format, s->width, s->height, 0, format, GL_UNSIGNED_BYTE, s->pixels );
        s->texture_width = s->width;
        s->texture_height = s->height;
    }
    else glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, s->width, s->height, format, GL_UNSIGNED_BYTE, s->pixels );
    s->texture_stale = false;
    return s->texture;
}

void gl_surface_gone( struct surface *s )
{
    if (!s->texture || s->texture_generation != gl_generation || display == EGL_NO_DISPLAY) return;
    if (make_current()) glDeleteTextures( 1, &s->texture );
    s->texture = 0;
}

/* an image the size of a window, to draw it into */
static bool image_alloc( struct window_image *image, int width, int height )
{
    if (width <= 0 || height <= 0 || width > max_texture || height > max_texture) return false;
    if (image->generation != gl_generation)
    {
        image->texture = image->framebuffer = 0;
        image->width = image->height = 0;
        image->generation = gl_generation;
    }
    if (!image->texture)
    {
        glGenTextures( 1, &image->texture );
        glGenFramebuffers( 1, &image->framebuffer );
    }
    glBindTexture( GL_TEXTURE_2D, image->texture );
    if (image->width != width || image->height != height)
    {
        glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
        texture_parameters( GL_TEXTURE_2D, GL_LINEAR );
        glBindFramebuffer( GL_FRAMEBUFFER, image->framebuffer );
        glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0 );
        if (glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE)
        {
            glBindFramebuffer( GL_FRAMEBUFFER, 0 );
            return false;
        }
        image->width = width;
        image->height = height;
    }
    else glBindFramebuffer( GL_FRAMEBUFFER, image->framebuffer );
    return true;
}

void gl_image_free( struct window_image *image )
{
    free( image->pixels );
    image->pixels = NULL;
    if (image->texture && image->generation == gl_generation && display != EGL_NO_DISPLAY && make_current())
    {
        glDeleteTextures( 1, &image->texture );
        glDeleteFramebuffers( 1, &image->framebuffer );
    }
    image->texture = image->framebuffer = 0;
    image->width = image->height = 0;
}

static bool image_valid( const struct window_image *image )
{
    return image->texture && image->generation == gl_generation;
}

/* images are shrunk into thumbnails: smoothly, with mip levels where GLES 3 has them */
static void image_mipmaps( struct window_image *image )
{
    if (!gles3) return;
    glBindTexture( GL_TEXTURE_2D, image->texture );
    glGenerateMipmap( GL_TEXTURE_2D );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
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
    const struct program *p = &prog_texture;
    int sx0 = 0, sy0 = 0, sw = s->width, sh = s->height, dw, dh;
    enum layer_mode mode = ctx->draw->mode( s, ctx->draw );
    bool external = false;
    float x0, y0, x1, y1;
    GLuint texture;

    if (s->dmabuf) texture = dmabuf_texture( s->dmabuf, &external );
    else texture = shm_texture( s );
    if (!texture) return;
    if (external) p = &prog_external;

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

    x0 = x * ctx->xform->sx + ctx->xform->tx;
    y0 = y * ctx->xform->sy + ctx->xform->ty;
    x1 = (x + dw) * ctx->xform->sx + ctx->xform->tx;
    y1 = (y + dh) * ctx->xform->sy + ctx->xform->ty;

    use_program( p );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, texture );
    glUniform1f( p->mode, mode );
    glUniform1f( p->swizzle, !s->dmabuf && !has_bgra );
    set_clip( p, ctx->clip, ctx->opacity );
    draw_quad( x0, y0, x1, y1, (float)sx0 / s->width, (float)sy0 / s->height,
               (float)(sx0 + sw) / s->width, (float)(sy0 + sh) / s->height );
}

static void draw_tree( struct surface *s, int x, int y, const struct layer_ctx *ctx )
{
    struct surface *child;

    if (s->pixels || s->dmabuf) draw_layer( s, x, y, ctx );
    wl_list_for_each( child, &s->children, child_link )
    {
        if (child->hwnd) continue;  /* a window of its own */
        draw_tree( child, x + child->sub_x, y + child->sub_y, ctx );
    }
}

static void draw_tree_callback( struct surface *s, int x, int y, void *ctx )
{
    draw_tree( s, x, y, ctx );
}

static void draw_image( const struct window_image *image, const struct frect *dest, const RECT *source,
                        const struct clip *clip, float opacity )
{
    const struct program *p = &prog_texture;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;

    if (!image_valid( image )) return;
    if (source)
    {
        u0 = (float)source->left / image->width;
        v0 = (float)source->top / image->height;
        u1 = (float)source->right / image->width;
        v1 = (float)source->bottom / image->height;
    }
    use_program( p );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, image->texture );
    glUniform1f( p->mode, LAYER_PREMULTIPLIED );
    glUniform1f( p->swizzle, 0 );
    set_clip( p, clip, opacity );
    draw_quad( dest->left, dest->top, dest->right, dest->bottom, u0, v0, u1, v1 );
}

/* where frames go: a monitor's surface, or an image */
struct target
{
    GLuint framebuffer;
    int    width, height;
    GLfloat proj[4];
};

static struct target current_target;

static void set_target( const struct target *target )
{
    current_target = *target;
    glBindFramebuffer( GL_FRAMEBUFFER, target->framebuffer );
    glViewport( 0, 0, target->width, target->height );
    memcpy( proj, target->proj, sizeof(proj) );
}

/* the window as it is now, into an image of its size (top row first) */
static bool render_window_image( const struct window_draw *draw, struct window_image *image )
{
    const struct dwm_window *w = draw->w;
    struct xform identity = { 1, 1, 0, 0 };
    struct target saved = current_target;
    struct layer_ctx ctx = { draw, &identity, NULL, 1 };
    int width = w->right - w->left, height = w->bottom - w->top;

    if (!image_alloc( image, width, height ))
    {
        set_target( &saved );
        return false;
    }
    glViewport( 0, 0, width, height );
    set_projection( w->left, w->top, width, height, true );
    glClearColor( 0, 0, 0, 0 );
    glClear( GL_COLOR_BUFFER_BIT );
    scene_surfaces( draw, draw_tree_callback, &ctx );
    image_mipmaps( image );
    set_target( &saved );
    return true;
}

bool gl_snapshot( struct window_state *ws, const struct dwm_window *w )
{
    struct window_draw draw;
    struct target none = { 0 };

    if (!compositing || !make_current()) return false;
    scene_static_draw( &draw, w, ws );
    current_target = none;
    if (!render_window_image( &draw, &ws->snapshot )) return false;
    ws->snapshot_rect.left = w->left;
    ws->snapshot_rect.top = w->top;
    ws->snapshot_rect.right = w->right;
    ws->snapshot_rect.bottom = w->bottom;
    ws->snapshot_valid = true;
    return true;
}

static bool blur_level( int level, int width, int height )
{
    if (!blur[level].texture)
    {
        glGenTextures( 1, &blur[level].texture );
        if (level) glGenFramebuffers( 1, &blur[level].framebuffer );
    }
    glBindTexture( GL_TEXTURE_2D, blur[level].texture );
    if (blur[level].width == width && blur[level].height == height) return true;
    /* the first level is copied from a monitor's buffer, which has no alpha */
    glTexImage2D( GL_TEXTURE_2D, 0, level ? GL_RGBA : GL_RGB, width, height, 0, level ? GL_RGBA : GL_RGB,
                  GL_UNSIGNED_BYTE, NULL );
    texture_parameters( GL_TEXTURE_2D, GL_LINEAR );
    blur[level].width = width;
    blur[level].height = height;
    if (!level) return true;
    glBindFramebuffer( GL_FRAMEBUFFER, blur[level].framebuffer );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur[level].texture, 0 );
    return glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
}

/* one pass of the blur, from one level into another */
static void blur_pass( const struct program *p, int from, int to )
{
    glBindFramebuffer( GL_FRAMEBUFFER, blur[to].framebuffer );
    glViewport( 0, 0, blur[to].width, blur[to].height );
    set_projection( 0, 0, blur[to].width, blur[to].height, true );
    use_program( p );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, blur[from].texture );
    glUniform2f( p->half, 0.5f / blur[from].width, 0.5f / blur[from].height );
    glUniform1f( p->offset, BLUR_OFFSET );
    set_clip( p, NULL, 1 );
    draw_quad( 0, 0, blur[to].width, blur[to].height, 0, 0, 1, 1 );
}

/* what lies behind the window on this monitor, blurred and tinted: acrylic */
static void draw_backdrop( const struct window_draw *draw, int ox, int oy )
{
    struct target saved = current_target;
    const struct frect *r = &draw->shown;
    float tint[4] =
    {
        ((draw->tint >> 16) & 0xff) / 255.0f, ((draw->tint >> 8) & 0xff) / 255.0f, (draw->tint & 0xff) / 255.0f,
        (draw->tint >> 24) / 255.0f
    };
    int x0, y0, x1, y1, width, height, level;

    if (!draw->blur)
    {
        float color[4] = { tint[0] * tint[3], tint[1] * tint[3], tint[2] * tint[3], tint[3] };

        use_program( &prog_color );
        glUniform4fv( prog_color.color, 1, color );
        set_clip( &prog_color, &draw->clip, draw->opacity );
        draw_quad( r->left, r->top, r->right, r->bottom, 0, 0, 1, 1 );
        return;
    }

    /* the part of this monitor's frame behind it, with a margin: output coordinates, top down */
    x0 = max( 0, (int)floorf( r->left ) - ox - BLUR_MARGIN );
    y0 = max( 0, (int)floorf( r->top ) - oy - BLUR_MARGIN );
    x1 = min( saved.width, (int)ceilf( r->right ) - ox + BLUR_MARGIN );
    y1 = min( saved.height, (int)ceilf( r->bottom ) - oy + BLUR_MARGIN );
    if (x1 <= x0 || y1 <= y0) return;
    width = x1 - x0;
    height = y1 - y0;

    glDisable( GL_BLEND );
    if (!blur_level( 0, width, height )) goto done;
    glCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x0, saved.height - y1, width, height );
    for (level = 1; level <= BLUR_LEVELS; level++)
    {
        if (!blur_level( level, max( 1, width >> level ), max( 1, height >> level ) )) goto done;
        blur_pass( &prog_down, level - 1, level );
    }
    for (level = BLUR_LEVELS - 1; level >= 1; level--) blur_pass( &prog_up, level + 1, level );

    set_target( &saved );
    glEnable( GL_BLEND );
    use_program( &prog_acrylic );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, blur[1].texture );
    glUniform4fv( prog_acrylic.tint, 1, tint );
    glUniform1f( prog_acrylic.noise, draw->ws && draw->ws->attr.accent == ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND ?
                                     0.02f : 0.0f );
    set_clip( &prog_acrylic, &draw->clip, draw->opacity );
    /* the copy's first row is the bottom of the region */
    draw_quad( r->left, r->top, r->right, r->bottom,
               (r->left - ox - x0) / width, 1 - (r->top - oy - y0) / height,
               (r->right - ox - x0) / width, 1 - (r->bottom - oy - y0) / height );
    return;

done:
    set_target( &saved );
    glEnable( GL_BLEND );
}

static void draw_thumbnail( const struct thumbnail_draw *t )
{
    struct window_state *ws = t->source_state;
    struct window_draw draw;
    const struct window_image *image;

    if (!ws) return;
    if (t->from_snapshot) image = &ws->snapshot;
    else
    {
        scene_static_draw( &draw, t->source, ws );
        if (!render_window_image( &draw, &ws->scratch )) return;
        image = &ws->scratch;
    }
    draw_image( image, &t->dest, &t->source_rect, NULL, t->opacity );
}

static void draw_window( const struct window_draw *draw, int ox, int oy )
{
    struct window_state *ws = draw->ws;
    struct thumbnail_draw thumbs[32];
    uint32_t count;

    if (draw->backdrop) draw_backdrop( draw, ox, oy );

    if (draw->from_snapshot)
    {
        if (ws && ws->snapshot_valid) draw_image( &ws->snapshot, &draw->shown, NULL, &draw->clip, draw->opacity );
    }
    else if (draw->as_image && ws && render_window_image( draw, &ws->scratch ))
    {
        draw_image( &ws->scratch, &draw->shown, NULL, &draw->clip, draw->opacity );
    }
    else
    {
        struct layer_ctx ctx = { draw, &draw->xform, draw->clip.radius > 0 ? &draw->clip : NULL, draw->opacity };
        scene_surfaces( draw, draw_tree_callback, &ctx );
    }

    count = scene_thumbnails( draw, thumbs, ARRAY_SIZE(thumbs) );
    for (uint32_t i = 0; i < count; i++) draw_thumbnail( &thumbs[i] );
}

static void draw_cursor( void )
{
    struct clip none = { { 0 }, -1 };

    if (cursor_hidden) return;
    if (!cursor_texture)
    {
        uint32_t rgba[20 * 12] = {0};

        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 12 && arrow[y][x]; x++)
                if (arrow[y][x] != ' ') rgba[y * 12 + x] = arrow[y][x] == 'B' ? 0xff000000 : 0xffffffff;
        glGenTextures( 1, &cursor_texture );
        glBindTexture( GL_TEXTURE_2D, cursor_texture );
        glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 12, 20, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
        texture_parameters( GL_TEXTURE_2D, GL_NEAREST );
    }
    use_program( &prog_texture );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, cursor_texture );
    glUniform1f( prog_texture.mode, LAYER_PREMULTIPLIED );
    glUniform1f( prog_texture.swizzle, 0 );
    set_clip( &prog_texture, &none, 1 );
    glUniform1f( prog_texture.radius, -1 );
    draw_quad( cursor_x, cursor_y, cursor_x + 12, cursor_y + 20, 0, 0, 1, 1 );
}

static void draw_unowned( struct surface *s, int x, int y, void *ctx )
{
    draw_tree( s, x, y, ctx );
}

static enum layer_mode plain_mode( const struct surface *s, const struct window_draw *draw )
{
    return s->alpha ? LAYER_PREMULTIPLIED : LAYER_OPAQUE;
}

static void draw_scene( int ox, int oy, int width, int height )
{
    static struct window_draw draws[512];
    struct target target = { 0, width, height };
    struct window_draw plain;
    struct xform identity = { 1, 1, 0, 0 };
    struct layer_ctx ctx = { &plain, &identity, NULL, 1 };
    uint32_t count;

    set_projection( ox, oy, width, height, false );
    memcpy( target.proj, proj, sizeof(proj) );
    set_target( &target );
    glClearColor( ((background >> 16) & 0xff) / 255.0f, ((background >> 8) & 0xff) / 255.0f,
                  (background & 0xff) / 255.0f, 1 );
    glClear( GL_COLOR_BUFFER_BIT );
    glEnable( GL_BLEND );
    glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

    count = scene_windows( draws, ARRAY_SIZE(draws) );
    for (uint32_t i = 0; i < count; i++)
    {
        const struct window_draw *draw = &draws[i];

        if (draw->shown.right <= ox || draw->shown.bottom <= oy || draw->shown.left >= ox + width ||
            draw->shown.top >= oy + height)
            continue;  /* on another monitor */
        draw_window( draw, ox, oy );
    }

    memset( &plain, 0, sizeof(plain) );
    plain.mode = plain_mode;
    scene_unowned_surfaces( draw_unowned, &ctx );
    draw_cursor();
}

/**********************************************************************
 *          Monitors
 */

static void free_output( struct gl_output *out )
{
    struct gl_output **p;

    if (out->locked) return;  /* once KMS lets its last frame go */
    for (p = &gl_outputs; *p; p = &(*p)->next) if (*p == out) { *p = out->next; break; }
    if (out->egl != EGL_NO_SURFACE && display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, context );
        eglDestroySurface( display, out->egl );
    }
    if (out->surface) gbm_surface_destroy( out->surface );
    free( out );
}

static void frame_destroyed( struct gbm_bo *bo, void *data )
{
    struct gl_frame *frame = data;

    kms_remove_fb( frame->fb );
    free( frame );
}

static struct gl_frame *frame_for_bo( struct gl_output *out, struct gbm_bo *bo )
{
    struct gl_frame *frame = gbm_bo_get_user_data( bo );
    uint32_t handles[4], pitches[4], offsets[4];
    int planes;

    if (frame) return frame;
    if (!(frame = calloc( 1, sizeof(*frame) ))) return NULL;
    frame->out = out;
    frame->bo = bo;
    planes = gbm_bo_get_plane_count( bo );
    for (int i = 0; i < planes && i < 4; i++)
    {
        handles[i] = gbm_bo_get_handle_for_plane( bo, i ).u32;
        pitches[i] = gbm_bo_get_stride_for_plane( bo, i );
        offsets[i] = gbm_bo_get_offset( bo, i );
    }
    frame->fb = kms_add_fb( gbm_bo_get_width( bo ), gbm_bo_get_height( bo ), gbm_bo_get_format( bo ), planes,
                            handles, pitches, offsets, gbm_bo_get_modifier( bo ) );
    gbm_bo_set_user_data( bo, frame, frame_destroyed );
    return frame;
}

static struct gl_output *output_surface( struct output *output, int width, int height )
{
    struct gl_output *out = output->gl;

    if (out && (out->width != width || out->height != height))
    {
        /* a new mode: a surface of the new size once the old frames are off screen */
        if (out->locked) return NULL;
        out->owner = NULL;
        output->gl = NULL;
        free_output( out );
        out = NULL;
    }
    if (out) return out;

    if (!(out = calloc( 1, sizeof(*out) ))) return NULL;
    out->owner = output;
    out->width = width;
    out->height = height;
    out->egl = EGL_NO_SURFACE;
    out->surface = gbm_surface_create( gbm, width, height, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING );
    if (out->surface)
        out->egl = create_window_surface ? create_window_surface( display, config, out->surface, NULL )
                                         : eglCreateWindowSurface( display, config, (EGLNativeWindowType)out->surface, NULL );
    out->next = gl_outputs;
    gl_outputs = out;
    if (!out->surface || out->egl == EGL_NO_SURFACE)
    {
        ERR( "no surface for a %dx%d monitor: %#x\n", width, height, eglGetError() );
        out->owner = NULL;
        free_output( out );
        return NULL;
    }
    output->gl = out;
    return out;
}

void gl_output_gone( struct output *output )
{
    struct gl_output *out = output->gl;

    if (!out) return;
    output->gl = NULL;
    out->owner = NULL;
    free_output( out );
}

static void hold_frame( struct gl_frame *frame )
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

static void release_frame( struct gl_frame *frame )
{
    struct gl_output *out = frame->out;

    frame->held = false;
    out->locked--;
    gbm_surface_release_buffer( out->surface, frame->bo );
    if (!out->owner) free_output( out );
}

bool gl_frame_released( void *buffer )
{
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        if (!buffer || held_frames[i] != buffer) continue;
        held_frames[i] = NULL;
        release_frame( buffer );
        return true;
    }
    return false;
}

bool gl_render( struct output *output, bool vrr )
{
    struct kms_output *o = output->kms;
    int width, height;
    struct gl_output *out;
    struct gl_frame *frame;
    struct gbm_bo *bo;

    if (!gl_active() || !o) return false;
    width = o->modes[o->mode].info.hdisplay;
    height = o->modes[o->mode].info.vdisplay;
    if (!(out = output_surface( output, width, height ))) return false;
    if (!gbm_surface_has_free_buffers( out->surface )) return false;
    if (!eglMakeCurrent( display, out->egl, out->egl, context ))
    {
        ERR( "cannot draw on %s: %#x\n", o->name, eglGetError() );
        return false;
    }

    draw_scene( o->x, o->y, width, height );
    if (!eglSwapBuffers( display, out->egl ))
    {
        ERR( "cannot finish a frame of %s: %#x\n", o->name, eglGetError() );
        return false;
    }
    if (!(bo = gbm_surface_lock_front_buffer( out->surface ))) return false;
    if (!(frame = frame_for_bo( out, bo )) || !frame->fb)
    {
        gbm_surface_release_buffer( out->surface, bo );
        give_up( "the card cannot show buffers drawn by its GPU" );
        return false;
    }
    hold_frame( frame );
    if (!frame->held)
    {
        gbm_surface_release_buffer( out->surface, bo );
        return false;
    }

    /* the pointer is drawn into the frame */
    kms_show_cursor( o, false, 0, 0 );
    if (kms_present( o, frame->fb, width, height, vrr, frame, true )) return true;
    gl_frame_released( frame );
    if (o->no_flip) give_up( "the driver cannot flip" );
    return false;
}

/**********************************************************************
 *          Reading GPU buffers for the CPU
 *
 * When the CPU composes, a window that draws with the GPU hands over a
 * buffer in video memory that the CPU often cannot map at all (amdgpu
 * refuses buffers made without CPU access). It is copied through the GPU.
 */
bool gpu_read_dmabuf( struct dmabuf *buffer, uint32_t *pixels )
{
    EGLImageKHR image;
    bool ok;

    if (!make_current()) return false;
    if (!has_extension( (const char *)glGetString( GL_EXTENSIONS ), "GL_EXT_read_format_bgra" )) return false;
    if (!(image = import_dmabuf( buffer )))
    {
        static unsigned int logged;
        if (logged++ < 8) ERR( "cannot import a %ux%u buffer: %#x\n", buffer->width, buffer->height, eglGetError() );
        return false;
    }

    glBindTexture( GL_TEXTURE_2D, read_texture );
    image_target_texture( GL_TEXTURE_2D, image );
    glBindFramebuffer( GL_FRAMEBUFFER, read_framebuffer );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, read_texture, 0 );
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
