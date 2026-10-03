/*
 * A program that plays something, as Chrome or Spotify tell the system:
 * SystemMediaTransportControls for its window, playing "Shchedryk" by
 * "Arctic test" with previous, play/pause and next. The presses of the
 * shell's buttons show in its title ("mediatest: Next 1"), and play/pause
 * switches between playing and paused, as a player would. Before the first
 * press the title says how many sessions the shell's shared section holds.
 *
 *   mediatest
 *
 * Built against Wine's WinRT headers (tools/wsl/make-mediatest.sh):
 * x86_64-w64-mingw32-gcc -I<wine>/include mediatest.c -o mediatest.exe -lcombase -mwindows
 */
#define COBJMACROS
#include <stdio.h>
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <initguid.h>
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Media
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_Storage_Streams
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>

static HWND window;
static ISystemMediaTransportControls *controls;
static int presses;

static HRESULT WINAPI handler_QueryInterface( ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs *iface,
                                              REFIID iid, void **out )
{
    *out = iface;
    return S_OK;
}

static ULONG WINAPI handler_AddRef( ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs *iface )
{
    return 2;
}

static ULONG WINAPI handler_Release( ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs *iface )
{
    return 1;
}

static HRESULT WINAPI handler_Invoke( ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs *iface,
                                      ISystemMediaTransportControls *sender, ISystemMediaTransportControlsButtonPressedEventArgs *args )
{
    static const char *names[] = { "Play", "Pause", "Stop", "Record", "FastForward", "Rewind", "Next", "Previous" };
    SystemMediaTransportControlsButton button = 0;
    char title[64];

    ISystemMediaTransportControlsButtonPressedEventArgs_get_Button( args, &button );
    if (button == SystemMediaTransportControlsButton_Play)
        ISystemMediaTransportControls_put_PlaybackStatus( controls, MediaPlaybackStatus_Playing );
    else if (button == SystemMediaTransportControlsButton_Pause)
        ISystemMediaTransportControls_put_PlaybackStatus( controls, MediaPlaybackStatus_Paused );
    snprintf( title, sizeof(title), "mediatest: %s %d", button < 8 ? names[button] : "?", ++presses );
    SetWindowTextA( window, title );
    return S_OK;
}

static ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgsVtbl handler_vtbl =
{
    handler_QueryInterface,
    handler_AddRef,
    handler_Release,
    handler_Invoke,
};

static ITypedEventHandler_SystemMediaTransportControls_SystemMediaTransportControlsButtonPressedEventArgs handler = { &handler_vtbl };

static void set_string( HRESULT (WINAPI *put)( IMusicDisplayProperties *, HSTRING ), IMusicDisplayProperties *props,
                        const WCHAR *text )
{
    HSTRING str;

    WindowsCreateString( text, wcslen( text ), &str );
    put( props, str );
    WindowsDeleteString( str );
}

int WINAPI WinMain( HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show )
{
    static const WCHAR class_name[] = L"Windows.Media.SystemMediaTransportControls";
    ISystemMediaTransportControlsInterop *interop;
    ISystemMediaTransportControlsDisplayUpdater *updater;
    IMusicDisplayProperties *music;
    EventRegistrationToken token;
    HSTRING name;
    MSG msg;
    HRESULT hr;

    RoInitialize( RO_INIT_SINGLETHREADED );
    window = CreateWindowExW( 0, L"STATIC", L"mediatest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 420, 160,
                              NULL, NULL, instance, NULL );

    WindowsCreateString( class_name, wcslen( class_name ), &name );
    hr = RoGetActivationFactory( name, &IID_ISystemMediaTransportControlsInterop, (void **)&interop );
    WindowsDeleteString( name );
    if (FAILED(hr))
    {
        SetWindowTextA( window, "mediatest: no SystemMediaTransportControls" );
        goto loop;
    }
    ISystemMediaTransportControlsInterop_GetForWindow( interop, window, &IID_ISystemMediaTransportControls, (void **)&controls );

    ISystemMediaTransportControls_put_IsEnabled( controls, TRUE );
    ISystemMediaTransportControls_put_IsPlayEnabled( controls, TRUE );
    ISystemMediaTransportControls_put_IsPauseEnabled( controls, TRUE );
    ISystemMediaTransportControls_put_IsNextEnabled( controls, TRUE );
    ISystemMediaTransportControls_put_IsPreviousEnabled( controls, TRUE );
    ISystemMediaTransportControls_add_ButtonPressed( controls, &handler, &token );

    ISystemMediaTransportControls_get_DisplayUpdater( controls, &updater );
    ISystemMediaTransportControlsDisplayUpdater_put_Type( updater, MediaPlaybackType_Music );
    ISystemMediaTransportControlsDisplayUpdater_get_MusicProperties( updater, &music );
    set_string( music->lpVtbl->put_Title, music, L"Щедрик" );
    set_string( music->lpVtbl->put_Artist, music, L"Arctic test" );
    IMusicDisplayProperties_Release( music );
    ISystemMediaTransportControlsDisplayUpdater_Update( updater );
    ISystemMediaTransportControlsDisplayUpdater_Release( updater );
    ISystemMediaTransportControls_put_PlaybackStatus( controls, MediaPlaybackStatus_Playing );
    {
        /* how many sessions the shell can see (wine/arctic_media.h: 16 rows of
         * 800 bytes after a 12 byte header, each starting with its id) */
        HANDLE mapping = OpenFileMappingW( FILE_MAP_READ, FALSE, L"Global\\__arctic_media" );
        const BYTE *view = mapping ? MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 ) : NULL;
        char title[64];
        int rows = 0;

        for (int i = 0; view && i < 16; i++)
            if (*(const UINT32 *)(view + 12 + i * 800)) rows++;
        snprintf( title, sizeof(title), "mediatest: playing (%s, %d sessions)", view ? "shared" : "no section", rows );
        SetWindowTextA( window, title );
    }

loop:
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
