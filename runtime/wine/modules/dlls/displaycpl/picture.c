/*
 * Display of the Control Panel: pictures in small
 *
 * A picture file cut to the shape asked for and made that small, as Windows
 * 11's thumbnails of the wallpapers are: read with WIC in a thread of its
 * own, so that the page comes at once and each picture after it, and kept
 * while Explorer runs (the same file at the same size is not read twice).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "displaycpl.h"
#include "wincodec.h"

HRESULT WINAPI WICCreateImagingFactory_Proxy( UINT version, IWICImagingFactory **factory );

/* GUID_WICPixelFormat32bppBGR */
static const GUID format_bgr = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e } };

struct cached
{
    WCHAR path[MAX_PATH];
    int width, height;
    FILETIME written;
    HBITMAP bitmap;
};

static struct cached cache[32];
static UINT cache_next;
static SRWLOCK cache_lock = SRWLOCK_INIT;

static BOOL written_time( const WCHAR *path, FILETIME *written )
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &data )) return FALSE;
    *written = data.ftLastWriteTime;
    return TRUE;
}

static HBITMAP cache_find( const WCHAR *path, int width, int height, const FILETIME *written )
{
    HBITMAP found = NULL;

    AcquireSRWLockShared( &cache_lock );
    for (UINT i = 0; i < ARRAY_SIZE(cache) && !found; i++)
        if (cache[i].bitmap && cache[i].width == width && cache[i].height == height &&
            !CompareFileTime( &cache[i].written, written ) && !wcsicmp( cache[i].path, path ))
            found = cache[i].bitmap;
    ReleaseSRWLockShared( &cache_lock );
    return found;
}

/* kept from then on; the oldest goes when the cache is full (a page shows
 * far fewer than it keeps) */
static HBITMAP cache_add( const WCHAR *path, int width, int height, const FILETIME *written, HBITMAP bitmap )
{
    HBITMAP found;
    struct cached *slot;
    HBITMAP old;

    AcquireSRWLockExclusive( &cache_lock );
    for (UINT i = 0; i < ARRAY_SIZE(cache); i++)
        if (cache[i].bitmap && cache[i].width == width && cache[i].height == height &&
            !CompareFileTime( &cache[i].written, written ) && !wcsicmp( cache[i].path, path ))
        {
            /* another thread read it as well */
            found = cache[i].bitmap;
            ReleaseSRWLockExclusive( &cache_lock );
            DeleteObject( bitmap );
            return found;
        }
    slot = &cache[cache_next++ % ARRAY_SIZE(cache)];
    old = slot->bitmap;
    lstrcpynW( slot->path, path, ARRAY_SIZE(slot->path) );
    slot->width = width;
    slot->height = height;
    slot->written = *written;
    slot->bitmap = bitmap;
    ReleaseSRWLockExclusive( &cache_lock );
    if (old) DeleteObject( old );
    return bitmap;
}

static HBITMAP read_picture( const WCHAR *path, int width, int height )
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapClipper *clipper = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    HBITMAP bitmap = NULL;
    UINT w, h;
    WICRect part;
    void *bits;

    if (FAILED(WICCreateImagingFactory_Proxy( WINCODEC_SDK_VERSION, &factory ))) return NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder )) ||
        FAILED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame )) ||
        FAILED(IWICBitmapFrameDecode_GetSize( frame, &w, &h )) || !w || !h)
        goto done;

    /* the middle of it, in the shape asked for: Fill, as the desktop has it */
    part.X = part.Y = 0;
    part.Width = w;
    part.Height = h;
    if ((ULONGLONG)w * height > (ULONGLONG)h * width)
    {
        part.Width = max( 1, (UINT)((ULONGLONG)h * width / height) );
        part.X = (w - part.Width) / 2;
    }
    else
    {
        part.Height = max( 1, (UINT)((ULONGLONG)w * height / width) );
        part.Y = (h - part.Height) / 2;
    }
    if (FAILED(IWICImagingFactory_CreateBitmapClipper( factory, &clipper )) ||
        FAILED(IWICBitmapClipper_Initialize( clipper, (IWICBitmapSource *)frame, &part )) ||
        FAILED(IWICImagingFactory_CreateBitmapScaler( factory, &scaler )) ||
        FAILED(IWICBitmapScaler_Initialize( scaler, (IWICBitmapSource *)clipper, width, height,
                                            WICBitmapInterpolationModeFant )) ||
        FAILED(IWICImagingFactory_CreateFormatConverter( factory, &converter )) ||
        FAILED(IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)scaler, &format_bgr,
                                               WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom )))
        goto done;

    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (!(bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0 ))) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels( converter, NULL, width * 4, width * height * 4, bits )))
    {
        DeleteObject( bitmap );
        bitmap = NULL;
    }

