/*
 * Snipping Tool
 *
 * SnippingTool.exe /clip is what Win+Shift+S starts (the raw input thread
 * of winsrv sees the keys): the screen snipping overlay, then the snip on
 * the clipboard, kept in Pictures\Screenshots and announced by a toast that
 * opens it for marking up. Started on its own, it is the window with New;
 * with a file, that image in the window.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "snippingtool.h"
#include "objbase.h"
#include "commctrl.h"
#include "shellapi.h"
#include "../../dlls/wpnapps/arctic_toast.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(snippingtool);

HINSTANCE instance;

static const WCHAR settings_key[] = L"Software\\Arctic\\SnippingTool";

static int editors;          /* windows open */
static BOOL toast_waiting;   /* the toast may still open one */

const WCHAR *string( UINT id )
{
    static WCHAR *cache[64];
    UINT slot = id - IDS_APP_NAME;
    const WCHAR *res;
    int len;

    if (slot >= ARRAY_SIZE(cache)) return L"";
    if (!cache[slot])
    {
        len = LoadStringW( instance, id, (WCHAR *)&res, 0 );
        if (!(cache[slot] = malloc( (len + 1) * sizeof(WCHAR) ))) return L"";
        memcpy( cache[slot], res, len * sizeof(WCHAR) );
        cache[slot][len] = 0;
    }
    return cache[slot];
}

/* the face of message boxes, height pixels high */
HFONT ui_font( UINT dpi, int height, int weight )
{
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    LOGFONTW font;

    if (!SystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi ))
        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    font = metrics.lfMessageFont;
    font.lfHeight = -height;
    font.lfWidth = 0;
    font.lfWeight = weight;
    font.lfQuality = ANTIALIASED_QUALITY;
    return CreateFontIndirectW( &font );
}

/* light or dark, as "Choose your default app mode" says */
void get_theme( struct theme *theme )
{
    DWORD light = 1, size = sizeof(light);

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size );
    theme->dark = !light;
    if (theme->dark)
    {
        theme->back = 0xff1c1c1c;
        theme->surface = 0xff202020;
        theme->text = 0xffffffff;
        theme->subtext = 0xffc5c5c5;
        theme->border = 0xff3a3a3a;
        theme->hover = 0xff2d2d2d;
        theme->pressed = 0xff383838;
        theme->accent = 0xff4cc2ff;
        theme->on_accent = 0xff000000;
    }
    else
    {
        theme->back = 0xffeeeeee;
        theme->surface = 0xfff9f9f9;
        theme->text = 0xff1a1a1a;
        theme->subtext = 0xff5f5f5f;
        theme->border = 0xffe0e0e0;
        theme->hover = 0xffeaeaea;
        theme->pressed = 0xffdedede;
        theme->accent = 0xff005fb8;
        theme->on_accent = 0xffffffff;
    }
}

DWORD setting( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

void set_setting( const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, settings_key, name, REG_DWORD, &value, sizeof(value) );
}

void editor_opened(void)
{
    editors++;
}

void editor_closed(void)
{
    if (!--editors && !toast_waiting) PostQuitMessage( 0 );
}

/* the toast */

static UINT toast_event;
static DWORD toast_id;
static struct image *snip;
static WCHAR snip_path[MAX_PATH];

static void toast_done(void)
{
    toast_waiting = FALSE;
    if (!editors) PostQuitMessage( 0 );
}

