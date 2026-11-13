/*
 * Arctic WordPad: pictures
 *
 * Read with the Windows Imaging Component (PNG, JPEG, GIF, BMP, TIFF and
 * the rest it knows) into a DIB, over white where they are see-through;
 * written as PNG for a DOCX.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <initguid.h>
#include <wincodec.h>
#include "wordpad.h"

#define MAX_SIDE 2400   /* pixels kept of a picture's longer side */

static IWICImagingFactory *factory(void)
{
    static IWICImagingFactory *f;

    if (!f && FAILED(CoCreateInstance( &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                       (void **)&f )))
        f = NULL;
    return f;
}

/* a BITMAPINFOHEADER and 24-bit rows, bottom up */
BYTE *image_to_dib( const BYTE *data, size_t len, int max_w, int max_h, int *width, int *height, size_t *dib_len )
{
    IWICImagingFactory *wic = factory();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame_dec = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *source;
    UINT w = 0, h = 0;
    BYTE *dib = NULL, *rgba = NULL;

    if (!wic) return NULL;
    if (FAILED(IWICImagingFactory_CreateStream( wic, &stream ))) return NULL;
    if (FAILED(IWICStream_InitializeFromMemory( stream, (BYTE *)data, (DWORD)len ))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream( wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                           &decoder ))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame_dec ))) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter( wic, &converter ))) goto done;
    if (FAILED(IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)frame_dec, &GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom ))) goto done;
    source = (IWICBitmapSource *)converter;
    IWICBitmapSource_GetSize( source, &w, &h );
    if (!w || !h) goto done;
    if (max_w <= 0) max_w = MAX_SIDE;
    if (max_h <= 0) max_h = MAX_SIDE;
    if ((int)w > max_w || (int)h > max_h)
    {
        /* no more pixels than it is shown with */
        double scale = min( (double)max_w / w, (double)max_h / h );
        UINT nw = (UINT)(w * scale + 0.5), nh = (UINT)(h * scale + 0.5);
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler( wic, &scaler )) &&
            SUCCEEDED(IWICBitmapScaler_Initialize( scaler, source, max( nw, 1 ), max( nh, 1 ), WICBitmapInterpolationModeFant )))
        {
            source = (IWICBitmapSource *)scaler;
            w = max( nw, 1 );
            h = max( nh, 1 );
        }
    }
    if (!(rgba = malloc( (size_t)w * h * 4 ))) goto done;
    if (FAILED(IWICBitmapSource_CopyPixels( source, NULL, w * 4, w * h * 4, rgba ))) goto done;
    {
        UINT stride = (w * 3 + 3) & ~3;
        BITMAPINFOHEADER *bih;

        *dib_len = sizeof(BITMAPINFOHEADER) + (size_t)stride * h;
        if (!(dib = calloc( 1, *dib_len ))) goto done;
        bih = (BITMAPINFOHEADER *)dib;
        bih->biSize = sizeof(*bih);
        bih->biWidth = w;
        bih->biHeight = h;
        bih->biPlanes = 1;
        bih->biBitCount = 24;
        bih->biSizeImage = stride * h;
        for (UINT y = 0; y < h; y++)
        {
            const BYTE *src = rgba + (size_t)y * w * 4;
            BYTE *dst = dib + sizeof(*bih) + (size_t)(h - 1 - y) * stride;

            for (UINT x = 0; x < w; x++, src += 4, dst += 3)
            {
                /* over white paper */
                int a = src[3];
                dst[0] = (BYTE)((src[0] * a + 255 * (255 - a)) / 255);
                dst[1] = (BYTE)((src[1] * a + 255 * (255 - a)) / 255);
                dst[2] = (BYTE)((src[2] * a + 255 * (255 - a)) / 255);
            }
        }
        *width = w;
        *height = h;
    }
done:
    free( rgba );
    if (scaler) IWICBitmapScaler_Release( scaler );
    if (converter) IWICFormatConverter_Release( converter );
    if (frame_dec) IWICBitmapFrameDecode_Release( frame_dec );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (stream) IWICStream_Release( stream );
    return dib;
}

