/*
 * Paint: the picture, its history, its files and the clipboard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "paint.h"
#include <wincodec.h>

struct image *pic;

struct image *image_create( int width, int height, UINT32 fill_pixel )
{
    struct image *image;

    if (width <= 0 || height <= 0 || (size_t)width * height > 400000000) return NULL;
    if (!(image = calloc( 1, sizeof(*image) ))) return NULL;
    if (!(image->bits = malloc( (size_t)width * height * 4 )))
    {
        free( image );
        return NULL;
    }
    image->width = width;
    image->height = height;
    for (size_t i = 0; i < (size_t)width * height; i++) image->bits[i] = fill_pixel;
    return image;
}

struct image *image_copy( const struct image *src )
{
    struct image *image = image_create( src->width, src->height, 0 );

    if (image) memcpy( image->bits, src->bits, (size_t)src->width * src->height * 4 );
    return image;
}

void image_free( struct image *image )
{
    if (!image) return;
    blit_forget( image->bits );
    free( image->bits );
    free( image );
}

HBITMAP image_dib( const struct image *image, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    void *p;
    HBITMAP bitmap;

    info.bmiHeader.biWidth = image->width;
    info.bmiHeader.biHeight = -image->height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if (!(bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, &p, NULL, 0 ))) return NULL;
    memcpy( p, image->bits, (size_t)image->width * image->height * 4 );
    if (bits) *bits = p;
    return bitmap;
}

/**********************************************************************
 *          Files
 */

static IWICImagingFactory *wic(void)
{
    IWICImagingFactory *factory = NULL;

    CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory );
    return factory;
}

/* straight alpha as GDI leaves it: what has none is opaque */
struct image *image_load( const WCHAR *path, GUID *format )
{
    IWICImagingFactory *factory = wic();
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *source = NULL;
    struct image *image = NULL;
    UINT width = 0, height = 0;
    HRESULT hr;

    if (!factory) return NULL;
    hr = IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder );
    if (SUCCEEDED(hr) && format) IWICBitmapDecoder_GetContainerFormat( decoder, format );
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame( decoder, 0, &frame );
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource( &GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)frame, &source );
    if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize( source, &width, &height );
    if (SUCCEEDED(hr) && (image = image_create( width, height, 0 )))
    {
        hr = IWICBitmapSource_CopyPixels( source, NULL, width * 4, width * height * 4, (BYTE *)image->bits );
        if (FAILED(hr))
        {
            image_free( image );
            image = NULL;
        }
        /* see-through pixels stay see-through: straight alpha, as WIC gives it */
    }
    if (source) IWICBitmapSource_Release( source );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    IWICImagingFactory_Release( factory );
    return image;
}

BOOL image_opaque( const struct image *image )
{
    for (size_t i = 0; i < (size_t)image->width * image->height; i++)
        if ((image->bits[i] >> 24) != 255) return FALSE;
    return TRUE;
}

void image_flatten( struct image *image, UINT32 back )
{
    for (size_t i = 0; i < (size_t)image->width * image->height; i++)
    {
        UINT32 p = image->bits[i], a = p >> 24, out = 0xff000000;
        if (a == 255) continue;
        for (int k = 0; k < 24; k += 8) out |= ((((p >> k) & 0xff) * a + ((back >> k) & 0xff) * (255 - a)) / 255) << k;
        image->bits[i] = out;
    }
}