done:
    if (converter) IWICFormatConverter_Release( converter );
    if (scaler) IWICBitmapScaler_Release( scaler );
    if (clipper) IWICBitmapClipper_Release( clipper );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    IWICImagingFactory_Release( factory );
    return bitmap;
}

HBITMAP picture_get( const WCHAR *path, int width, int height )
{
    FILETIME written;
    HBITMAP bitmap;

    if (!path || !path[0] || width <= 0 || height <= 0 || !written_time( path, &written )) return NULL;
    if ((bitmap = cache_find( path, width, height, &written ))) return bitmap;
    if (!(bitmap = read_picture( path, width, height ))) return NULL;
    return cache_add( path, width, height, &written, bitmap );
}

/* only what was read before: for a drawing that cannot wait */
HBITMAP picture_peek( const WCHAR *path, int width, int height )
{
    FILETIME written;

    if (!path || !path[0] || !written_time( path, &written )) return NULL;
    return cache_find( path, width, height, &written );
}

/* the size of the picture itself: its header only */
BOOL picture_size( const WCHAR *path, UINT *width, UINT *height )
{
    IWICImagingFactory *factory;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    BOOL ret = FALSE;

    *width = *height = 0;
    if (!path || !path[0] || FAILED(WICCreateImagingFactory_Proxy( WINCODEC_SDK_VERSION, &factory ))) return FALSE;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &decoder )) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame )) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize( frame, width, height )))
        ret = *width && *height;
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    IWICImagingFactory_Release( factory );
    return ret;
}

struct job
{
    HWND tile;
    BOOL set;
    WCHAR path[MAX_PATH];
    int width, height;
};

static DWORD WINAPI picture_thread( void *arg )
{
    struct job *job = arg;
    HRESULT init = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    HBITMAP bitmap = picture_get( job->path, job->width, job->height );

    /* the tile may be gone: the bitmap stays in the cache all the same */
    if (bitmap && IsWindow( job->tile ))
    {
        if (job->set) PostMessageW( job->tile, TM_SETBITMAP, (WPARAM)bitmap, 0 );
        else InvalidateRect( job->tile, NULL, FALSE );
    }
    if (SUCCEEDED(init)) CoUninitialize();
    free( job );
    return 0;
}

/* the picture on the tile when it is read (set), or the tile drawn again
 * then: its drawing takes it with picture_peek */
void picture_to_tile( HWND tile, const WCHAR *path, int width, int height, BOOL set )
{
    FILETIME written;
    HBITMAP bitmap;
    struct job *job;
    HANDLE thread;

    if (!tile || !path || !path[0] || width <= 0 || height <= 0 || !written_time( path, &written )) return;
    if ((bitmap = cache_find( path, width, height, &written )))
    {
        if (set) tile_set_bitmap( tile, bitmap );
        else InvalidateRect( tile, NULL, FALSE );
        return;
    }
    if (!(job = calloc( 1, sizeof(*job) ))) return;
    job->tile = tile;
    job->set = set;
    lstrcpynW( job->path, path, ARRAY_SIZE(job->path) );
    job->width = width;
    job->height = height;
    if ((thread = CreateThread( NULL, 0, picture_thread, job, 0, NULL ))) CloseHandle( thread );
    else free( job );
}
