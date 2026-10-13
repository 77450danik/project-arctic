/*
 * Arctic web engine: images
 *
 * Decoded by WIC, which every Windows has (in Arctic it is Wine's
 * windowscodecs: PNG, JPEG, GIF, BMP, ICO, TIFF), into premultiplied BGRA in
 * a DIB section, so the rasterizer draws them with AlphaBlend. WebP and AVIF
 * come with the decoders of stage 6, inside the engine.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>

#include "arctichtml_private.h"

#include <wincodec.h>

/* the engine's own copies: no import library has to carry them */
static const GUID factory_clsid = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID factory_iid = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID format_pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

#define MAX_IMAGE_SIDE 16384

/* A factory of this thread: documents are loaded on threads of their own,
 * and one factory made on the thread of the first would be used from
 * apartments it does not belong to. The thread may have COM already, in
 * whatever apartment: that is fine for WIC, which works in both. */
static IWICImagingFactory *get_factory(void)
{
    IWICImagingFactory *factory = NULL;
    HRESULT hr;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    hr = CoCreateInstance( &factory_clsid, NULL, CLSCTX_INPROC_SERVER, &factory_iid, (void **)&factory );
    if (FAILED(hr)) ERR( "no WIC: %#lx\n", hr );
    return SUCCEEDED(hr) ? factory : NULL;
}

/* what came over the network or from the disk, as bytes */
BOOL image_decode( struct image *image, const void *data, size_t len )
{
    IWICImagingFactory *factory;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER), .biPlanes = 1, .biBitCount = 32,
                                       .biCompression = BI_RGB } };
    UINT width = 0, height = 0;
    void *bits;
    HRESULT hr;

    if (!data || !len || len > 0xffffffff || !(factory = get_factory())) return FALSE;
    hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory( stream, (BYTE *)data, (DWORD)len );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream( factory, (IStream *)stream, NULL,
                                                                        WICDecodeMetadataCacheOnDemand, &decoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame( decoder, 0, &frame );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter( factory, &converter );
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)frame, &format_pbgra,
                                                            WICBitmapDitherTypeNone, NULL, 0.0,
                                                            WICBitmapPaletteTypeCustom );
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize( converter, &width, &height );
    if (SUCCEEDED(hr) && (!width || !height || width > MAX_IMAGE_SIDE || height > MAX_IMAGE_SIDE)) hr = E_FAIL;
    if (SUCCEEDED(hr))
    {
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -(LONG)height;
        if (!(image->bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0 ))) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_CopyPixels( converter, NULL, width * 4, width * height * 4, bits );

    if (converter) IWICFormatConverter_Release( converter );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (stream) IWICStream_Release( stream );
    if (factory) IWICImagingFactory_Release( factory );

    if (FAILED(hr))
    {
        WARN( "%s: no image (%#lx)\n", image->url, hr );
        if (image->bitmap) DeleteObject( image->bitmap );
        image->bitmap = NULL;
        return FALSE;
    }
    image->width = width;
    image->height = height;
    TRACE( "%s: %ux%u\n", image->url, width, height );
    return TRUE;
}