static LRESULT CALLBACK reply_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (toast_event && msg == toast_event)
    {
        if (wparam != toast_id || !toast_waiting) return 0;
        KillTimer( hwnd, 1 );
        if (lparam == ARCTIC_TOAST_ACTIVATED) editor_open( image_addref( snip ), snip_path );
        toast_done();
        return 0;
    }
    if (msg == WM_TIMER)  /* the shell never said: no reason to stay */
    {
        KillTimer( hwnd, 1 );
        if (toast_waiting) toast_done();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL CALLBACK find_taskbar( HWND hwnd, LPARAM lparam )
{
    WCHAR class[32];

    if (!GetClassNameW( hwnd, class, ARRAY_SIZE(class) ) || wcscmp( class, L"Shell_TrayWnd" )) return TRUE;
    *(HWND *)lparam = hwnd;
    return FALSE;
}

/* "Snip copied to clipboard", the snip beside it; a click opens it */
static BOOL show_toast( HWND reply )
{
    struct arctic_toast data = { .size = sizeof(data) };
    COPYDATASTRUCT copy = { .dwData = ARCTIC_TOAST_SHOW, .cbData = sizeof(data), .lpData = &data };
    DWORD_PTR result = 0;
    HWND taskbar = 0;

    EnumWindows( find_taskbar, (LPARAM)&taskbar );
    if (!taskbar) return FALSE;
    data.reply = HandleToULong( reply );
    data.id = toast_id = GetCurrentProcessId();
    lstrcpynW( data.app, string( IDS_APP_NAME ), ARRAY_SIZE(data.app) );
    GetModuleFileNameW( NULL, data.icon, ARRAY_SIZE(data.icon) );
    lstrcpynW( data.title, string( IDS_TOAST_TITLE ), ARRAY_SIZE(data.title) );
    lstrcpynW( data.text, string( IDS_TOAST_TEXT ), ARRAY_SIZE(data.text) );
    lstrcpynW( data.image, snip_path, ARRAY_SIZE(data.image) );
    return SendMessageTimeoutW( taskbar, WM_COPYDATA, (WPARAM)reply, (LPARAM)&copy, SMTO_ABORTIFHUNG, 5000,
                                &result ) && result;
}

/* Win+Shift+S. TRUE: the toast is up, the message loop waits for it */
static BOOL clip(void)
{
    WNDCLASSW class = { .lpfnWndProc = reply_proc, .hInstance = instance, .lpszClassName = L"ArcticSnipReply" };
    HANDLE mutex = CreateMutexW( NULL, FALSE, L"Local\\ArcticScreenSnipping" );
    HWND reply;

    if (GetLastError() == ERROR_ALREADY_EXISTS)  /* the keys again while the screen is being snipped */
    {
        CloseHandle( mutex );
        return FALSE;
    }
    snip = snip_screen();
    CloseHandle( mutex );
    if (!snip) return FALSE;

    if (!autosave_path( snip_path, ARRAY_SIZE(snip_path) ) || FAILED(save_image( snip_path, snip )))
        snip_path[0] = 0;
    RegisterClassW( &class );
    reply = CreateWindowExW( 0, class.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, instance, NULL );
    clipboard_set_image( reply, snip );
    if (!reply || !snip_path[0]) return FALSE;

    toast_event = RegisterWindowMessageW( ARCTIC_TOAST_EVENT );
    ChangeWindowMessageFilterEx( reply, toast_event, MSGFLT_ALLOW, NULL );
    toast_waiting = TRUE;
    if (!show_toast( reply ))
    {
        WARN( "no shell to show the toast\n" );
        toast_waiting = FALSE;
        return FALSE;
    }
    SetTimer( reply, 1, 120000, NULL );
    return TRUE;
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX controls = { .dwSize = sizeof(controls), .dwICC = ICC_WIN95_CLASSES };
    WCHAR **argv, path[MAX_PATH];
    BOOL wait;
    int argc;
    MSG msg;

    instance = inst;
    /* the screen's own pixels: a snip is never scaled. Here, as in winsrv,
     * the thread's awareness is what takes */
    SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    InitCommonControlsEx( &controls );
    argv = CommandLineToArgvW( GetCommandLineW(), &argc );

    if (argc > 1 && (!wcsicmp( argv[1], L"/clip" ) || !wcsicmp( argv[1], L"-clip" ) ||
                     !wcsnicmp( argv[1], L"ms-screenclip:", 14 )))
        wait = clip();
    else if (argc > 1 && GetFullPathNameW( argv[1], ARRAY_SIZE(path), path, NULL ))
        wait = editor_open( load_image( path ), path ) != 0;
    else
        wait = editor_open( NULL, NULL ) != 0;

    if (wait)
        while (GetMessageW( &msg, 0, 0, 0 ) > 0)
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    image_release( snip );
    LocalFree( argv );
    CoUninitialize();
    return 0;
}
