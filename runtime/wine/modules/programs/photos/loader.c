/*
 * Photos: pictures, their folder and their small copies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "photos.h"
#include <wincodec.h>

static IWICImagingFactory *wic(void)
{
    IWICImagingFactory *factory = NULL;

    CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory );
    return factory;
}

static BOOL query_uint( IWICMetadataQueryReader *reader, const WCHAR *name, UINT *value )
{
    PROPVARIANT v;
    BOOL ok = FALSE;

    PropVariantInit( &v );
    if (reader && SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName( reader, name, &v )))
    {
        switch (v.vt)
        {
        case VT_UI1: *value = v.bVal; ok = TRUE; break;
        case VT_UI2: *value = v.uiVal; ok = TRUE; break;
        case VT_UI4: *value = v.ulVal; ok = TRUE; break;
        case VT_I2: *value = v.iVal; ok = TRUE; break;
        case VT_I4: *value = v.lVal; ok = TRUE; break;
        }
    }
    PropVariantClear( &v );
    return ok;
}

static BOOL query_text( IWICMetadataQueryReader *reader, const WCHAR *name, WCHAR *out, int len )
{
    PROPVARIANT v;
    BOOL ok = FALSE;

    PropVariantInit( &v );
    if (reader && SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName( reader, name, &v )))
    {
        if (v.vt == VT_LPSTR && v.pszVal)
        {
            MultiByteToWideChar( CP_UTF8, 0, v.pszVal, -1, out, len );
            ok = TRUE;
        }
        else if (v.vt == VT_LPWSTR && v.pwszVal)
        {
            lstrcpynW( out, v.pwszVal, len );
            ok = TRUE;
        }
    }
    PropVariantClear( &v );
    if (ok)   /* trailing blanks, as cameras pad their names */
    {
        int n = lstrlenW( out );
        while (n && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    }
    return ok && out[0];
}

static UINT32 *copy_pixels( IWICBitmapSource *frame, UINT *w, UINT *h )
{
    IWICBitmapSource *source = NULL;
    UINT32 *bits = NULL;

    if (FAILED(WICConvertBitmapSource( &GUID_WICPixelFormat32bppBGRA, frame, &source ))) return NULL;
    if (SUCCEEDED(IWICBitmapSource_GetSize( source, w, h )) && *w && *h && (UINT64)*w * *h < 400000000 &&
        (bits = malloc( (size_t)*w * *h * 4 )))
    {
        if (FAILED(IWICBitmapSource_CopyPixels( source, NULL, *w * 4, *w * *h * 4, (BYTE *)bits )))
        {
            free( bits );
            bits = NULL;
        }
    }
    IWICBitmapSource_Release( source );
    return bits;
}

/* EXIF's orientation into the pixels: 6 is a quarter turn to the right */
static UINT32 *orient( UINT32 *bits, int *w, int *h, int orientation )
{
    int sw = *w, sh = *h, nw = sw, nh = sh;
    UINT32 *out;

    if (orientation < 2 || orientation > 8) return bits;
    if (orientation >= 5) { nw = sh; nh = sw; }
    if (!(out = malloc( (size_t)nw * nh * 4 ))) return bits;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
        {
            int nx, ny;
            switch (orientation)
            {
            case 2: nx = sw - 1 - x; ny = y; break;
            case 3: nx = sw - 1 - x; ny = sh - 1 - y; break;
            case 4: nx = x; ny = sh - 1 - y; break;
            case 5: nx = y; ny = x; break;
            case 6: nx = sh - 1 - y; ny = x; break;
            case 7: nx = sh - 1 - y; ny = sw - 1 - x; break;
            default: nx = y; ny = sw - 1 - x; break;
            }
            out[(size_t)ny * nw + nx] = bits[(size_t)y * sw + x];
        }
    free( bits );
    *w = nw;
    *h = nh;
    return out;
}