/* PNG and TIFF keep what is see-through; JPEG, BMP and GIF get it on white */
BOOL image_save( const struct image *image, const WCHAR *path, const GUID *format )
{
    IWICImagingFactory *factory = wic();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    BOOL indexed = IsEqualGUID( format, &GUID_ContainerFormatGif );
    BOOL alpha = (IsEqualGUID( format, &GUID_ContainerFormatPng ) || IsEqualGUID( format, &GUID_ContainerFormatTiff )) &&
                 !image_opaque( image );
    WICPixelFormatGUID pf = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    const WICPixelFormatGUID *src_pf = alpha ? &GUID_WICPixelFormat32bppBGRA : &GUID_WICPixelFormat24bppBGR;
    UINT stride = alpha ? image->width * 4 : (image->width * 3 + 3) & ~3;
    BYTE *rgb;
    WCHAR temp[MAX_PATH + 8];
    HRESULT hr;

    if (!factory) return FALSE;
    if (!(rgb = malloc( (size_t)stride * image->height )))
    {
        IWICImagingFactory_Release( factory );
        return FALSE;
    }
    for (int y = 0; y < image->height; y++)
        for (int x = 0; x < image->width; x++)
        {
            UINT32 p = image->bits[(size_t)y * image->width + x], a = p >> 24;
            if (alpha)
            {
                memcpy( rgb + (size_t)y * stride + x * 4, &p, 4 );
                continue;
            }
            BYTE *out = rgb + (size_t)y * stride + x * 3;
            out[0] = ((p & 0xff) * a + 255 * (255 - a)) / 255;
            out[1] = (((p >> 8) & 0xff) * a + 255 * (255 - a)) / 255;
            out[2] = (((p >> 16) & 0xff) * a + 255 * (255 - a)) / 255;
        }

    /* written beside, then in place: a failure leaves the file as it was */
    swprintf( temp, ARRAY_SIZE(temp), L"%s.~new", path );
    hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename( stream, temp, GENERIC_WRITE );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, format, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, (IStream *)stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, &options );
    if (SUCCEEDED(hr) && options && IsEqualGUID( format, &GUID_ContainerFormatJpeg ))
    {
        PROPBAG2 name = { 0 };
        VARIANT value;

        name.pstrName = (WCHAR *)L"ImageQuality";
        V_VT( &value ) = VT_R4;
        V_R4( &value ) = 0.95f;
        IPropertyBag2_Write( options, 1, &name, &value );
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, options );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, image->width, image->height );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &pf );
    if (SUCCEEDED(hr) && indexed && !IsEqualGUID( &pf, src_pf ))
    {
        /* the GIF encoder wants a palette: one made from the picture */
        IWICBitmap *bitmap = NULL;
        IWICPalette *palette = NULL;
        IWICFormatConverter *conv = NULL;

        hr = IWICImagingFactory_CreateBitmapFromMemory( factory, image->width, image->height,
                                                        src_pf, stride,
                                                        stride * image->height, rgb, &bitmap );
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreatePalette( factory, &palette );
        if (SUCCEEDED(hr)) hr = IWICPalette_InitializeFromBitmap( palette, (IWICBitmapSource *)bitmap, 256, FALSE );
        if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter( factory, &conv );
        if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize( conv, (IWICBitmapSource *)bitmap, &pf,
                                                                 WICBitmapDitherTypeNone, palette, 0,
                                                                 WICBitmapPaletteTypeCustom );
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPalette( frame, palette );
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource( frame, (IWICBitmapSource *)conv, NULL );
        if (conv) IWICFormatConverter_Release( conv );
        if (palette) IWICPalette_Release( palette );
        if (bitmap) IWICBitmap_Release( bitmap );
    }
    else if (SUCCEEDED(hr))
    {
        if (!IsEqualGUID( &pf, src_pf ))
        {
            /* the encoder wants another format: converted by WIC */
            IWICBitmap *bitmap = NULL;
            hr = IWICImagingFactory_CreateBitmapFromMemory( factory, image->width, image->height,
                                                            src_pf, stride,
                                                            stride * image->height, rgb, &bitmap );
            if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource( frame, (IWICBitmapSource *)bitmap, NULL );
            if (bitmap) IWICBitmap_Release( bitmap );
        }
        else hr = IWICBitmapFrameEncode_WritePixels( frame, image->height, stride, stride * image->height, rgb );
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (options) IPropertyBag2_Release( options );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IWICStream_Release( stream );
    IWICImagingFactory_Release( factory );
    free( rgb );
    if (SUCCEEDED(hr) && !MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED ))
        hr = HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(hr)) DeleteFileW( temp );
    return SUCCEEDED(hr);
}

/**********************************************************************
 *          The clipboard
 */

