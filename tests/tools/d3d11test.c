/* A Direct3D 11 window: the path a game takes, d3d11 -> DXVK -> Vulkan ->
 * dwm.exe. Prints the adapter and the feature level it got, then clears the
 * window to a changing colour. "d3d11test fullscreen" fills the primary
 * monitor without a frame; Esc closes it. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static LRESULT WINAPI proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_DESTROY) PostQuitMessage( 0 );
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) DestroyWindow( hwnd );
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI WinMain( HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show )
{
    WNDCLASSW cls = { .lpfnWndProc = proc, .hInstance = instance, .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                      .lpszClassName = L"d3d11test" };
    DXGI_SWAP_CHAIN_DESC desc = { 0 };
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swapchain;
    ID3D11Texture2D *back;
    ID3D11RenderTargetView *target;
    IDXGIDevice *dxgi;
    IDXGIAdapter *adapter;
    DXGI_ADAPTER_DESC info;
    D3D_FEATURE_LEVEL level;
    BOOL full = !lstrcmpiA( cmdline, "fullscreen" );
    HRESULT hr;
    HWND hwnd;
    MSG msg;

    RegisterClassW( &cls );
    if (full)
        hwnd = CreateWindowExW( 0, cls.lpszClassName, L"Direct3D 11", WS_POPUP | WS_VISIBLE, 0, 0,
                                GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ), 0, 0, instance, NULL );
    else
        hwnd = CreateWindowExW( 0, cls.lpszClassName, L"Direct3D 11", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, 0, 0, instance, NULL );

    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                        &desc, &swapchain, &device, &level, &context );
    if (FAILED(hr))
    {
        printf( "d3d11test: no device: %#lx\n", hr );
        return 1;
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface( device, &IID_IDXGIDevice, (void **)&dxgi )) &&
        SUCCEEDED(IDXGIDevice_GetAdapter( dxgi, &adapter )) && SUCCEEDED(IDXGIAdapter_GetDesc( adapter, &info )))
        printf( "d3d11test: %ls, %04x:%04x, feature level %x\n", info.Description, info.VendorId, info.DeviceId, level );
    fflush( stdout );

    IDXGISwapChain_GetBuffer( swapchain, 0, &IID_ID3D11Texture2D, (void **)&back );
    ID3D11Device_CreateRenderTargetView( device, (ID3D11Resource *)back, NULL, &target );

    for (int frame = 0;; frame++)
    {
        float color[4] = { (frame % 120) / 120.0f, 0.3f, 1.0f - (frame % 120) / 120.0f, 1.0f };

        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT) return 0;
            DispatchMessageW( &msg );
        }
        ID3D11DeviceContext_OMSetRenderTargets( context, 1, &target, NULL );
        ID3D11DeviceContext_ClearRenderTargetView( context, target, color );
        IDXGISwapChain_Present( swapchain, 1, 0 );
    }
}