/* a GIF's frames, each put on what the one before left */
static void load_animation( IWICBitmapDecoder *decoder, struct picture *p, UINT count )
{
    IWICMetadataQueryReader *top = NULL;
    UINT sw = 0, sh = 0;
    UINT32 *canvas, *before = NULL;
    size_t bytes;

    if (SUCCEEDED(IWICBitmapDecoder_GetMetadataQueryReader( decoder, &top )))
    {
        query_uint( top, L"/logscrdesc/Width", &sw );
        query_uint( top, L"/logscrdesc/Height", &sh );
        IWICMetadataQueryReader_Release( top );
    }
    if (!sw || !sh) { sw = p->width; sh = p->height; }
    count = min( count, 1000 );
    bytes = (size_t)sw * sh * 4;
    if (bytes * count > (512u << 20)) count = (UINT)((512u << 20) / bytes);
    if (count < 2 || !(canvas = calloc( 1, bytes ))) return;
    if (!(p->frame_bits = calloc( count, sizeof(UINT32 *) )) || !(p->delays = calloc( count, sizeof(UINT) )))
    {
        free( canvas );
        return;
    }
    for (UINT i = 0; i < count; i++)
    {
        IWICBitmapFrameDecode *frame = NULL;
        IWICMetadataQueryReader *reader = NULL;
        UINT left = 0, topy = 0, delay = 10, disposal = 0, w = 0, h = 0;
        UINT32 *bits;

        if (FAILED(IWICBitmapDecoder_GetFrame( decoder, i, &frame ))) break;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader( frame, &reader )))
        {
            query_uint( reader, L"/imgdesc/Left", &left );
            query_uint( reader, L"/imgdesc/Top", &topy );
            query_uint( reader, L"/grctlext/Delay", &delay );
            query_uint( reader, L"/grctlext/Disposal", &disposal );
            IWICMetadataQueryReader_Release( reader );
        }
        bits = copy_pixels( (IWICBitmapSource *)frame, &w, &h );
        IWICBitmapFrameDecode_Release( frame );
        if (!bits) break;
        if (disposal == 3) before = malloc( bytes ), before && memcpy( before, canvas, bytes );
        for (UINT y = 0; y < h && y + topy < sh; y++)
            for (UINT x = 0; x < w && x + left < sw; x++)
            {
                UINT32 px_ = bits[(size_t)y * w + x];
                if (px_ >> 24) canvas[(size_t)(y + topy) * sw + x + left] = px_;
            }
        free( bits );
        if (!(p->frame_bits[i] = malloc( bytes ))) break;
        memcpy( p->frame_bits[i], canvas, bytes );
        p->delays[i] = delay < 2 ? 100 : delay * 10;
        p->frames = i + 1;
        if (disposal == 2)   /* back to nothing where it was */
        {
            for (UINT y = topy; y < min( sh, topy + h ); y++)
                memset( canvas + (size_t)y * sw + left, 0, (min( sw, left + w ) - min( sw, left )) * 4 );
        }
        else if (disposal == 3 && before)
        {
            memcpy( canvas, before, bytes );
            free( before );
            before = NULL;
        }
    }
    free( before );
    free( canvas );
    if (p->frames >= 2)
    {
        free( p->bits );
        p->bits = p->frame_bits[0];
        p->width = sw;
        p->height = sh;
    }
    else
    {
        for (int i = 0; i < p->frames; i++) free( p->frame_bits[i] );
        free( p->frame_bits );
        free( p->delays );
        p->frame_bits = NULL;
        p->delays = NULL;
        p->frames = 1;
    }
}

struct picture *picture_load( const WCHAR *path )
{
    IWICImagingFactory *factory = wic();
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICMetadataQueryReader *reader = NULL;
    struct picture *p = NULL;
    UINT w = 0, h = 0, count = 1, orientation = 1;
    UINT32 *bits;