/* the picture as PNG in memory, see-through kept, for other programs */
static HGLOBAL png_global( const struct image *image )
{
    IWICImagingFactory *factory = wic();
    IStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    HGLOBAL ret = 0, data;
    STATSTG stat;
    HRESULT hr;

    if (!factory) return 0;
    hr = CreateStreamOnHGlobal( NULL, TRUE, &stream );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, &GUID_ContainerFormatPng, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, image->width, image->height );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &pf );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels( frame, image->height, image->width * 4,
                                                                image->width * 4 * image->height, (BYTE *)image->bits );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );
    if (SUCCEEDED(hr) && SUCCEEDED(IStream_Stat( stream, &stat, STATFLAG_NONAME )) &&
        SUCCEEDED(GetHGlobalFromStream( stream, &data )) && (ret = GlobalAlloc( GMEM_MOVEABLE, stat.cbSize.LowPart )))
    {
        memcpy( GlobalLock( ret ), GlobalLock( data ), stat.cbSize.LowPart );
        GlobalUnlock( data );
        GlobalUnlock( ret );
    }
    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IStream_Release( stream );
    IWICImagingFactory_Release( factory );
    return ret;
}

BOOL clipboard_put( HWND owner, const struct image *image )
{
    SIZE_T row = (SIZE_T)image->width * 4;
    BITMAPINFOHEADER *header;
    HGLOBAL dib, png = png_global( image );
    BOOL opened = FALSE;

    if (!(dib = GlobalAlloc( GMEM_MOVEABLE, sizeof(*header) + row * image->height ))) return FALSE;
    header = GlobalLock( dib );
    memset( header, 0, sizeof(*header) );
    header->biSize = sizeof(*header);
    header->biWidth = image->width;
    header->biHeight = image->height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = row * image->height;
    /* a DIB has no transparency for most programs: it goes on white */
    for (int y = 0; y < image->height; y++)
    {
        const UINT32 *src = image->bits + (SIZE_T)y * image->width;
        UINT32 *dst = (UINT32 *)((BYTE *)(header + 1) + row * (image->height - 1 - y));
        for (int x = 0; x < image->width; x++)
        {
            UINT32 p = src[x], a = p >> 24, out = 0xff000000;
            for (int k = 0; k < 24; k += 8) out |= ((((p >> k) & 0xff) * a + 255 * (255 - a)) / 255) << k;
            dst[x] = out;
        }
    }
    GlobalUnlock( dib );
    for (int i = 0; i < 10 && !(opened = OpenClipboard( owner )); i++) Sleep( 50 );
    if (!opened)
    {
        GlobalFree( dib );
        if (png) GlobalFree( png );
        return FALSE;
    }
    EmptyClipboard();
    if (!SetClipboardData( CF_DIB, dib )) GlobalFree( dib );
    if (png && !SetClipboardData( RegisterClipboardFormatW( L"PNG" ), png )) GlobalFree( png );
    CloseClipboard();
    return TRUE;
}

BOOL clipboard_has_image(void)
{
    return IsClipboardFormatAvailable( CF_DIB ) || IsClipboardFormatAvailable( CF_BITMAP ) ||
           IsClipboardFormatAvailable( RegisterClipboardFormatW( L"PNG" ) );
}

static struct image *from_png( HGLOBAL data )
{
    IWICImagingFactory *factory = wic();
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *source = NULL;
    struct image *image = NULL;
    UINT w = 0, h = 0;
    SIZE_T len = GlobalSize( data );
    HGLOBAL copy;

    if (!factory) return NULL;
    if ((copy = GlobalAlloc( GMEM_MOVEABLE, len )))
    {
        memcpy( GlobalLock( copy ), GlobalLock( data ), len );
        GlobalUnlock( data );
        GlobalUnlock( copy );
        if (FAILED(CreateStreamOnHGlobal( copy, TRUE, &stream ))) GlobalFree( copy );
    }
    if (stream &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream( factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder )) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame )) &&
        SUCCEEDED(WICConvertBitmapSource( &GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)frame, &source )) &&
        SUCCEEDED(IWICBitmapSource_GetSize( source, &w, &h )) && (image = image_create( w, h, 0 )))
    {
        if (FAILED(IWICBitmapSource_CopyPixels( source, NULL, w * 4, w * h * 4, (BYTE *)image->bits )))
        {
            image_free( image );
            image = NULL;
        }
    }
    if (source) IWICBitmapSource_Release( source );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (stream) IStream_Release( stream );
    IWICImagingFactory_Release( factory );
    return image;
}

