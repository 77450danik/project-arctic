/* Prints the ARGB of immersive colours named on the command line, from the
 * undocumented uxtheme exports (by ordinal), for the colour set in use.
 *
 *   immersivecolors.exe ImmersiveInputSwitchDarkBackground ... */
#include <windows.h>
#include <stdio.h>

typedef DWORD (WINAPI *GetImmersiveUserColorSetPreference_t)(BOOL, BOOL);
typedef DWORD (WINAPI *GetImmersiveColorTypeFromName_t)(const WCHAR *);
typedef DWORD (WINAPI *GetImmersiveColorFromColorSetEx_t)(DWORD, DWORD, BOOL, DWORD);

int wmain( int argc, WCHAR **argv )
{
    HMODULE ux = LoadLibraryW( L"uxtheme.dll" );
    GetImmersiveUserColorSetPreference_t pref = (void *)GetProcAddress( ux, MAKEINTRESOURCEA(98) );
    GetImmersiveColorTypeFromName_t type = (void *)GetProcAddress( ux, MAKEINTRESOURCEA(96) );
    GetImmersiveColorFromColorSetEx_t color = (void *)GetProcAddress( ux, MAKEINTRESOURCEA(95) );
    DWORD set;

    if (!pref || !type || !color) return 1;
    set = pref( FALSE, FALSE );
    for (int i = 1; i < argc; i++)
    {
        DWORD abgr = color( set, type( argv[i] ), FALSE, 0 );
        printf( "%ls #%02lX%02lX%02lX%02lX\n", argv[i], abgr >> 24, abgr & 0xff, (abgr >> 8) & 0xff,
                (abgr >> 16) & 0xff );
    }
    return 0;
}