    if (!factory) return NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder )))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame ))) goto done;
    if (!(bits = copy_pixels( (IWICBitmapSource *)frame, &w, &h ))) goto done;
    if (!(p = calloc( 1, sizeof(*p) ))) { free( bits ); goto done; }
    p->bits = bits;
    p->width = w;
    p->height = h;
    p->frames = 1;
    IWICBitmapDecoder_GetContainerFormat( decoder, &p->format );
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader( frame, &reader )))
    {
        if (!query_uint( reader, L"/app1/ifd/{ushort=274}", &orientation ))
            query_uint( reader, L"/ifd/{ushort=274}", &orientation );
        if (!query_text( reader, L"/app1/ifd/exif/{ushort=36867}", p->taken, ARRAY_SIZE(p->taken) ))
            query_text( reader, L"/ifd/exif/{ushort=36867}", p->taken, ARRAY_SIZE(p->taken) );
        {
            WCHAR make[64] = { 0 }, model[64] = { 0 };
            if (!query_text( reader, L"/app1/ifd/{ushort=271}", make, ARRAY_SIZE(make) ))
                query_text( reader, L"/ifd/{ushort=271}", make, ARRAY_SIZE(make) );
            if (!query_text( reader, L"/app1/ifd/{ushort=272}", model, ARRAY_SIZE(model) ))
                query_text( reader, L"/ifd/{ushort=272}", model, ARRAY_SIZE(model) );
            /* the model often has the maker's name in it already */
            if (make[0] && model[0] && !StrStrIW( model, make )) swprintf( p->camera, ARRAY_SIZE(p->camera), L"%s %s", make, model );
            else lstrcpynW( p->camera, model[0] ? model : make, ARRAY_SIZE(p->camera) );
        }
        IWICMetadataQueryReader_Release( reader );
    }
    p->orientation = orientation;
    {
        int iw = p->width, ih = p->height;
        p->bits = orient( p->bits, &iw, &ih, orientation );
        p->width = iw;
        p->height = ih;
    }
    if (IsEqualGUID( &p->format, &GUID_ContainerFormatGif ) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrameCount( decoder, &count )) && count > 1)
        load_animation( decoder, p, count );

done:
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    IWICImagingFactory_Release( factory );
    return p;
}

void picture_free( struct picture *p )
{
    if (!p) return;
    if (p->frame_bits)
    {
        for (int i = 0; i < p->frames; i++)
        {
            blit_forget( p->frame_bits[i] );
            free( p->frame_bits[i] );
        }
        free( p->frame_bits );
        free( p->delays );
    }
    else
    {
        blit_forget( p->bits );
        free( p->bits );
    }
    free( p );
}

/* the picture turned a quarter to the right, into its file in its own
 * format: the turn is in the pixels, the camera's flag goes */
BOOL picture_save_rotated( const WCHAR *path, const struct picture *p )
{
    IWICImagingFactory *factory = wic();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    IWICBitmap *bitmap = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    BOOL jpeg = IsEqualGUID( &p->format, &GUID_ContainerFormatJpeg );
    int w = p->height, h = p->width;
    UINT32 *bits;
    WCHAR temp[MAX_PATH + 8];
    HRESULT hr;

    if (!factory) return FALSE;
    if (!(bits = malloc( (size_t)w * h * 4 )))
    {
        IWICImagingFactory_Release( factory );
        return FALSE;
    }
    for (int y = 0; y < p->height; y++)
        for (int x = 0; x < p->width; x++)
            bits[(size_t)x * w + (p->height - 1 - y)] = p->bits[(size_t)y * p->width + x];
    swprintf( temp, ARRAY_SIZE(temp), L"%s.~new", path );
    hr = IWICImagingFactory_CreateBitmapFromMemory( factory, w, h, &GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4,
                                                    (BYTE *)bits, &bitmap );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename( stream, temp, GENERIC_WRITE );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, &p->format, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, (IStream *)stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, &options );
    if (SUCCEEDED(hr) && options && jpeg)
    {
        PROPBAG2 name = { 0 };
        VARIANT value;
        name.pstrName = (WCHAR *)L"ImageQuality";
        V_VT( &value ) = VT_R4;
        V_R4( &value ) = 0.95f;
        IPropertyBag2_Write( options, 1, &name, &value );
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, options );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, w, h );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &pf );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource( frame, (IWICBitmapSource *)bitmap, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (options) IPropertyBag2_Release( options );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IWICStream_Release( stream );
    if (bitmap) IWICBitmap_Release( bitmap );
    IWICImagingFactory_Release( factory );
    free( bits );
    if (SUCCEEDED(hr) && !MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED )) hr = E_FAIL;
    if (FAILED(hr)) DeleteFileW( temp );
    return SUCCEEDED(hr);
}

