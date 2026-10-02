/*
 * The volume icon of the notification area without the taskbar's stobject.dll:
 * creates the shell service object of sndvolsso.dll and starts it as stobject
 * does, for a VM test of an image whose ReactOS files predate it. The icon
 * and its flyout live as long as this program.
 *
 *   volumetest
 *
 * zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows volumetest.c -o volumetest.exe -lole32
 */
#define COBJMACROS
#include <windows.h>
#include <docobj.h>
#include <shlguid.h>

static const CLSID CLSID_SndVolSSO =
    { 0xa6b0e3c1, 0x5f2d, 0x4b8a, { 0x9c, 0x7e, 0x31, 0xd4, 0xf0, 0xa2, 0xb6, 0xe8 } };
static const GUID CGID_ShellServiceObject_ =
    { 0x000214d2, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID IID_IOleCommandTarget_ =
    { 0xb722bccb, 0x4e68, 0x101b, { 0xa2, 0xbc, 0x00, 0xaa, 0x00, 0x40, 0x47, 0x70 } };

int main(void)
{
    IOleCommandTarget *target;
    HRESULT hr;
    MSG msg;

    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    hr = CoCreateInstance( &CLSID_SndVolSSO, NULL, CLSCTX_INPROC_SERVER, &IID_IOleCommandTarget_, (void **)&target );
    if (FAILED(hr))
    {
        MessageBoxA( NULL, "sndvolsso.dll is not registered", "volumetest", MB_OK );
        return 1;
    }
    IOleCommandTarget_Exec( target, &CGID_ShellServiceObject_, OLECMDID_NEW, OLECMDEXECOPT_DODEFAULT, NULL, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
