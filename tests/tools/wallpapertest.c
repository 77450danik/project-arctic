/* Goes through the steps PaintDesktop takes to load the wallpaper and prints
 * the result of each, then paints the desktop into a bitmap and samples it. */
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>

int main( void )
{
    WCHAR path[MAX_PATH];
    HRESULT (WINAPI *create)( UINT, IWICImagingFactory ** ) = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    HMODULE module;
    HRESULT hr;
    UINT w = 0, h = 0;
    HDC screen, dc;
    HBITMAP bmp;
    RECT rect = { 0, 0, 1280, 800 };

    path[0] = 0;
    printf( "wallpapertest: SPI_GETDESKWALLPAPER %d '%ls'\n",
            SystemParametersInfoW( SPI_GETDESKWALLPAPER, MAX_PATH, path, 0 ), path );
    printf( "wallpapertest: file attributes %#lx\n", GetFileAttributesW( path ) );
    module = LoadLibraryW( L"windowscodecs.dll" );
    if (module) create = (void *)GetProcAddress( module, "WICCreateImagingFactory_Proxy" );
    printf( "wallpapertest: windowscodecs %p proxy %p\n", module, create );
    if (!create) return 1;
    hr = create( WINCODEC_SDK_VERSION, &factory );
    printf( "wallpapertest: factory %#lx\n", hr );
    if (FAILED(hr)) return 1;
    hr = IWICImagingFactory_CreateDecoderFromFilename( factory, path, NULL, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &decoder );
    printf( "wallpapertest: decoder %#lx\n", hr );
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame( decoder, 0, &frame );
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize( frame, &w, &h );
    printf( "wallpapertest: frame %#lx size %ux%u\n", hr, w, h );

    screen = GetDC( 0 );
    dc = CreateCompatibleDC( screen );
    bmp = CreateCompatibleBitmap( screen, rect.right, rect.bottom );
    SelectObject( dc, bmp );
    printf( "wallpapertest: PaintDesktop %d pixels %06lx %06lx %06lx\n", PaintDesktop( dc ),
            GetPixel( dc, 100, 100 ), GetPixel( dc, 640, 400 ), GetPixel( dc, 1100, 700 ) );
    return 0;
}