/**********************************************************************
 *          The folder
 */

BOOL is_picture_name( const WCHAR *name )
{
    static const WCHAR *const exts[] =
    {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", L".tif", L".tiff", L".ico",
        L".webp", L".heic", L".heif", L".jxr", L".wdp", L".dds",
    };
    const WCHAR *ext = wcsrchr( name, '.' );

    if (!ext) return FALSE;
    for (int i = 0; i < ARRAY_SIZE(exts); i++) if (!wcsicmp( ext, exts[i] )) return TRUE;
    return FALSE;
}

static int cmp_names( const void *a, const void *b )
{
    return StrCmpLogicalW( *(WCHAR *const *)a, *(WCHAR *const *)b );
}

void folder_free( struct folder *f )
{
    for (int i = 0; i < f->count; i++) free( f->names[i] );
    free( f->names );
    memset( f, 0, sizeof(*f) );
}

void folder_load( struct folder *f, const WCHAR *path, int *index )
{
    WCHAR pattern[MAX_PATH + 4], *slash;
    const WCHAR *name;
    WIN32_FIND_DATAW data;
    HANDLE find;
    int cap = 0;

    folder_free( f );
    *index = -1;
    lstrcpynW( f->dir, path, MAX_PATH );
    if ((slash = wcsrchr( f->dir, '\\' ))) *slash = 0;
    name = slash ? path + (slash - f->dir) + 1 : path;
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", f->dir );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!is_picture_name( data.cFileName )) continue;
            if (f->count == cap)
            {
                WCHAR **grown = realloc( f->names, (cap = max( 64, cap * 2 )) * sizeof(WCHAR *) );
                if (!grown) break;
                f->names = grown;
            }
            f->names[f->count++] = _wcsdup( data.cFileName );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (f->count) qsort( f->names, f->count, sizeof(WCHAR *), cmp_names );
    for (int i = 0; i < f->count; i++) if (!wcsicmp( f->names[i], name )) *index = i;
    if (*index < 0)
    {
        /* a picture the folder does not list (hidden, another type): alone */
        WCHAR **grown = realloc( f->names, (f->count + 1) * sizeof(WCHAR *) );
        if (!grown) return;
        f->names = grown;
        f->names[f->count] = _wcsdup( name );
        *index = f->count++;
    }
}

/**********************************************************************
 *          Small copies, made one at a time behind the window
 */

#define THUMB_MAX 192

struct thumb
{
    UINT32 *bits;
    int width, height;
    BOOL wanted, failed;
};

static CRITICAL_SECTION cs;
static HANDLE wake;
static HWND notify_to;
static struct thumb *thumbs;
static WCHAR **paths;
static int thumb_count;
static LONG generation;

static UINT32 *make_thumb( IWICImagingFactory *factory, const WCHAR *path, int *tw, int *th_ )
{
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICMetadataQueryReader *reader = NULL;
    UINT w = 0, h = 0, orientation = 1;
    UINT32 *bits = NULL;

    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder )))
        return NULL;
    if (SUCCEEDED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame )) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize( frame, &w, &h )) && w && h)
    {
        double s = min( 1.0, (double)THUMB_MAX / max( w, h ) );
        UINT nw = max( 1, (UINT)(w * s) ), nh = max( 1, (UINT)(h * s) );

        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader( frame, &reader )))
        {
            if (!query_uint( reader, L"/app1/ifd/{ushort=274}", &orientation ))
                query_uint( reader, L"/ifd/{ushort=274}", &orientation );
            IWICMetadataQueryReader_Release( reader );
        }
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler( factory, &scaler )) &&
            SUCCEEDED(IWICBitmapScaler_Initialize( scaler, (IWICBitmapSource *)frame, nw, nh, WICBitmapInterpolationModeFant )))
        {
            UINT cw, ch;
            if ((bits = copy_pixels( (IWICBitmapSource *)scaler, &cw, &ch )))
            {
                int iw = cw, ih = ch;
                bits = orient( bits, &iw, &ih, orientation );
                *tw = iw;
                *th_ = ih;
            }
        }
    }
    if (scaler) IWICBitmapScaler_Release( scaler );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    IWICBitmapDecoder_Release( decoder );
    return bits;
}