static struct image *from_bitmap( HBITMAP bitmap )
{
    BITMAP bm;
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    struct image *image;
    HDC hdc;

    if (!GetObjectW( bitmap, sizeof(bm), &bm ) || !(image = image_create( bm.bmWidth, bm.bmHeight, 0xffffffff )))
        return NULL;
    info.bmiHeader.biWidth = bm.bmWidth;
    info.bmiHeader.biHeight = -bm.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    hdc = GetDC( 0 );
    GetDIBits( hdc, bitmap, 0, bm.bmHeight, image->bits, &info, DIB_RGB_COLORS );
    ReleaseDC( 0, hdc );
    return image;
}

struct image *clipboard_get( HWND owner )
{
    struct image *image = NULL;
    UINT png = RegisterClipboardFormatW( L"PNG" );
    HANDLE data;

    if (!OpenClipboard( owner )) return NULL;
    if ((data = GetClipboardData( png ))) image = from_png( data );   /* see-through kept */
    if (!image && (data = GetClipboardData( CF_BITMAP )) && (image = from_bitmap( data )))
    {
        /* a bitmap's fourth byte is no alpha */
        for (size_t i = 0; i < (size_t)image->width * image->height; i++) image->bits[i] |= 0xff000000;
    }
    CloseClipboard();
    return image;
}

/**********************************************************************
 *          The history: the whole picture before each change
 */

#define HISTORY_MAX 40
#define HISTORY_BYTES (768u << 20)

static struct image *undo_stack[HISTORY_MAX], *redo_stack[HISTORY_MAX];
static int undo_count, redo_count;

static size_t stack_bytes(void)
{
    size_t total = 0;
    for (int i = 0; i < undo_count; i++) total += (size_t)undo_stack[i]->width * undo_stack[i]->height * 4;
    for (int i = 0; i < redo_count; i++) total += (size_t)redo_stack[i]->width * redo_stack[i]->height * 4;
    return total;
}

static void drop_oldest(void)
{
    if (!undo_count) return;
    image_free( undo_stack[0] );
    memmove( undo_stack, undo_stack + 1, --undo_count * sizeof(undo_stack[0]) );
}

void history_push(void)
{
    struct image *copy;

    if (!pic || !(copy = image_copy( pic ))) return;
    for (int i = 0; i < redo_count; i++) image_free( redo_stack[i] );
    redo_count = 0;
    if (undo_count == HISTORY_MAX) drop_oldest();
    undo_stack[undo_count++] = copy;
    while (undo_count > 1 && stack_bytes() > HISTORY_BYTES) drop_oldest();
    set_modified( TRUE );
}

void history_clear(void)
{
    for (int i = 0; i < undo_count; i++) image_free( undo_stack[i] );
    for (int i = 0; i < redo_count; i++) image_free( redo_stack[i] );
    undo_count = redo_count = 0;
}

BOOL can_undo(void) { return undo_count > 0; }
BOOL can_redo(void) { return redo_count > 0; }

void set_picture( struct image *image )
{
    image_free( pic );
    pic = image;
    canvas_picture_changed();
}

void undo(void)
{
    canvas_cancel();
    if (!undo_count) return;
    if (redo_count == HISTORY_MAX)
    {
        image_free( redo_stack[0] );
        memmove( redo_stack, redo_stack + 1, --redo_count * sizeof(redo_stack[0]) );
    }
    redo_stack[redo_count++] = pic;
    pic = NULL;
    set_picture( undo_stack[--undo_count] );
    set_modified( TRUE );
}

void redo(void)
{
    canvas_cancel();
    if (!redo_count) return;
    if (undo_count == HISTORY_MAX) drop_oldest();
    undo_stack[undo_count++] = pic;
    pic = NULL;
    set_picture( redo_stack[--redo_count] );
    set_modified( TRUE );
}
