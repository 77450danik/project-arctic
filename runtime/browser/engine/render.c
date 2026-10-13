/*
 * Arctic web engine: a page into a picture, with no window
 *
 * What htmlrun.exe is built on (docs/browser.md, component 7): the same
 * document, layout and rasterizer as the window, into a PNG. The reference
 * screenshots and the web-platform-tests stand on it, and so does any check
 * that wants to see a page without a desktop.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>
#include <string.h>

#include "arctichtml_private.h"

#include <wincodec.h>

static const GUID factory_clsid = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID factory_iid = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID container_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID format_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static HRESULT save_png( const struct surface *surface, const WCHAR *path )
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID format = format_bgra;
    HRESULT hr;

    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    hr = CoCreateInstance( &factory_clsid, NULL, CLSCTX_INPROC_SERVER, &factory_iid, (void **)&factory );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream( factory, &stream );
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename( stream, path, GENERIC_WRITE );
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder( factory, &container_png, NULL, &encoder );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize( encoder, (IStream *)stream, WICBitmapEncoderNoCache );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame( encoder, &frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize( frame, NULL );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize( frame, surface->width, surface->height );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat( frame, &format );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels( frame, surface->height, surface->width * 4,
                                                               surface->width * surface->height * 4,
                                                               (BYTE *)surface->bits );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit( frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit( encoder );

    if (frame) IWICBitmapFrameEncode_Release( frame );
    if (encoder) IWICBitmapEncoder_Release( encoder );
    if (stream) IWICStream_Release( stream );
    if (factory) IWICImagingFactory_Release( factory );
    return hr;
}

HRESULT WINAPI ArcticHtmlRenderToFile( const WCHAR *url, UINT width, UINT height, BOOL full_page, const WCHAR *png )
{
    struct display_list list = { 0 };
    struct surface surface;
    struct document *doc;
    HRESULT hr;

    if (!url || !png || !width || !height) return E_INVALIDARG;
    if (!(doc = document_load( url, NULL, width, height ))) return E_FAIL;

    layout_document( doc, &list, width, height );
    if (full_page && list.content_height > (int)height) height = list.content_height;
    if (!surface_create( &surface, width, height ))
    {
        display_list_free( &list );
        document_free( doc );
        return E_OUTOFMEMORY;
    }
    raster_paint( &surface, &list, 0 );
    hr = save_png( &surface, png );

    surface_destroy( &surface );
    display_list_free( &list );
    font_cache_clear();
    document_free( doc );
    TRACE( "%s -> %s: %#lx\n", debugstr_w(url), debugstr_w(png), hr );
    return hr;
}