/* 32-bit pixels, top down, into a PNG */
static BYTE *encode_png( const BYTE *bgra, int width, int height, size_t *png_len )
{
    IWICImagingFactory *wic = factory();
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame_enc = NULL;
    IStream *stream = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HGLOBAL global;
    BYTE *png = NULL;
    STATSTG stat;

    HRESULT hr;
    int step = 0;

    if (!wic) { trace( "png: no factory\n" ); return NULL; }
    if (FAILED(CreateStreamOnHGlobal( NULL, TRUE, &stream ))) return NULL;
#define TRY(call) do { step++; if (FAILED(hr = (call))) goto done; } while (0)
    TRY( IWICImagingFactory_CreateEncoder( wic, &GUID_ContainerFormatPng, NULL, &encoder ) );
    TRY( IWICBitmapEncoder_Initialize( encoder, stream, WICBitmapEncoderNoCache ) );
    TRY( IWICBitmapEncoder_CreateNewFrame( encoder, &frame_enc, NULL ) );
    TRY( IWICBitmapFrameEncode_Initialize( frame_enc, NULL ) );
    TRY( IWICBitmapFrameEncode_SetSize( frame_enc, width, height ) );
    TRY( IWICBitmapFrameEncode_SetPixelFormat( frame_enc, &format ) );
    TRY( IWICBitmapFrameEncode_WritePixels( frame_enc, height, width * 4, width * height * 4, (BYTE *)bgra ) );
    TRY( IWICBitmapFrameEncode_Commit( frame_enc ) );
    TRY( IWICBitmapEncoder_Commit( encoder ) );
#undef TRY
    step = 0;
    if (SUCCEEDED(IStream_Stat( stream, &stat, STATFLAG_NONAME )) && SUCCEEDED(GetHGlobalFromStream( stream, &global )))
    {
        void *p = GlobalLock( global );
        *png_len = (size_t)stat.cbSize.QuadPart;
        if (p && (png = malloc( *png_len ))) memcpy( png, p, *png_len );
        GlobalUnlock( global );
    }
done:
    if (step) trace( "png: step %d failed %08lx\n", step, hr );
    if (frame_enc) IWICBitmapFrameEncode_Release( frame_enc );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    IStream_Release( stream );
    return png;
}

/* a metafile riched20 kept (EMF, or WMF of mapping mode mm) played at its
 * size in pixels, into a PNG */
BYTE *image_from_metafile( const BYTE *bits, size_t len, BOOL enhanced, int mm, int width, int height, size_t *png_len )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    HENHMETAFILE emf;
    HBITMAP bitmap;
    HDC dc;
    BYTE *pixels, *png = NULL;
    RECT r = { 0, 0, width, height };

    if (width <= 0 || height <= 0 || width > 8000 || height > 8000) return NULL;
    if (enhanced) emf = SetEnhMetaFileBits( (UINT)len, bits );
    else
    {
        METAFILEPICT mfp = { mm ? mm : MM_ANISOTROPIC, width, height, NULL };
        emf = SetWinMetaFileBits( (UINT)len, bits, NULL, &mfp );
    }
    if (!emf) return NULL;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    dc = CreateCompatibleDC( NULL );
    if ((bitmap = CreateDIBSection( dc, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( dc, bitmap );

        FillRect( dc, &r, GetStockObject( WHITE_BRUSH ) );
        SetStretchBltMode( dc, HALFTONE );
        PlayEnhMetaFile( dc, emf, &r );
        GdiFlush();
        for (int i = 0; i < width * height; i++) pixels[i * 4 + 3] = 255;
        png = encode_png( pixels, width, height, png_len );
        SelectObject( dc, old );
        DeleteObject( bitmap );
    }
    DeleteDC( dc );
    DeleteEnhMetaFile( emf );
    return png;
}

/* the size of a metafile's picture in pixels as it was made: an EMF's
 * own bounds, which StretchDIBits of a picture gives at its size */
BOOL metafile_size( const BYTE *bits, size_t len, int *width, int *height )
{
    const ENHMETAHEADER *h = (const ENHMETAHEADER *)bits;

    if (len < sizeof(*h) || h->iType != EMR_HEADER) return FALSE;
    *width = h->rclBounds.right - h->rclBounds.left + 1;
    *height = h->rclBounds.bottom - h->rclBounds.top + 1;
    return *width > 0 && *height > 0;
}