static DWORD WINAPI thumb_thread( void *arg )
{
    IWICImagingFactory *factory;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    factory = wic();
    for (;;)
    {
        int index = -1;
        LONG gen;
        WCHAR path[MAX_PATH];

        WaitForSingleObject( wake, INFINITE );
        for (;;)
        {
            UINT32 *bits;
            int w = 0, h = 0;

            EnterCriticalSection( &cs );
            gen = generation;
            index = -1;
            for (int i = 0; i < thumb_count; i++)
                if (thumbs[i].wanted && !thumbs[i].bits && !thumbs[i].failed) { index = i; break; }
            if (index >= 0)
            {
                lstrcpynW( path, paths[index], MAX_PATH );
                thumbs[index].wanted = FALSE;
            }
            LeaveCriticalSection( &cs );
            if (index < 0) break;
            bits = factory ? make_thumb( factory, path, &w, &h ) : NULL;
            EnterCriticalSection( &cs );
            if (gen == generation && index < thumb_count)
            {
                thumbs[index].bits = bits;
                thumbs[index].width = w;
                thumbs[index].height = h;
                thumbs[index].failed = !bits;
                bits = NULL;
            }
            LeaveCriticalSection( &cs );
            free( bits );
            PostMessageW( notify_to, WM_APP_THUMB, index, 0 );
        }
    }
    return 0;
}

void thumbs_start( HWND notify )
{
    notify_to = notify;
    InitializeCriticalSection( &cs );
    wake = CreateEventW( NULL, FALSE, FALSE, NULL );
    CloseHandle( CreateThread( NULL, 0, thumb_thread, NULL, 0, NULL ) );
}

void thumbs_reset( const struct folder *f )
{
    EnterCriticalSection( &cs );
    generation++;
    for (int i = 0; i < thumb_count; i++)
    {
        free( thumbs[i].bits );
        free( paths[i] );
    }
    free( thumbs );
    free( paths );
    thumb_count = 0;
    thumbs = calloc( max( f->count, 1 ), sizeof(*thumbs) );
    paths = calloc( max( f->count, 1 ), sizeof(*paths) );
    if (thumbs && paths)
    {
        for (int i = 0; i < f->count; i++)
        {
            WCHAR full[MAX_PATH * 2];
            swprintf( full, ARRAY_SIZE(full), L"%s\\%s", f->dir, f->names[i] );
            paths[i] = _wcsdup( full );
        }
        thumb_count = f->count;
    }
    LeaveCriticalSection( &cs );
}

void thumbs_want( int index )
{
    BOOL go = FALSE;

    EnterCriticalSection( &cs );
    if (index >= 0 && index < thumb_count && !thumbs[index].bits && !thumbs[index].failed)
    {
        thumbs[index].wanted = TRUE;
        go = TRUE;
    }
    LeaveCriticalSection( &cs );
    if (go) SetEvent( wake );
}

/* the bits stay the caller's to read only while the folder stays */
BOOL thumb_get( int index, UINT32 **bits, int *width, int *height )
{
    BOOL ok = FALSE;

    EnterCriticalSection( &cs );
    *bits = NULL;
    if (index >= 0 && index < thumb_count)
    {
        *bits = thumbs[index].bits;
        *width = thumbs[index].width;
        *height = thumbs[index].height;
        ok = !thumbs[index].failed;
    }
    LeaveCriticalSection( &cs );
    return ok;
}
