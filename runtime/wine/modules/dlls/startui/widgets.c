/*
 * Windows Widgets: the weather on the taskbar, the widgets board it opens,
 * and the widgets of the network (the weather, the news, Copilot)
 *
 * A button in the taskbar shows the weather: explorer puts it at the far left
 * of a centred taskbar and next to the tray otherwise (traywnd.cpp,
 * ArcticPlaceWidget). It opens the board where it stands, from the taskbar's
 * edge in Windows 10's style, with a margin and rounded corners in Windows
 * 11's: the widgets of the list Board (gadgets.c keeps the lists) in two
 * columns, each in its size, "..." on each for its sizes and the rest, "+" at
 * the top for more. The desktop shows the same widgets, drawn by the same
 * code, in the same sizes.
 *
 * Where the data comes from, every half hour and when the board opens:
 * ipwho.is (the town of this address, unless the user set one),
 * geocoding-api.open-meteo.com (its name in the user's language),
 * api.open-meteo.com (the weather), www.bing.com/news (the headlines and
 * their pictures, in the user's language and country; only the stories that
 * have a picture are kept, three of them). A link, and what is asked of
 * Copilot, open in the default browser.
 *
 * HKCU\Software\Arctic\Widgets: Show (0 off, 1 the icon, 2 the icon and the
 * text), Hover (the board opens under the pointer), City, Latitude, Longitude
 * (the town the user set) and what was last seen, for the next logon.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "shellapi.h"
#include "objbase.h"
#include "wincodec.h"
#include "winhttp.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define WIDGETS_KEY     L"Software\\Arctic\\Widgets"
#define USER_AGENT      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)"

#define WM_WIDGET_DATA  (WM_APP + 1)
#define WM_BOARD_RELOAD (WM_APP + 2)
#define WM_BOARD_TICK   (WM_APP + 3)

#define TIMER_REFRESH   1
#define TIMER_FIRST     2
#define TIMER_RETRY     3
#define TIMER_PANEL     4
#define TIMER_SMOOTH    5
#define TIMER_CARET     6

#define NEWS_COUNT      3       /* the stories shown, each with its picture */
#define NEWS_SCAN       30      /* the feed's items looked at for them */
#define MAX_DAYS        5
#define PIC_W           px( 640 )
#define HEADER_10       px( 52 )
#define HEADER_11       px( 76 )

#define BUTTON_HOT      1       /* as ArcticTaskbarPaintButton takes them */
#define BUTTON_PRESSED  2
#define BUTTON_START    16

BOOL WINAPI ArcticTaskbarIsLight(void);
void WINAPI ArcticTaskbarPaintButton( HDC hdc, const RECT *button, DWORD flags );

struct day
{
    WCHAR name[16];
    int code, high, low;
};

struct news
{
    WCHAR title[300], source[80], url[1400];
    UINT32 *pixels;     /* premultiplied, whole, PIC_W at most, or NULL */
    int width, height;
};

struct data
{
    BOOL have_weather;
    WCHAR city[64];
    int temp, feels, humidity, wind, code;
    BOOL is_day, imperial;
    struct day days[MAX_DAYS];
    int day_count;
    struct news news[NEWS_COUNT];
    int news_count;
};

struct fonts
{
    HFONT title, body, small_, big, huge_, headline;
};

static HWND button, tray, panel;
static struct data *data;
static SRWLOCK data_lock = SRWLOCK_INIT;   /* the desktop's thread reads data too */
static BOOL button_hot, button_down, panel_hover, style11, in_popup;
static DWORD hidden_at, fetched_at;
static LONG fetching;

static struct palette pal;
static struct look look;

/**********************************************************************
 *          The registry
 */

static DWORD get_dword( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, WIDGETS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

static void set_dword( const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, WIDGETS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static BOOL get_string( const WCHAR *name, WCHAR *text, DWORD chars )
{
    DWORD size = chars * sizeof(WCHAR);

    text[0] = 0;
    return !RegGetValueW( HKEY_CURRENT_USER, WIDGETS_KEY, name, RRF_RT_REG_SZ, NULL, text, &size ) && text[0];
}

static void set_string( const WCHAR *name, const WCHAR *text )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, WIDGETS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    if (text) RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)text, (lstrlenW( text ) + 1) * sizeof(WCHAR) );
    else RegDeleteValueW( key, name );
    RegCloseKey( key );
}

DWORD ArcticWidgetShow(void)
{
    return min( get_dword( L"Show", 2 ), 2 );
}

void ArcticWidgetSetShow( DWORD show )
{
    set_dword( L"Show", show );
}

/**********************************************************************
 *          The network
 */

/* the bytes of a URL, with a NUL after them, for free() */
static char *http_get( const WCHAR *url, DWORD *out_size )
{
    URL_COMPONENTS parts = { sizeof(parts) };
    WCHAR host[256], path[2800], extra[2800];
    HINTERNET session = NULL, connection = NULL, request = NULL;
    DWORD size = 0, capacity = 0, status = 0, len = sizeof(status), got;
    char *body = NULL, *grown;
    BOOL ok = FALSE;

    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAY_SIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAY_SIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAY_SIZE(extra);
    if (!WinHttpCrackUrl( url, 0, 0, &parts )) return NULL;
    if (lstrlenW( path ) + lstrlenW( extra ) >= ARRAY_SIZE(path)) return NULL;
    lstrcatW( path, extra );

    if (!(session = WinHttpOpen( USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0 )))
        return NULL;
    WinHttpSetTimeouts( session, 8000, 8000, 8000, 15000 );
    if (!(connection = WinHttpConnect( session, host, parts.nPort, 0 ))) goto done;
    if (!(request = WinHttpOpenRequest( connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0 )))
        goto done;
    if (!WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 )) goto done;
    if (!WinHttpReceiveResponse( request, NULL )) goto done;
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL ) ||
        status < 200 || status >= 300)
        goto done;
    for (;;)
    {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable( request, &available )) goto done;
        if (!available) break;
        if (size + available + 1 > capacity)
        {
            capacity = max( capacity * 2, size + available + 1 );
            if (capacity > 8 * 1024 * 1024 || !(grown = realloc( body, capacity ))) goto done;
            body = grown;
        }
        if (!WinHttpReadData( request, body + size, available, &got )) goto done;
        size += got;
    }
    if (!body) body = malloc( 1 );
    if (body)
    {
        body[size] = 0;
        ok = TRUE;
    }
done:
    if (request) WinHttpCloseHandle( request );
    if (connection) WinHttpCloseHandle( connection );
    WinHttpCloseHandle( session );
    if (!ok)
    {
        free( body );
        return NULL;
    }
    if (out_size) *out_size = size;
    return body;
}

static void url_encode( const WCHAR *text, WCHAR *out, int out_size )
{
    char utf8[4096];
    int n = WideCharToMultiByte( CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL ), used = 0;

    for (int i = 0; i < n - 1 && used < out_size - 4; i++)
    {
        unsigned char c = utf8[i];

        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.')
            out[used++] = c;
        else
        {
            swprintf( out + used, 4, L"%%%02X", c );
            used += 3;
        }
    }
    out[used] = 0;
}

/* JSON, as far as these answers need: what follows "key": */
static const char *json_key( const char *p, const char *key )
{
    size_t n = strlen( key );

    while (p && (p = strchr( p, '"' )))
    {
        if (!strncmp( p + 1, key, n ) && p[n + 1] == '"')
        {
            const char *q = p + n + 2;

            while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
            if (*q == ':')
            {
                q++;
                while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

static BOOL json_number( const char *p, const char *key, double *value )
{
    if (!(p = json_key( p, key ))) return FALSE;
    *value = strtod( p, NULL );
    return TRUE;
}

static BOOL json_string( const char *p, const char *key, WCHAR *text, int chars )
{
    const char *end;
    int n;

    if (!(p = json_key( p, key )) || *p != '"') return FALSE;
    p++;
    if (!(end = strchr( p, '"' ))) return FALSE;
    n = MultiByteToWideChar( CP_UTF8, 0, p, end - p, text, chars - 1 );
    text[n] = 0;
    return n > 0;
}

static int json_numbers( const char *p, const char *key, double *values, int max_count )
{
    int count = 0;

    if (!(p = json_key( p, key )) || *p != '[') return 0;
    p++;
    while (count < max_count && *p && *p != ']')
    {
        char *end;

        values[count++] = strtod( p, &end );
        if (end == p) break;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return count;
}

static int round_int( double value )
{
    return (int)floor( value + 0.5 );
}

/* the user's language and country: uk, UA */
static void user_locale( WCHAR *lang, WCHAR *country )
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH] = L"en-US", *dash;

    GetUserDefaultLocaleName( name, ARRAY_SIZE(name) );
    if ((dash = wcschr( name, '-' )))
    {
        *dash = 0;
        lstrcpynW( country, dash + 1, 8 );
    }
    else lstrcpyW( country, L"US" );
    lstrcpynW( lang, name, 8 );
}

static BOOL find_town( const WCHAR *name, const WCHAR *lang, WCHAR *city, int chars, double *lat, double *lon )
{
    WCHAR url[512], encoded[256];
    const char *results;
    BOOL ok = FALSE;
    char *body;

    url_encode( name, encoded, ARRAY_SIZE(encoded) );
    swprintf( url, ARRAY_SIZE(url), L"https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=%s&format=json",
              encoded, lang );
    if (!(body = http_get( url, NULL ))) return FALSE;
    if ((results = json_key( body, "results" )) && json_number( results, "latitude", lat ) &&
        json_number( results, "longitude", lon ))
    {
        json_string( results, "name", city, chars );
        ok = TRUE;
    }
    free( body );
    return ok;
}

/* where the weather is of: the town the user set, or that of this address */
static BOOL find_place( const WCHAR *lang, WCHAR *city, int chars, double *lat, double *lon )
{
    static BOOL known;
    static double known_lat, known_lon;
    static WCHAR known_city[64];
    WCHAR text[32];
    char *body;
    double value;
    BOOL ok = FALSE;

    if (get_string( L"Latitude", text, ARRAY_SIZE(text) ))
    {
        *lat = wcstod( text, NULL );
        if (get_string( L"Longitude", text, ARRAY_SIZE(text) ))
        {
            *lon = wcstod( text, NULL );
            get_string( L"City", city, chars );
            return TRUE;
        }
    }
    if (known)
    {
        lstrcpynW( city, known_city, chars );
        *lat = known_lat;
        *lon = known_lon;
        return TRUE;
    }
    if (!(body = http_get( L"https://ipwho.is/?fields=success,city,latitude,longitude", NULL ))) return FALSE;
    if (json_string( body, "city", known_city, ARRAY_SIZE(known_city) ) && json_number( body, "latitude", &known_lat ) &&
        json_number( body, "longitude", &known_lon ))
    {
        WCHAR name[64];

        /* the town's name in the user's language, if it can be had */
        if (find_town( known_city, lang, name, ARRAY_SIZE(name), &value, &value )) lstrcpyW( known_city, name );
        lstrcpynW( city, known_city, chars );
        *lat = known_lat;
        *lon = known_lon;
        known = ok = TRUE;
    }
    free( body );
    return ok;
}

static void fetch_weather( struct data *d, const WCHAR *lang, const WCHAR *country )
{
    WCHAR url[700];
    double lat, lon, value, codes[MAX_DAYS], high[MAX_DAYS], low[MAX_DAYS];
    const char *section;
    char *body;
    int count;

    if (!find_place( lang, d->city, ARRAY_SIZE(d->city), &lat, &lon )) return;
    d->imperial = !lstrcmpiW( country, L"US" );
    swprintf( url, ARRAY_SIZE(url),
              L"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
              L"&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
              L"&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=%d&timezone=auto%s",
              lat, lon, MAX_DAYS, d->imperial ? L"&temperature_unit=fahrenheit&wind_speed_unit=mph" : L"" );
    if (!(body = http_get( url, NULL ))) return;
    if ((section = json_key( body, "current" )) && json_number( section, "temperature_2m", &value ))
    {
        d->temp = round_int( value );
        if (json_number( section, "apparent_temperature", &value )) d->feels = round_int( value );
        if (json_number( section, "relative_humidity_2m", &value )) d->humidity = round_int( value );
        if (json_number( section, "wind_speed_10m", &value )) d->wind = round_int( value );
        if (json_number( section, "weather_code", &value )) d->code = round_int( value );
        if (json_number( section, "is_day", &value )) d->is_day = value != 0;
        d->have_weather = TRUE;
    }
    if ((section = json_key( body, "daily" )))
    {
        const char *times = json_key( section, "time" );

        count = json_numbers( section, "weather_code", codes, MAX_DAYS );
        count = min( count, json_numbers( section, "temperature_2m_max", high, MAX_DAYS ) );
        count = min( count, json_numbers( section, "temperature_2m_min", low, MAX_DAYS ) );
        for (int i = 0; i < count && times; i++)
        {
            SYSTEMTIME st = { 0 };
            int year, month, day;

            if (!(times = strchr( times, '"' ))) break;
            if (sscanf( times + 1, "%d-%d-%d", &year, &month, &day ) != 3) break;
            times = strchr( times + 1, '"' );
            if (times) times++;
            st.wYear = year;
            st.wMonth = month;
            st.wDay = day;
            GetDateFormatW( LOCALE_USER_DEFAULT, 0, &st, L"ddd", d->days[i].name, ARRAY_SIZE(d->days[i].name) );
            d->days[i].code = round_int( codes[i] );
            d->days[i].high = round_int( high[i] );
            d->days[i].low = round_int( low[i] );
            d->day_count = i + 1;
        }
    }
    free( body );
}

static void decode_entities( WCHAR *text )
{
    static const struct { const WCHAR *name; WCHAR c; } names[] =
    {
        { L"amp;", '&' }, { L"lt;", '<' }, { L"gt;", '>' }, { L"quot;", '"' }, { L"apos;", '\'' }, { L"nbsp;", ' ' },
    };
    WCHAR *in = text, *out = text;

    while (*in)
    {
        BOOL done = FALSE;

        if (*in == '&')
        {
            if (in[1] == '#')
            {
                BOOL hex = in[2] == 'x' || in[2] == 'X';
                WCHAR *end;
                long value = wcstol( in + (hex ? 3 : 2), &end, hex ? 16 : 10 );

                if (*end == ';' && value > 0 && value < 0xffff)
                {
                    *out++ = value;
                    in = end + 1;
                    done = TRUE;
                }
            }
            for (UINT i = 0; i < ARRAY_SIZE(names) && !done; i++)
            {
                int n = lstrlenW( names[i].name );

                if (!wcsncmp( in + 1, names[i].name, n ))
                {
                    *out++ = names[i].c;
                    in += n + 1;
                    done = TRUE;
                }
            }
        }
        if (!done) *out++ = *in++;
    }
    *out = 0;
}

static BOOL xml_text( const char *start, const char *end, const char *tag, WCHAR *text, int chars )
{
    char open[40], close[40];
    const char *p, *q;
    int n;

    sprintf( open, "<%s>", tag );
    sprintf( close, "</%s>", tag );
    if (!(p = strstr( start, open )) || p >= end) return FALSE;
    p += strlen( open );
    if (!(q = strstr( p, close )) || q > end) return FALSE;
    if (!strncmp( p, "<![CDATA[", 9 ))
    {
        p += 9;
        if (q - 3 >= p && !strncmp( q - 3, "]]>", 3 )) q -= 3;
    }
    n = MultiByteToWideChar( CP_UTF8, 0, p, q - p, text, chars - 1 );
    text[n] = 0;
    decode_entities( text );
    decode_entities( text );    /* some feeds encode theirs twice */
    return n > 0;
}

/* a picture, whole, no larger than the width and height: each widget crops
 * what it needs of it (draw_cover) */
static BOOL decode_image( const BYTE *bytes, DWORD size, struct news *n, int max_w, int max_h )
{
    int width, height;
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapScaler *scaler = NULL;
    UINT src_w = 0, src_h = 0, scaled_w, scaled_h;
    UINT32 *pixels = NULL;
    double fit;
    WICRect rect;
    BOOL ok = FALSE;

    if (FAILED(CoCreateInstance( &wic_factory_clsid, NULL, CLSCTX_INPROC_SERVER, &wic_factory_iid, (void **)&factory ))) return FALSE;
    if (FAILED(IWICImagingFactory_CreateStream( factory, &stream ))) goto done;
    if (FAILED(IWICStream_InitializeFromMemory( stream, (BYTE *)bytes, size ))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream( factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                           &decoder )))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame( decoder, 0, &frame ))) goto done;
    if (FAILED(IWICBitmapFrameDecode_GetSize( frame, &src_w, &src_h )) || !src_w || !src_h) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter( factory, &converter ))) goto done;
    if (FAILED(IWICFormatConverter_Initialize( converter, (IWICBitmapSource *)frame, &wic_format_pbgra, WICBitmapDitherTypeNone,
                                               NULL, 0.0, WICBitmapPaletteTypeCustom )))
        goto done;
    fit = min( 1.0, min( (double)max_w / src_w, (double)max_h / src_h ) );
    width = scaled_w = max( 1, (int)floor( src_w * fit + 0.5 ) );
    height = scaled_h = max( 1, (int)floor( src_h * fit + 0.5 ) );
    if (FAILED(IWICImagingFactory_CreateBitmapScaler( factory, &scaler ))) goto done;
    if (FAILED(IWICBitmapScaler_Initialize( scaler, (IWICBitmapSource *)converter, scaled_w, scaled_h,
                                            WICBitmapInterpolationModeFant )))
        goto done;
    if (!(pixels = malloc( width * height * sizeof(*pixels) ))) goto done;
    rect.X = (scaled_w - width) / 2;
    rect.Y = (scaled_h - height) / 2;
    rect.Width = width;
    rect.Height = height;
    if (SUCCEEDED(IWICBitmapScaler_CopyPixels( scaler, &rect, width * sizeof(*pixels), width * height * sizeof(*pixels),
                                               (BYTE *)pixels )))
    {
        n->pixels = pixels;
        n->width = width;
        n->height = height;
        pixels = NULL;
        ok = TRUE;
    }
    free( pixels );
done:
    if (scaler) IWICBitmapScaler_Release( scaler );
    if (converter) IWICFormatConverter_Release( converter );
    if (frame) IWICBitmapFrameDecode_Release( frame );
    if (decoder) IWICBitmapDecoder_Release( decoder );
    if (stream) IWICStream_Release( stream );
    IWICImagingFactory_Release( factory );
    return ok;
}

static void fetch_image( struct news *n, const WCHAR *address )
{
    WCHAR url[1000];
    DWORD size = 0;
    char *body;

    /* bing's own resizer, and over https; the width alone: given a height
     * too, it pads the picture with white instead of cropping it */
    swprintf( url, ARRAY_SIZE(url), L"https:%s&w=%d", !wcsncmp( address, L"http:", 5 ) ? address + 5 :
              !wcsncmp( address, L"https:", 6 ) ? address + 6 : address, PIC_W );
    if (!(body = http_get( url, &size ))) return;
    decode_image( (const BYTE *)body, size, n, PIC_W, PIC_W );
    free( body );
}

static void fetch_news( struct data *d, const WCHAR *lang, const WCHAR *country )
{
    WCHAR url[512], encoded[128], query[64], language[20], image[900];
    const char *p, *end;
    int scanned = 0;
    char *body;

    lstrcpynW( query, load_string( IDS_WIDGET_QUERY ), ARRAY_SIZE(query) );
    url_encode( query, encoded, ARRAY_SIZE(encoded) );
    swprintf( language, ARRAY_SIZE(language), L"%s-%s", lang, country );
    CharLowerW( language );
    swprintf( url, ARRAY_SIZE(url), L"https://www.bing.com/news/search?q=%s&format=rss&setlang=%s&cc=%s", encoded, language, country );
    if (!(body = http_get( url, NULL ))) return;
    for (p = body; d->news_count < NEWS_COUNT && scanned++ < NEWS_SCAN && (p = strstr( p, "<item>" )); p = end)
    {
        struct news *n = &d->news[d->news_count];

        if (!(end = strstr( p, "</item>" ))) break;
        memset( n, 0, sizeof(*n) );
        if (!xml_text( p, end, "title", n->title, ARRAY_SIZE(n->title) ) ||
            !xml_text( p, end, "link", n->url, ARRAY_SIZE(n->url) ))
            continue;
        /* a story without its picture is not shown: the next one is taken */
        if (!xml_text( p, end, "News:Image", image, ARRAY_SIZE(image) )) continue;
        fetch_image( n, image );
        if (!n->pixels) continue;
        xml_text( p, end, "News:Source", n->source, ARRAY_SIZE(n->source) );
        d->news_count++;
    }
    free( body );
}

static void data_free( struct data *d )
{
    if (!d) return;
    for (int i = 0; i < NEWS_COUNT; i++) free( d->news[i].pixels );
    free( d );
}

static DWORD WINAPI fetch_thread( void *arg )
{
    struct data *d = calloc( 1, sizeof(*d) );
    WCHAR lang[8], country[8];
    HRESULT init = CoInitializeEx( NULL, COINIT_MULTITHREADED );

    if (d)
    {
        user_locale( lang, country );
        fetch_weather( d, lang, country );
        fetch_news( d, lang, country );
        if (!PostMessageW( button, WM_WIDGET_DATA, 0, (LPARAM)d )) data_free( d );
    }
    if (SUCCEEDED(init)) CoUninitialize();
    InterlockedExchange( &fetching, 0 );
    return 0;
}

static void start_fetch(void)
{
    HANDLE thread;

    if (InterlockedCompareExchange( &fetching, 1, 0 )) return;
    if ((thread = CreateThread( NULL, 0, fetch_thread, NULL, 0, NULL ))) CloseHandle( thread );
    else InterlockedExchange( &fetching, 0 );
}

/**********************************************************************
 *          The pictures of the weather, drawn four times over
 */

#define ICON_SAMPLES 4

static void blend_coverage( UINT32 *pixel, COLORREF color, int coverage )
{
    UINT32 dst = *pixel, inverse = 255 - coverage;
    UINT32 a = coverage + ((dst >> 24) * inverse) / 255;
    UINT32 r = GetRValue( color ) * coverage / 255 + (((dst >> 16) & 0xff) * inverse) / 255;
    UINT32 g = GetGValue( color ) * coverage / 255 + (((dst >> 8) & 0xff) * inverse) / 255;
    UINT32 b = GetBValue( color ) * coverage / 255 + ((dst & 0xff) * inverse) / 255;

    *pixel = (a << 24) | (r << 16) | (g << 8) | b;
}

struct shape
{
    int type;               /* 0 a disc, 1 a capsule, 2 a triangle */
    float a, b, c, d, e, f;
};

static BOOL shape_has( const struct shape *s, float x, float y )
{
    if (s->type == 0)
        return (x - s->a) * (x - s->a) + (y - s->b) * (y - s->b) <= s->c * s->c;
    if (s->type == 1)
    {
        float vx = s->c - s->a, vy = s->d - s->b, wx = x - s->a, wy = y - s->b;
        float t = (wx * vx + wy * vy) / max( vx * vx + vy * vy, 1e-6f ), dx, dy;

        t = max( 0.0f, min( 1.0f, t ) );
        dx = wx - t * vx;
        dy = wy - t * vy;
        return dx * dx + dy * dy <= s->e * s->e;
    }
    {
        float d1 = (x - s->c) * (s->b - s->d) - (s->a - s->c) * (y - s->d);
        float d2 = (x - s->e) * (s->d - s->f) - (s->c - s->e) * (y - s->f);
        float d3 = (x - s->a) * (s->f - s->b) - (s->e - s->a) * (y - s->b);

        return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
    }
}

static void draw_shape( UINT32 *pixels, int size, const struct shape *s, COLORREF color )
{
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int hits = 0;

            for (int j = 0; j < ICON_SAMPLES; j++)
                for (int i = 0; i < ICON_SAMPLES; i++)
                    if (shape_has( s, (x + (i + 0.5f) / ICON_SAMPLES) / size, (y + (j + 0.5f) / ICON_SAMPLES) / size )) hits++;
            if (hits) blend_coverage( &pixels[y * size + x], color, hits * 255 / (ICON_SAMPLES * ICON_SAMPLES) );
        }
    }
}

static void disc( UINT32 *pixels, int size, float x, float y, float r, COLORREF color )
{
    struct shape s = { 0, x, y, r };

    draw_shape( pixels, size, &s, color );
}

static void capsule( UINT32 *pixels, int size, float x0, float y0, float x1, float y1, float r, COLORREF color )
{
    struct shape s = { 1, x0, y0, x1, y1, r };

    draw_shape( pixels, size, &s, color );
}

static void triangle( UINT32 *pixels, int size, float x0, float y0, float x1, float y1, float x2, float y2, COLORREF color )
{
    struct shape s = { 2, x0, y0, x1, y1, x2, y2 };

    draw_shape( pixels, size, &s, color );
}

static void sun( UINT32 *pixels, int size, float x, float y, float r, COLORREF color )
{
    for (int i = 0; i < 8; i++)
    {
        float angle = i * 3.14159265f / 4, c = cosf( angle ), s = sinf( angle );

        capsule( pixels, size, x + c * r * 1.45f, y + s * r * 1.45f, x + c * r * 1.9f, y + s * r * 1.9f, r * 0.14f, color );
    }
    disc( pixels, size, x, y, r, color );
}

static void moon( UINT32 *pixels, int size, float x, float y, float r, COLORREF color )
{
    UINT32 *cut = calloc( size * size, sizeof(*cut) );

    disc( pixels, size, x, y, r, color );
    if (!cut) return;
    /* the shadow's side taken away */
    disc( cut, size, x + r * 0.55f, y - r * 0.35f, r * 0.9f, RGB( 255, 255, 255 ) );
    for (int i = 0; i < size * size; i++)
    {
        UINT32 keep = 255 - (cut[i] >> 24), p = pixels[i];

        pixels[i] = (((p >> 24) * keep / 255) << 24) | ((((p >> 16) & 0xff) * keep / 255) << 16) |
                    ((((p >> 8) & 0xff) * keep / 255) << 8) | ((p & 0xff) * keep / 255);
    }
    free( cut );
}

static void cloud( UINT32 *pixels, int size, float x, float y, float s, COLORREF color )
{
    disc( pixels, size, x - 0.2f * s, y + 0.06f * s, 0.16f * s, color );
    disc( pixels, size, x + 0.0f * s, y - 0.08f * s, 0.24f * s, color );
    disc( pixels, size, x + 0.22f * s, y + 0.03f * s, 0.19f * s, color );
    capsule( pixels, size, x - 0.2f * s, y + 0.14f * s, x + 0.22f * s, y + 0.14f * s, 0.08f * s, color );
}

/* over something light the cloud is darker */
static void draw_weather( HDC hdc, int code, BOOL day, int x, int y, int size, BOOL light )
{
    COLORREF yellow = RGB( 0xff, 0xb9, 0x00 ), pale = RGB( 0xe8, 0xe4, 0xbf );
    COLORREF front = light ? RGB( 0xa4, 0xb1, 0xc0 ) : RGB( 0xe8, 0xed, 0xf3 );
    COLORREF back = light ? RGB( 0x80, 0x8e, 0x9f ) : RGB( 0xb1, 0xbc, 0xc9 );
    COLORREF water = RGB( 0x3f, 0x9f, 0xef ), snow = light ? RGB( 0x5a, 0xa5, 0xe2 ) : RGB( 0xd2, 0xeb, 0xff );
    UINT32 *p = calloc( size * size, sizeof(*p) );

    if (!p) return;
    switch (code)
    {
    case 0:
        if (day) sun( p, size, 0.5f, 0.5f, 0.23f, yellow );
        else moon( p, size, 0.5f, 0.5f, 0.3f, pale );
        break;
    case 1:
    case 2:
        if (day) sun( p, size, 0.36f, 0.36f, 0.15f, yellow );
        else moon( p, size, 0.36f, 0.34f, 0.19f, pale );
        cloud( p, size, 0.54f, 0.58f, 0.9f, front );
        break;
    case 45:
    case 48:
        cloud( p, size, 0.5f, 0.38f, 0.95f, front );
        capsule( p, size, 0.2f, 0.74f, 0.8f, 0.74f, 0.035f, back );
        capsule( p, size, 0.3f, 0.86f, 0.7f, 0.86f, 0.035f, back );
        break;
    case 51: case 53: case 55: case 56: case 57:
        cloud( p, size, 0.5f, 0.38f, 0.95f, front );
        for (int i = 0; i < 3; i++)
            capsule( p, size, 0.34f + i * 0.17f, 0.72f, 0.31f + i * 0.17f, 0.8f, 0.028f, water );
        break;
    case 61: case 63: case 65: case 66: case 67: case 80: case 81: case 82:
        cloud( p, size, 0.5f, 0.38f, 0.95f, front );
        for (int i = 0; i < 3; i++)
            capsule( p, size, 0.35f + i * 0.17f, 0.7f, 0.28f + i * 0.17f, 0.88f, 0.032f, water );
        break;
    case 71: case 73: case 75: case 77: case 85: case 86:
        cloud( p, size, 0.5f, 0.38f, 0.95f, front );
        for (int i = 0; i < 3; i++) disc( p, size, 0.32f + i * 0.18f, 0.76f + (i % 2) * 0.09f, 0.048f, snow );
        break;
    case 95: case 96: case 99:
        cloud( p, size, 0.5f, 0.36f, 0.95f, front );
        triangle( p, size, 0.56f, 0.56f, 0.38f, 0.76f, 0.5f, 0.76f, yellow );
        triangle( p, size, 0.5f, 0.7f, 0.62f, 0.7f, 0.42f, 0.94f, yellow );
        break;
    default:
        cloud( p, size, 0.42f, 0.42f, 0.62f, back );
        cloud( p, size, 0.53f, 0.56f, 0.9f, front );
        break;
    }
    blend_bits( hdc, x, y, size, size, p );
    free( p );
}

static const WCHAR *weather_text( int code, BOOL day )
{
    UINT id;

    switch (code)
    {
    case 0: id = day ? IDS_WX_SUNNY : IDS_WX_CLEAR; break;
    case 1: id = IDS_WX_MOSTLY_CLEAR; break;
    case 2: id = IDS_WX_PARTLY_CLOUDY; break;
    case 45: case 48: id = IDS_WX_FOG; break;
    case 51: case 53: case 55: id = IDS_WX_DRIZZLE; break;
    case 56: case 57: case 66: case 67: id = IDS_WX_FREEZING; break;
    case 61: case 63: case 65: id = IDS_WX_RAIN; break;
    case 71: case 73: case 75: case 77: id = IDS_WX_SNOW; break;
    case 80: case 81: case 82: id = IDS_WX_SHOWERS; break;
    case 85: case 86: id = IDS_WX_SNOW_SHOWERS; break;
    case 95: case 96: case 99: id = IDS_WX_STORM; break;
    default: id = IDS_WX_CLOUDY; break;
    }
    return load_string( id );
}

/**********************************************************************
 *          Fonts
 */

static void fonts_make( struct fonts *f )
{
    f->title = shell_font( px( 14 ), FW_SEMIBOLD );
    f->body = shell_font( px( 14 ), FW_NORMAL );
    f->small_ = shell_font( px( 12 ), FW_NORMAL );
    f->big = shell_font( px( 28 ), FW_SEMIBOLD );
    f->huge_ = shell_font( px( 48 ), FW_LIGHT );
    f->headline = shell_font( px( 18 ), FW_SEMIBOLD );
}

static void fonts_free( struct fonts *f )
{
    DeleteObject( f->title );
    DeleteObject( f->body );
    DeleteObject( f->small_ );
    DeleteObject( f->big );
    DeleteObject( f->huge_ );
    DeleteObject( f->headline );
}

static int text_width( HDC hdc, HFONT font, const WCHAR *text )
{
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    return size.cx;
}

/* the height of a text in a width, in no more than some lines */
static int text_height( HDC hdc, HFONT font, const WCHAR *text, int width, int lines )
{
    HGDIOBJ old = SelectObject( hdc, font );
    RECT rect = { 0, 0, width, 0 };
    TEXTMETRICW tm;

    GetTextMetricsW( hdc, &tm );
    DrawTextW( hdc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( hdc, old );
    return min( rect.bottom, lines * tm.tmHeight );
}

/**********************************************************************
 *          The button
 */

static void degrees( WCHAR *text, int chars, int value, BOOL unit )
{
    if (unit) swprintf( text, chars, L"%d\x00b0%c", value, data->imperial ? 'F' : 'C' );
    else swprintf( text, chars, L"%d\x00b0", value );
}

int WINAPI ArcticWidgetWidth(void)
{
    struct fonts f;
    WCHAR temp[16];
    const WCHAR *text;
    DWORD show = ArcticWidgetShow();
    int width;
    HDC hdc;

    if (!button || !show || !data || !data->have_weather) return 0;
    if (show == 1) return px( 40 );
    if (!(hdc = GetDC( button ))) return 0;
    fonts_make( &f );
    degrees( temp, ARRAY_SIZE(temp), data->temp, TRUE );
    text = weather_text( data->code, data->is_day );
    if (taskbar_style11())
        width = px( 8 + 24 + 8 + 10 ) + max( text_width( hdc, f.small_, temp ), text_width( hdc, f.small_, text ) );
    else
        width = px( 6 + 20 + 6 + 6 + 8 ) + text_width( hdc, f.small_, temp ) + text_width( hdc, f.small_, text );
    fonts_free( &f );
    ReleaseDC( button, hdc );
    return min( width, px( 220 ) );
}

static void paint_button(void)
{
    struct palette p;
    struct fonts f;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client, r;
    WCHAR temp[16];
    const WCHAR *text;
    BOOL win11 = taskbar_style11();
    HDC mem;

    palette_init( &p, ArcticTaskbarIsLight() );
    if (!(mem = paint_begin( button, &ps, &p, &bitmap, &old ))) return;
    GetClientRect( button, &client );
    if (button_hot || button_down || (panel && IsWindowVisible( panel )))
    {
        if (win11) ArcticTaskbarPaintButton( mem, &client, BUTTON_START | (button_down ? BUTTON_PRESSED : BUTTON_HOT) );
        else fill_alpha( mem, &client, p.ink, button_down ? 0x14 : 0x20 );
    }
    if (data && data->have_weather)
    {
        int icon = px( win11 ? 24 : 20 ), height = client.bottom, x = px( win11 ? 8 : 6 );

        if (ArcticWidgetShow() == 1) x = (client.right - icon) / 2;
        draw_weather( mem, data->code, data->is_day, x, (height - icon) / 2, icon, p.light );
        if (ArcticWidgetShow() == 2)
        {
            fonts_make( &f );
            degrees( temp, ARRAY_SIZE(temp), data->temp, TRUE );
            text = weather_text( data->code, data->is_day );
            x += icon + px( win11 ? 8 : 6 );
            if (win11)
            {
                int line = px( 16 ), top = (height - 2 * line) / 2;

                SetRect( &r, x, top, client.right - px( 4 ), top + line );
                paint_text( mem, temp, &r, f.small_, p.text, DT_LEFT );
                SetRect( &r, x, top + line, client.right - px( 4 ), top + 2 * line );
                paint_text( mem, text, &r, f.small_, p.dim, DT_LEFT );
            }
            else
            {
                int width = text_width( mem, f.small_, temp );

                SetRect( &r, x, 0, x + width + px( 2 ), height );
                paint_text( mem, temp, &r, f.small_, p.text, DT_LEFT );
                SetRect( &r, x + width + px( 6 ), 0, client.right - px( 2 ), height );
                paint_text( mem, text, &r, f.small_, p.text, DT_LEFT );
            }
            fonts_free( &f );
        }
    }
    paint_end( button, &ps, mem, bitmap, old );
}

/**********************************************************************
 *          The widgets of the network: what they share
 */

static int card_radius( BOOL eleven )
{
    return eleven ? px( 8 ) : 0;
}

static BYTE card_alpha( const struct palette *p )
{
    return p->light ? 0x0a : 0x10;
}

static COLORREF mix( COLORREF a, COLORREF b, int t, int range )
{
    return RGB( GetRValue( a ) + (GetRValue( b ) - GetRValue( a )) * t / range,
                GetGValue( a ) + (GetGValue( b ) - GetGValue( a )) * t / range,
                GetBValue( a ) + (GetBValue( b ) - GetBValue( a )) * t / range );
}

static BYTE corner_coverage( int x, int y, int width, int height, int radius, BOOL bottom )
{
    int hits = 0;

    if (radius <= 0) return 255;
    if (x >= radius && x < width - radius) return 255;
    if (y >= radius && (y < height - radius || !bottom)) return 255;
    for (int j = 0; j < 4; j++)
    {
        for (int i = 0; i < 4; i++)
        {
            float px_ = x + (i + 0.5f) / 4, py = y + (j + 0.5f) / 4;
            float dx = max( max( radius - px_, px_ - (width - radius) ), 0.f );
            float dy = max( radius - py, bottom ? py - (height - radius) : 0.f );

            dy = max( dy, 0.f );
            if (dx * dx + dy * dy <= radius * radius) hits++;
        }
    }
    return hits * 255 / 16;
}

static void gradient_card( HDC hdc, const RECT *r, int radius, COLORREF top, COLORREF bottom )
{
    int width = r->right - r->left, height = r->bottom - r->top;
    UINT32 *pixels = malloc( width * height * sizeof(*pixels) );

    if (!pixels) return;
    for (int y = 0; y < height; y++)
    {
        COLORREF c = mix( top, bottom, y, max( height - 1, 1 ) );

        for (int x = 0; x < width; x++)
        {
            UINT32 a = corner_coverage( x, y, width, height, radius, TRUE );

            pixels[y * width + x] = (a << 24) | ((GetRValue( c ) * a / 255) << 16) | ((GetGValue( c ) * a / 255) << 8) |
                                    (GetBValue( c ) * a / 255);
        }
    }
    blend_bits( hdc, r->left, r->top, width, height, pixels );
    free( pixels );
}

/* a story's picture filling r, its middle kept; toward the bottom darker,
 * under its title, when scrim */
static void draw_cover( HDC hdc, const struct news *n, const RECT *r, int radius, BOOL bottom_round, BOOL scrim )
{
    int width = r->right - r->left, height = r->bottom - r->top, taps;
    float scale, ox, oy;
    UINT32 *pixels;

    if (!n->pixels || width <= 0 || height <= 0 || !(pixels = malloc( width * height * sizeof(*pixels) ))) return;
    scale = max( (float)width / n->width, (float)height / n->height );
    ox = (n->width - width / scale) / 2;
    oy = (n->height - height / scale) / 2;
    /* smaller than half: more samples a pixel, or it shimmers */
    taps = max( 1, min( 6, (int)ceilf( 1 / scale ) ) );
    for (int y = 0; y < height; y++)
    {
        float dark = 1;

        if (scrim)
        {
            float t = (y - height * 0.35f) / (height * 0.65f);

            if (t > 0) dark = 1 - 0.75f * min( t, 1.f );
        }
        for (int x = 0; x < width; x++)
        {
            float sum[4] = { 0 };
            UINT32 a = corner_coverage( x, y, width, height, radius, bottom_round ), out[4];

            for (int j = 0; j < taps; j++)
            {
                for (int i = 0; i < taps; i++)
                {
                    float sx = ox + (x + (i + 0.5f) / taps) / scale - 0.5f, sy = oy + (y + (j + 0.5f) / taps) / scale - 0.5f;
                    int x0, y0, x1, y1;
                    float fx, fy;

                    sx = max( 0.f, min( sx, n->width - 1.f ) );
                    sy = max( 0.f, min( sy, n->height - 1.f ) );
                    x0 = (int)sx;
                    y0 = (int)sy;
                    x1 = min( x0 + 1, n->width - 1 );
                    y1 = min( y0 + 1, n->height - 1 );
                    fx = sx - x0;
                    fy = sy - y0;
                    for (int c = 0; c < 4; c++)
                    {
                        float p00 = (n->pixels[y0 * n->width + x0] >> (c * 8)) & 0xff;
                        float p10 = (n->pixels[y0 * n->width + x1] >> (c * 8)) & 0xff;
                        float p01 = (n->pixels[y1 * n->width + x0] >> (c * 8)) & 0xff;
                        float p11 = (n->pixels[y1 * n->width + x1] >> (c * 8)) & 0xff;

                        sum[c] += (p00 * (1 - fx) + p10 * fx) * (1 - fy) + (p01 * (1 - fx) + p11 * fx) * fy;
                    }
                }
            }
            for (int c = 0; c < 4; c++)
            {
                float v = sum[c] / (taps * taps);

                if (c < 3) v *= dark;
                out[c] = (UINT32)(v * a / 255 + 0.5f);
            }
            pixels[y * width + x] = (out[3] << 24) | (out[2] << 16) | (out[1] << 8) | out[0];
        }
    }
    blend_bits( hdc, r->left, r->top, width, height, pixels );
    free( pixels );
}

static void weather_colors( COLORREF *top, COLORREF *bottom )
{
    int code = data->code;

    if (!data->is_day)
    {
        *top = RGB( 0x18, 0x24, 0x44 );
        *bottom = RGB( 0x2f, 0x45, 0x76 );
    }
    else if (code <= 2)
    {
        *top = RGB( 0x1f, 0x7a, 0xd1 );
        *bottom = RGB( 0x62, 0xb0, 0xef );
    }
    else if (code == 3 || code == 45 || code == 48)
    {
        *top = RGB( 0x47, 0x64, 0x86 );
        *bottom = RGB( 0x7c, 0x98, 0xb6 );
    }
    else if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    {
        *top = RGB( 0x56, 0x80, 0xa8 );
        *bottom = RGB( 0x98, 0xb7, 0xd3 );
    }
    else
    {
        *top = RGB( 0x34, 0x47, 0x5d );
        *bottom = RGB( 0x58, 0x6e, 0x87 );
    }
}

static void card_text( HDC hdc, BOOL opaque, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    if (opaque) text_over_opaque( hdc, text, rect, font, color, format );
    else paint_text( hdc, text, rect, font, color, format );
}

/* the card under a widget of the board; on the desktop its window is the card */
static void plain_card( const struct widget_paint *p )
{
    if (!p->desktop) fill_round_rect( p->hdc, &p->rect, (float)card_radius( p->style11 ), p->pal->ink, card_alpha( p->pal ) );
}

static void no_connection( const struct widget_paint *p, const struct fonts *f )
{
    RECT t = p->rect;

    plain_card( p );
    paint_text( p->hdc, load_string( IDS_WIDGET_NO_DATA ), &t, f->body, p->pal->dim, DT_CENTER );
}

/**********************************************************************
 *          The weather
 */

struct weather_ink
{
    BOOL opaque, light;
    COLORREF text, dim, ink;
};

/* the picture and the degrees, and what it is like beside them */
static void weather_now( HDC hdc, const struct fonts *f, const struct weather_ink *ink, int x, int y, int right, int icon )
{
    WCHAR line[64];
    int temp_w, detail;
    RECT t;

    draw_weather( hdc, data->code, data->is_day, x, y, icon, ink->light );
    degrees( line, ARRAY_SIZE(line), data->temp, FALSE );
    temp_w = text_width( hdc, f->huge_, line );
    SetRect( &t, x + icon + px( 8 ), y, x + icon + px( 8 ) + temp_w + px( 4 ), y + icon );
    card_text( hdc, ink->opaque, line, &t, f->huge_, ink->text, DT_LEFT );
    detail = t.right + px( 14 );
    SetRect( &t, detail, y + icon / 2 - px( 20 ), right, y + icon / 2 );
    card_text( hdc, ink->opaque, weather_text( data->code, data->is_day ), &t, f->body, ink->text, DT_LEFT | DT_END_ELLIPSIS );
    swprintf( line, ARRAY_SIZE(line), load_string( IDS_WIDGET_FEELS ), data->feels );
    SetRect( &t, detail, y + icon / 2, right, y + icon / 2 + px( 18 ) );
    card_text( hdc, ink->opaque, line, &t, f->small_, ink->dim, DT_LEFT | DT_END_ELLIPSIS );
}

/* "Humidity 80%  ·  Wind 12 km/h" */
static void weather_air( HDC hdc, const struct fonts *f, const struct weather_ink *ink, RECT *t )
{
    WCHAR line[128], wind[48];
    int n;

    n = swprintf( line, ARRAY_SIZE(line), load_string( IDS_WIDGET_HUMIDITY ), data->humidity );
    swprintf( wind, ARRAY_SIZE(wind), load_string( data->imperial ? IDS_WIDGET_WIND_MPH : IDS_WIDGET_WIND ), data->wind );
    if (n > 0) swprintf( line + n, ARRAY_SIZE(line) - n, L"  \x00b7  %s", wind );
    card_text( hdc, ink->opaque, line, t, f->small_, ink->dim, DT_LEFT | DT_END_ELLIPSIS );
}

/* the days side by side: the name, the picture, the highest and lowest */
static void weather_days_row( HDC hdc, const struct fonts *f, const struct weather_ink *ink, int left, int right, int top )
{
    int cell = data->day_count ? (right - left) / data->day_count : 0, mini = px( 32 );
    WCHAR line[32];
    RECT t;

    for (int i = 0; i < data->day_count; i++)
    {
        int cx = left + i * cell;

        SetRect( &t, cx, top, cx + cell, top + px( 18 ) );
        card_text( hdc, ink->opaque, data->days[i].name, &t, f->small_, ink->dim, DT_CENTER );
        draw_weather( hdc, data->days[i].code, TRUE, cx + (cell - mini) / 2, top + px( 22 ), mini, ink->light );
        swprintf( line, ARRAY_SIZE(line), L"%d\x00b0 / %d\x00b0", data->days[i].high, data->days[i].low );
        SetRect( &t, cx, top + px( 58 ), cx + cell, top + px( 76 ) );
        card_text( hdc, ink->opaque, line, &t, f->small_, ink->text, DT_CENTER );
    }
}

/* the days one under the other, each with a bar of its lowest to its
 * highest among those of all of them */
static void weather_days_list( HDC hdc, const struct fonts *f, const struct weather_ink *ink, const RECT *area )
{
    int low = 1000, high = -1000, rows = data->day_count, row_h, mini = px( 28 );
    COLORREF bar = ink->opaque ? RGB( 255, 255, 255 ) : ink->ink;
    WCHAR line[16];
    RECT t;

    if (!rows) return;
    for (int i = 0; i < rows; i++)
    {
        low = min( low, data->days[i].low );
        high = max( high, data->days[i].high );
    }
    row_h = (area->bottom - area->top) / rows;
    for (int i = 0; i < rows; i++)
    {
        int top = area->top + i * row_h, cy = top + row_h / 2, x = area->left;
        int bar_left = x + px( 144 ), bar_right = area->right - px( 44 ), span = max( high - low, 1 );
        RECT track, fill;

        SetRect( &t, x, top, x + px( 60 ), top + row_h );
        card_text( hdc, ink->opaque, data->days[i].name, &t, f->body, ink->text, DT_LEFT );
        draw_weather( hdc, data->days[i].code, TRUE, x + px( 62 ), cy - mini / 2, mini, ink->light );
        degrees( line, ARRAY_SIZE(line), data->days[i].low, FALSE );
        SetRect( &t, x + px( 96 ), top, bar_left - px( 10 ), top + row_h );
        card_text( hdc, ink->opaque, line, &t, f->body, ink->dim, DT_RIGHT );
        SetRect( &track, bar_left, cy - px( 2 ), bar_right, cy + px( 2 ) );
        fill_round_rect( hdc, &track, (float)px( 2 ), bar, 0x30 );
        SetRect( &fill, bar_left + (bar_right - bar_left) * (data->days[i].low - low) / span, track.top,
                 bar_left + (bar_right - bar_left) * (data->days[i].high - low) / span, track.bottom );
        if (fill.right - fill.left < px( 4 )) fill.right = fill.left + px( 4 );
        fill_round_rect( hdc, &fill, (float)px( 2 ), bar, 0xd0 );
        degrees( line, ARRAY_SIZE(line), data->days[i].high, FALSE );
        SetRect( &t, bar_right + px( 10 ), top, area->right, top + row_h );
        card_text( hdc, ink->opaque, line, &t, f->body, ink->text, DT_LEFT );
    }
}

static void paint_weather( struct widget_paint *p, const struct fonts *f )
{
    const RECT *r = &p->rect;
    int pad = px( 16 ), x = r->left + pad, y = r->top + px( 12 ), more = px( 30 );
    struct weather_ink ink;
    HDC hdc = p->hdc;
    RECT t;

    if (!data->have_weather)
    {
        no_connection( p, f );
        return;
    }
    widget_add_target( p, HIT_CARD, 0, r );
    ink.opaque = p->style11;
    ink.light = !ink.opaque && p->pal->light;
    ink.text = ink.opaque ? RGB( 255, 255, 255 ) : p->pal->text;
    ink.dim = ink.opaque ? RGB( 0xe4, 0xee, 0xf8 ) : p->pal->dim;
    ink.ink = p->pal->ink;
    if (ink.opaque)
    {
        COLORREF top, bottom;

        weather_colors( &top, &bottom );
        gradient_card( hdc, r, card_radius( p->style11 ), top, bottom );
    }
    else plain_card( p );

    SetRect( &t, x, y, r->right - pad - more, y + px( 20 ) );
    card_text( hdc, ink.opaque, data->city, &t, f->title, ink.text, DT_LEFT | DT_END_ELLIPSIS );
    y += px( 28 );

    if (p->size == W_LARGE)
    {
        int split = (r->left + r->right) / 2;
        RECT days;

        weather_now( hdc, f, &ink, x, y + px( 6 ), split - px( 8 ), px( 88 ) );
        SetRect( &t, x, y + px( 112 ), split - px( 8 ), y + px( 130 ) );
        weather_air( hdc, f, &ink, &t );
        SetRect( &days, split + px( 8 ), r->top + px( 44 ), r->right - pad, r->bottom - px( 40 ) );
        weather_days_list( hdc, f, &ink, &days );
    }
    else
    {
        weather_now( hdc, f, &ink, x, y + px( 4 ), r->right - pad, px( 56 ) );
        SetRect( &t, x, y + px( 72 ), r->right - pad, y + px( 90 ) );
        weather_air( hdc, f, &ink, &t );
        if (p->size == W_MEDIUM) weather_days_row( hdc, f, &ink, x, r->right - pad, r->top + px( 184 ) );
    }
    if (p->size != W_SMALL)
    {
        SetRect( &t, x, r->bottom - px( 30 ), r->right - pad, r->bottom - px( 10 ) );
        card_text( hdc, ink.opaque, load_string( IDS_WIDGET_FORECAST ), &t, f->small_, ink.dim, DT_RIGHT );
    }
}

/**********************************************************************
 *          The news: three stories, each with its picture
 */

static int news_wanted( enum widget_size size )
{
    return size == W_SMALL ? 1 : NEWS_COUNT;
}

/* the picture filling the tile, the title over its dark bottom */
static void story_cover( struct widget_paint *p, const struct fonts *f, int i, const RECT *r, int radius, BOOL bottom_round,
                         HFONT title_font, int lines )
{
    const struct news *n = &data->news[i];
    int pad = px( 12 ), height;
    RECT t;

    draw_cover( p->hdc, n, r, radius, bottom_round, TRUE );
    SetRect( &t, r->left + pad, r->bottom - pad - px( 16 ), r->right - pad, r->bottom - pad );
    text_over_opaque( p->hdc, n->source, &t, f->small_, RGB( 0xd8, 0xd8, 0xd8 ), DT_LEFT );
    height = text_height( p->hdc, title_font, n->title, r->right - r->left - 2 * pad, lines );
    SetRect( &t, r->left + pad, t.top - px( 4 ) - height, r->right - pad, t.top - px( 4 ) );
    text_over_opaque( p->hdc, n->title, &t, title_font, RGB( 255, 255, 255 ), DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS );
    widget_add_target( p, HIT_STORY, i, r );
}

/* a small picture at the left, the title and where it is from beside it */
static void story_row( struct widget_paint *p, const struct fonts *f, int i, const RECT *r )
{
    const struct news *n = &data->news[i];
    int pad = px( 12 ), thumb = r->bottom - r->top - px( 16 ), height;
    RECT pic, t;

    SetRect( &pic, r->left + pad, r->top + px( 8 ), r->left + pad + thumb, r->top + px( 8 ) + thumb );
    draw_cover( p->hdc, n, &pic, px( 6 ), TRUE, FALSE );
    height = text_height( p->hdc, f->title, n->title, r->right - pic.right - 2 * pad, 2 );
    SetRect( &t, pic.right + pad, pic.top, r->right - pad, pic.top + height );
    paint_text( p->hdc, n->title, &t, f->title, p->pal->text, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS );
    SetRect( &t, pic.right + pad, t.bottom + px( 2 ), r->right - pad, t.bottom + px( 20 ) );
    paint_text( p->hdc, n->source, &t, f->small_, p->pal->dim, DT_LEFT );
    widget_add_target( p, HIT_STORY, i, r );
}

static void paint_news( struct widget_paint *p, const struct fonts *f )
{
    const RECT *r = &p->rect;
    int count = min( data->news_count, news_wanted( p->size ) ), radius = card_radius( p->style11 ), gap = px( 6 );
    int width = r->right - r->left, height = r->bottom - r->top;
    RECT a, b, c;

    if (!count)
    {
        no_connection( p, f );
        return;
    }
    if (count == 1)
    {
        story_cover( p, f, 0, r, radius, TRUE, p->size == W_SMALL ? f->title : f->headline, p->size == W_SMALL ? 2 : 3 );
        return;
    }
    if (p->size == W_MEDIUM)
    {
        /* the first over the width, the others in rows under it */
        int top = height * 54 / 100, row = (height - top) / (count - 1);

        plain_card( p );
        SetRect( &a, r->left, r->top, r->right, r->top + top );
        story_cover( p, f, 0, &a, radius, FALSE, f->title, 2 );
        for (int i = 1; i < count; i++)
        {
            SetRect( &b, r->left, a.bottom + (i - 1) * row, r->right, a.bottom + i * row );
            story_row( p, f, i, &b );
        }
        return;
    }
    /* large: the first at the left, the others at the right one over the other */
    SetRect( &a, r->left, r->top, r->left + (width - gap) / 2, r->bottom );
    if (count == 2)
    {
        SetRect( &b, a.right + gap, r->top, r->right, r->bottom );
        story_cover( p, f, 0, &a, radius, TRUE, f->headline, 3 );
        story_cover( p, f, 1, &b, radius, TRUE, f->headline, 3 );
        return;
    }
    SetRect( &b, a.right + gap, r->top, r->right, r->top + (height - gap) / 2 );
    SetRect( &c, a.right + gap, b.bottom + gap, r->right, r->bottom );
    story_cover( p, f, 0, &a, radius, TRUE, f->headline, 4 );
    story_cover( p, f, 1, &b, radius, TRUE, f->title, 2 );
    story_cover( p, f, 2, &c, radius, TRUE, f->title, 2 );
}

/**********************************************************************
 *          Copilot
 */

/* a sparkle with hollow sides, its colour running across it, and a small one
 * beside it: Copilot's mark here */
static float sparkle_at( float x, float y, float cx, float cy, float r )
{
    float dx = fabsf( x - cx ) / r, dy = fabsf( y - cy ) / r;

    return sqrtf( dx ) + sqrtf( dy );
}

static void draw_sparkle( HDC hdc, int x, int y, int size )
{
    static const COLORREF from = RGB( 0x3a, 0x9b, 0xff ), to = RGB( 0xd0, 0x6c, 0xf0 );
    UINT32 *p = calloc( size * size, sizeof(*p) );

    if (!p) return;
    for (int py = 0; py < size; py++)
    {
        for (int px_ = 0; px_ < size; px_++)
        {
            int hits = 0;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float u = (px_ + (i + 0.5f) / 4) / size, v = (py + (j + 0.5f) / 4) / size;

                    if (sparkle_at( u, v, 0.42f, 0.56f, 0.42f ) <= 1.0f || sparkle_at( u, v, 0.82f, 0.18f, 0.17f ) <= 1.0f) hits++;
                }
            }
            if (hits) blend_coverage( &p[py * size + px_], mix( from, to, px_ + py, max( 2 * size - 2, 1 ) ), hits * 255 / 16 );
        }
    }
    blend_bits( hdc, x, y, size, size, p );
    free( p );
}

/* an arrow to the right, for sending */
static void draw_arrow( HDC hdc, int x, int y, int size, COLORREF color )
{
    UINT32 *p = calloc( size * size, sizeof(*p) );

    if (!p) return;
    capsule( p, size, 0.16f, 0.5f, 0.82f, 0.5f, 0.06f, color );
    capsule( p, size, 0.54f, 0.22f, 0.82f, 0.5f, 0.06f, color );
    capsule( p, size, 0.54f, 0.78f, 0.82f, 0.5f, 0.06f, color );
    blend_bits( hdc, x, y, size, size, p );
    free( p );
}

/* a plus, for adding */
static void draw_plus( HDC hdc, int x, int y, int size, COLORREF color )
{
    UINT32 *p = calloc( size * size, sizeof(*p) );

    if (!p) return;
    capsule( p, size, 0.2f, 0.5f, 0.8f, 0.5f, 0.045f, color );
    capsule( p, size, 0.5f, 0.2f, 0.5f, 0.8f, 0.045f, color );
    blend_bits( hdc, x, y, size, size, p );
    free( p );
}

/* the mark and the field; what is typed is drawn over it, frame by frame */
static void paint_copilot( struct widget_paint *p, const struct fonts *f )
{
    const RECT *r = &p->rect;
    int pad = px( 16 ), mark = px( 22 ), side = px( 32 ), inset = px( 4 ), field_h = px( 40 );
    int y = r->top + (r->bottom - r->top - (mark + px( 16 ) + field_h)) / 2;
    RECT t, field, send;

    plain_card( p );
    draw_sparkle( p->hdc, r->left + pad, y, mark );
    SetRect( &t, r->left + pad + mark + px( 8 ), y, r->right - pad - px( 30 ), y + mark );
    paint_text( p->hdc, L"Copilot", &t, f->title, p->pal->text, DT_LEFT );
    SetRect( &field, r->left + pad, y + mark + px( 16 ), r->right - pad, y + mark + px( 16 ) + field_h );
    fill_round_rect( p->hdc, &field, (float)(p->style11 ? px( 6 ) : 0), p->pal->ink, p->pal->light ? 0x0d : 0x14 );
    widget_add_target( p, HIT_ASK, 0, &field );
    SetRect( &send, field.right - inset - side, field.top + inset, field.right - inset, field.bottom - inset );
    widget_add_target( p, HIT_SEND, 0, &send );
    if (p->ask)
    {
        p->ask->field = field;
        p->ask->send = send;
    }
}

void widget_paint_ask( HDC hdc, const struct palette *p, struct widget_ask *ask, BOOL send_hot, BOOL eleven, int shift )
{
    HFONT body = shell_font( px( 14 ), FW_NORMAL );
    const WCHAR *shown = ask->text;
    RECT field = ask->field, send = ask->send, t;
    int width, arrow = px( 18 );

    OffsetRect( &field, 0, shift );
    OffsetRect( &send, 0, shift );
    if (ask->text[0]) fill_round_rect( hdc, &send, (float)(eleven ? px( 4 ) : 0), accent_color(), 255 );
    else if (send_hot) fill_round_rect( hdc, &send, (float)(eleven ? px( 4 ) : 0), p->ink, 0x14 );
    draw_arrow( hdc, (send.left + send.right - arrow) / 2, (send.top + send.bottom - arrow) / 2, arrow,
                ask->text[0] ? RGB( 255, 255, 255 ) : p->dim );
    SetRect( &t, field.left + px( 12 ), field.top, field.right - px( 44 ), field.bottom );
    width = t.right - t.left;
    if (!ask->text[0]) paint_text( hdc, load_string( IDS_WIDGET_ASK ), &t, body, p->dim, DT_LEFT );
    else
    {
        /* the end of a long question: that is where the typing is */
        while (*shown && text_width( hdc, body, shown ) > width) shown++;
        paint_text( hdc, shown, &t, body, p->text, DT_LEFT );
    }
    if (ask->active && ask->caret)
    {
        int x = t.left + (ask->text[0] ? text_width( hdc, body, shown ) + 1 : 0);
        RECT caret;

        SetRect( &caret, x, field.top + px( 10 ), x + 1, field.bottom - px( 10 ) );
        fill_alpha( hdc, &caret, p->ink, 255 );
    }
    DeleteObject( body );
}

enum ask_result widget_ask_input( struct widget_ask *ask, UINT msg, WPARAM wparam, HWND hwnd )
{
    int len = lstrlenW( ask->text );

    if (msg == WM_CHAR)
    {
        if (wparam < 0x20 || wparam == 0x7f || len >= WIDGET_ASK_MAX) return ASK_IGNORED;
        ask->text[len] = (WCHAR)wparam;
        ask->text[len + 1] = 0;
        return ASK_CHANGED;
    }
    if (msg != WM_KEYDOWN) return ASK_IGNORED;
    switch (wparam)
    {
    case VK_ESCAPE:
        if (!ask->text[0]) return ASK_LEAVE;
        ask->text[0] = 0;
        return ASK_CHANGED;
    case VK_RETURN:
        return ask->text[0] ? ASK_SEND : ASK_IGNORED;
    case VK_BACK:
        if (!len) return ASK_IGNORED;
        ask->text[len - 1] = 0;
        return ASK_CHANGED;
    case 'V':
    {
        HANDLE handle;
        const WCHAR *text;

        if (!(GetKeyState( VK_CONTROL ) & 0x8000) || !OpenClipboard( hwnd )) return ASK_IGNORED;
        if ((handle = GetClipboardData( CF_UNICODETEXT )) && (text = GlobalLock( handle )))
        {
            for (; *text && len < WIDGET_ASK_MAX; text++)
                if (*text >= 0x20 && *text != 0x7f) ask->text[len++] = *text;
            ask->text[len] = 0;
            GlobalUnlock( handle );
        }
        CloseClipboard();
        return ASK_CHANGED;
    }
    }
    return ASK_IGNORED;
}

/**********************************************************************
 *          The widgets of the network, for both
 */

void widget_paint_net( struct widget_paint *p )
{
    struct fonts f;

    fonts_make( &f );
    AcquireSRWLockShared( &data_lock );
    if (p->kind == W_COPILOT) paint_copilot( p, &f );
    else if (data && p->kind == W_WEATHER) paint_weather( p, &f );
    else if (data && p->kind == W_NEWS) paint_news( p, &f );
    else if (!data) no_connection( p, &f );
    ReleaseSRWLockShared( &data_lock );
    fonts_free( &f );
}

/* a card under the pointer, a little lighter: only what has alpha, the card
 * itself and its picture. GDI's text has none, dwm.exe keys it by brightness,
 * and alpha laid over it would turn it into a faint shadow. */
void widget_tint( HDC hdc, const RECT *r, int top, COLORREF color, BYTE alpha )
{
    UINT32 sr = GetRValue( color ) * alpha / 255, sg = GetGValue( color ) * alpha / 255, sb = GetBValue( color ) * alpha / 255;
    UINT32 inverse = 255 - alpha;
    DIBSECTION dib;
    HGDIOBJ bitmap = GetCurrentObject( hdc, OBJ_BITMAP );

    GdiFlush();
    if (!bitmap || GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !dib.dsBm.bmBits || dib.dsBm.bmBitsPixel != 32) return;
    for (int y = max( r->top, top ); y < min( r->bottom, abs( (int)dib.dsBm.bmHeight ) ); y++)
    {
        UINT32 *row = (UINT32 *)((BYTE *)dib.dsBm.bmBits + y * dib.dsBm.bmWidthBytes);

        for (int x = max( r->left, 0 ); x < min( r->right, (int)dib.dsBm.bmWidth ); x++)
        {
            UINT32 p = row[x], a = p >> 24;

            if (!a) continue;
            row[x] = ((alpha + a * inverse / 255) << 24) | ((sr + ((p >> 16) & 0xff) * inverse / 255) << 16) |
                     ((sg + ((p >> 8) & 0xff) * inverse / 255) << 8) | (sb + (p & 0xff) * inverse / 255);
        }
    }
}

static void open_link( const WCHAR *url )
{
    ShellExecuteW( NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL );
}

/* Copilot has no interface to talk to from here, and its own page takes no
 * question from the address: without a sign-in it goes to the login and the
 * text is lost. Copilot Search runs what the address carries. */
static void send_ask( struct widget_ask *ask )
{
    WCHAR *encoded, *url, lang[8], country[8], language[20];
    int size = (WIDGET_ASK_MAX + 1) * 12;

    if (!ask || !ask->text[0]) return;
    encoded = malloc( size * sizeof(WCHAR) );
    url = malloc( (size + 128) * sizeof(WCHAR) );
    if (encoded && url)
    {
        user_locale( lang, country );
        swprintf( language, ARRAY_SIZE(language), L"%s-%s", lang, country );
        CharLowerW( language );
        url_encode( ask->text, encoded, size );
        swprintf( url, size + 128, L"https://www.bing.com/copilotsearch?q=%s&setlang=%s&cc=%s", encoded, language, country );
        ask->text[0] = 0;
        open_link( url );
    }
    free( encoded );
    free( url );
}

/* what a click on a widget opens */
void widget_activate( enum widget_kind kind, const struct widget_target *t, struct widget_ask *ask )
{
    WCHAR url[1400], lang[8], country[8];

    switch (t->hit)
    {
    case HIT_STORY:
        url[0] = 0;
        AcquireSRWLockShared( &data_lock );
        if (data && t->index >= 0 && t->index < data->news_count) lstrcpynW( url, data->news[t->index].url, ARRAY_SIZE(url) );
        ReleaseSRWLockShared( &data_lock );
        if (url[0]) open_link( url );
        break;
    case HIT_CARD:
        if (kind == W_WEATHER)
        {
            user_locale( lang, country );
            swprintf( url, ARRAY_SIZE(url), L"https://www.msn.com/%s-%s/weather/forecast", lang, country );
            open_link( url );
        }
        else if (kind == W_MONITOR) ShellExecuteW( NULL, NULL, L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL );
        else if (kind == W_BATTERY) ShellExecuteW( NULL, NULL, L"control.exe", L"powercfg.cpl", NULL, SW_SHOWNORMAL );
        break;
    case HIT_SEND:
        send_ask( ask );
        break;
    default:
        break;
    }
}

/**********************************************************************
 *          The board: the widgets in two columns
 *
 * Those of the list Board, in its order, each in the column that is
 * shorter, the wide ones under both. What scrolls is painted once into a
 * bitmap of its full height and only copied while it moves; the live
 * widgets (the clock, the monitoring, the battery) are painted again in it
 * when the widgets' thread says they changed.
 */

struct layer
{
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    UINT32 *bits;
    int width, height;
};

struct card
{
    int widget;      /* in board[] */
    RECT rect;       /* in the content */
};

/* what the pointer is over */
enum spot_kind { SPOT_NONE, SPOT_CARD, SPOT_TARGET, SPOT_MORE, SPOT_ADD };

struct spot
{
    enum spot_kind kind;
    int card;
    struct widget_target target;
};

struct board_target
{
    int card;
    struct widget_target target;
};

static struct widget board[W_KINDS];
static int board_count;
static struct card cards[W_KINDS];
static int card_count, content_height, scroll;
static struct board_target targets[W_KINDS * WIDGET_TARGETS];
static int target_count;
static struct spot hot_spot, pressed_spot;
static struct layer content_layer, header_layer;
static struct fonts panel_fonts;
static BOOL fonts_ready, content_dirty, header_dirty;
static struct smooth_scroll smooth;
static struct widget_ask board_ask;
static RECT add_rect;    /* in the window */

/* a card carried by the pointer: the others make way, the order is kept */
static int drag_widget = -1;              /* in board[] */
static BOOL dragging, drag_ended;
static POINT drag_start, drag_grab;       /* the grab: where in the card it was taken */
static enum widget_kind drag_last = W_KINDS;   /* what it last changed places with */
static struct layer drag_layer;

static BOOL is_live( enum widget_kind kind )
{
    return kind == W_CLOCK || kind == W_MONITOR || kind == W_BATTERY;
}

static void layer_free( struct layer *l )
{
    if (l->dc && l->old) SelectObject( l->dc, l->old );
    if (l->bitmap) DeleteObject( l->bitmap );
    if (l->dc) DeleteDC( l->dc );
    memset( l, 0, sizeof(*l) );
}

/* without alpha: black, or white, is the acrylic seen through */
static void layer_clear( struct layer *l, const RECT *r )
{
    UINT32 back = pal.light ? 0x00ffffff : 0;

    if (!l->bits) return;
    GdiFlush();
    for (int y = max( (int)r->top, 0 ); y < min( (int)r->bottom, l->height ); y++)
        for (int x = max( (int)r->left, 0 ); x < min( (int)r->right, l->width ); x++)
            l->bits[(size_t)y * l->width + x] = back;
}

static BOOL layer_make( struct layer *l, int width, int height )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), max( width, 1 ), -max( height, 1 ), 1, 32, BI_RGB } };
    HDC screen = GetDC( NULL );
    RECT all;

    layer_free( l );
    l->dc = CreateCompatibleDC( screen );
    l->bitmap = CreateDIBSection( screen, &info, DIB_RGB_COLORS, (void **)&l->bits, NULL, 0 );
    ReleaseDC( NULL, screen );
    if (!l->dc || !l->bitmap)
    {
        layer_free( l );
        return FALSE;
    }
    l->old = SelectObject( l->dc, l->bitmap );
    l->width = max( width, 1 );
    l->height = max( height, 1 );
    SetRect( &all, 0, 0, l->width, l->height );
    layer_clear( l, &all );
    SetBkMode( l->dc, TRANSPARENT );
    return TRUE;
}

static int header_height(void)
{
    return style11 ? HEADER_11 : HEADER_10;
}

static int view_height( const RECT *client )
{
    return client->bottom - header_height();
}

static int max_scroll( const RECT *client )
{
    return max( 0, content_height - view_height( client ) );
}

static int board_width(void)
{
    int width, height;

    widget_extent( W_NEWS, W_LARGE, &width, &height );
    return width + 2 * widget_gap();
}

static void layout(void)
{
    int gap = widget_gap(), column, height, y[2] = { 0, 0 };

    widget_extent( W_WEATHER, W_SMALL, &column, &height );
    card_count = 0;
    for (int i = 0; i < board_count; i++)
    {
        int columns, rows, w, h, col, top;

        widget_cells( board[i].kind, board[i].size, &columns, &rows );
        widget_extent( board[i].kind, board[i].size, &w, &h );
        if (columns > 1)
        {
            col = 0;
            top = max( y[0], y[1] );
            y[0] = y[1] = top + h + gap;
        }
        else
        {
            col = y[1] < y[0];
            top = y[col];
            y[col] = top + h + gap;
        }
        cards[card_count].widget = i;
        SetRect( &cards[card_count].rect, gap + col * (column + gap), top, gap + col * (column + gap) + w, top + h );
        card_count++;
    }
    content_height = max( y[0], y[1] ) + px( 4 );
    content_dirty = TRUE;
}

static void paint_board_widget( HDC hdc, const RECT *rect, const struct widget *w, struct widget_target *local, int *count,
                                struct widget_ask *ask )
{
    struct widget_paint p = { 0 };

    p.hdc = hdc;
    p.pal = &pal;
    p.rect = *rect;
    p.kind = w->kind;
    p.size = w->size;
    p.style11 = style11;
    p.targets = local;
    p.ask = ask;
    if (is_live( w->kind )) widget_paint_local( &p );
    else widget_paint_net( &p );
    if (count) *count = p.target_count;
}

/* a card into the content, its targets with it; the one carried leaves
 * only its place */
static void render_card( int i )
{
    struct widget_target local[WIDGET_TARGETS];
    int kept = 0, count = 0;

    for (int j = 0; j < target_count; j++) if (targets[j].card != i) targets[kept++] = targets[j];
    target_count = kept;
    if (dragging && cards[i].widget == drag_widget)
    {
        fill_round_rect( content_layer.dc, &cards[i].rect, (float)card_radius( style11 ), pal.ink, 0x08 );
        return;
    }
    paint_board_widget( content_layer.dc, &cards[i].rect, &board[cards[i].widget], local, &count, &board_ask );
    for (int j = 0; j < count && target_count < ARRAY_SIZE(targets); j++)
    {
        targets[target_count].card = i;
        targets[target_count].target = local[j];
        target_count++;
    }
}

/* everything that scrolls, once */
static void render_content( const RECT *client )
{
    target_count = 0;
    content_dirty = FALSE;
    if (!layer_make( &content_layer, client->right, content_height )) return;
    for (int i = 0; i < card_count; i++) render_card( i );
}

/* the live widgets changed: only their cards again */
static void render_live(void)
{
    if (content_dirty || !content_layer.dc) return;
    for (int i = 0; i < card_count; i++)
    {
        if (!is_live( board[cards[i].widget].kind )) continue;
        layer_clear( &content_layer, &cards[i].rect );
        render_card( i );
    }
}

static void render_header( const RECT *client )
{
    int header = header_height(), size = px( 32 );
    WCHAR text[64];
    RECT r;

    header_dirty = FALSE;
    if (!layer_make( &header_layer, client->right, header )) return;
    if (style11)
    {
        /* the time in the middle, adding and the user at the right */
        GetTimeFormatW( LOCALE_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, text, ARRAY_SIZE(text) );
        SetRect( &r, 0, px( 18 ), client->right, px( 62 ) );
        paint_text( header_layer.dc, text, &r, panel_fonts.big, pal.text, DT_CENTER );
        avatar_draw( header_layer.dc, client->right - px( 48 ), px( 24 ), size, &pal );
        SetRect( &add_rect, client->right - px( 48 ) - px( 12 ) - size, px( 24 ), client->right - px( 48 ) - px( 12 ), px( 24 ) + size );
    }
    else
    {
        SetRect( &add_rect, client->right - widget_gap() - size, (header - size) / 2, client->right - widget_gap(),
                 (header - size) / 2 + size );
        SetRect( &r, widget_gap(), 0, client->right / 2, header );
        paint_text( header_layer.dc, load_string( IDS_WIDGETS_TITLE ), &r, panel_fonts.title, pal.text, DT_LEFT );
        GetDateFormatW( LOCALE_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, text, ARRAY_SIZE(text) );
        SetRect( &r, client->right / 2, 0, add_rect.left - px( 12 ), header );
        paint_text( header_layer.dc, text, &r, panel_fonts.small_, pal.dim, DT_RIGHT );
    }
}

/* where the content's top stands now: the list is on its way there */
static int shown_scroll( const RECT *client )
{
    return max( 0, min( scroll - smooth_scroll_offset( &smooth ), max_scroll( client ) ) );
}

static int board_index( enum widget_kind kind )
{
    for (int i = 0; i < board_count; i++) if (board[i].kind == kind) return i;
    return -1;
}

static void on_paint(void)
{
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client;
    HDC mem;
    int header, view, shown, rows, plus = px( 16 );

    GetClientRect( panel, &client );
    if (content_dirty) render_content( &client );
    if (header_dirty) render_header( &client );
    if (!(mem = paint_begin( panel, &ps, &pal, &bitmap, &old ))) return;
    header = header_height();
    view = view_height( &client );
    shown = shown_scroll( &client );
    rows = min( view, content_height - shown );
    if (header_layer.dc) BitBlt( mem, 0, 0, client.right, header, header_layer.dc, 0, 0, SRCCOPY );
    if (content_layer.dc && rows > 0) BitBlt( mem, 0, header, client.right, rows, content_layer.dc, 0, shown, SRCCOPY );

    SaveDC( mem );
    IntersectClipRect( mem, 0, header, client.right, client.bottom );
    if (hot_spot.kind == SPOT_TARGET && (hot_spot.target.hit == HIT_STORY || hot_spot.target.hit == HIT_CARD))
    {
        enum widget_kind kind = board[cards[hot_spot.card].widget].kind;
        BOOL white = kind == W_WEATHER && style11;
        RECT r = hot_spot.target.rect;

        OffsetRect( &r, 0, header - shown );
        widget_tint( mem, &r, header, white ? RGB( 255, 255, 255 ) : pal.ink, white ? 0x14 : 0x0c );
    }
    if (board_index( W_COPILOT ) >= 0)
        widget_paint_ask( mem, &pal, &board_ask, hot_spot.kind == SPOT_TARGET && hot_spot.target.hit == HIT_SEND, style11,
                          header - shown );
    if ((hot_spot.kind == SPOT_CARD || hot_spot.kind == SPOT_TARGET || hot_spot.kind == SPOT_MORE) && hot_spot.card < card_count)
    {
        enum widget_kind kind = board[cards[hot_spot.card].widget].kind;
        RECT r = cards[hot_spot.card].rect;

        OffsetRect( &r, 0, header - shown );
        widget_more_button( mem, &pal, &r, style11, hot_spot.kind == SPOT_MORE,
                            kind == W_NEWS || (kind == W_WEATHER && style11) );
    }
    RestoreDC( mem, -1 );

    /* adding */
    if (hot_spot.kind == SPOT_ADD) fill_round_rect( mem, &add_rect, (float)(style11 ? px( 4 ) : 0), pal.ink, 0x14 );
    draw_plus( mem, (add_rect.left + add_rect.right - plus) / 2, (add_rect.top + add_rect.bottom - plus) / 2, plus, pal.ink );

    /* how far down it is */
    if (content_height > view)
    {
        int track = view - px( 8 ), length = max( px( 32 ), track * view / content_height );
        int top = header + px( 4 ) + (track - length) * shown / (content_height - view);
        RECT r;

        SetRect( &r, client.right - px( 6 ), top, client.right - px( 3 ), top + length );
        fill_round_rect( mem, &r, 1.5f, pal.ink, 0x70 );
    }

    /* the card carried, under the pointer */
    if (dragging && drag_layer.dc)
    {
        POINT cursor;

        GetCursorPos( &cursor );
        ScreenToClient( panel, &cursor );
        IntersectClipRect( mem, 0, header, client.right, client.bottom );
        BitBlt( mem, cursor.x - drag_grab.x, cursor.y - drag_grab.y, drag_layer.width, drag_layer.height, drag_layer.dc, 0, 0,
                SRCCOPY );
    }
    paint_end( panel, &ps, mem, bitmap, old );
}

static void stop_typing(void);
static void update_hot(void);

/* the pointer in the content's coordinates */
static POINT content_point( POINT pt )
{
    RECT client;

    GetClientRect( panel, &client );
    pt.y += shown_scroll( &client ) - header_height();
    return pt;
}

static void drag_begin( int widget, POINT pt )
{
    POINT cp = content_point( pt );
    RECT rect = cards[widget].rect, local;

    drag_widget = widget;
    drag_grab.x = cp.x - rect.left;
    drag_grab.y = cp.y - rect.top;
    drag_last = W_KINDS;
    SetRect( &local, 0, 0, rect.right - rect.left, rect.bottom - rect.top );
    if (layer_make( &drag_layer, local.right, local.bottom ))
        paint_board_widget( drag_layer.dc, &local, &board[widget], NULL, NULL, NULL );
    dragging = TRUE;
    hot_spot.kind = SPOT_NONE;
    stop_typing();
    SetCapture( panel );
    content_dirty = TRUE;
    InvalidateRect( panel, NULL, FALSE );
}

/* the pointer over another card: the carried one takes its place */
static void drag_move( POINT pt )
{
    RECT client;
    POINT cp;
    int over = -1, edge = px( 48 );

    GetClientRect( panel, &client );
    /* at the top or the bottom: the list goes along */
    if (pt.y > client.bottom - edge) scroll = min( scroll + px( 24 ), max_scroll( &client ) );
    else if (pt.y < header_height() + edge) scroll = max( scroll - px( 24 ), 0 );
    cp = content_point( pt );
    for (int i = 0; i < card_count; i++) if (PtInRect( &cards[i].rect, cp )) over = cards[i].widget;
    if (over < 0 || over == drag_widget) drag_last = W_KINDS;
    else if (board[over].kind != drag_last)
    {
        struct widget carried = board[drag_widget];

        /* changed places with that one: not again until the pointer leaves it,
         * or two of different sizes would swap back and forth */
        drag_last = board[over].kind;
        if (over < drag_widget) memmove( &board[over + 1], &board[over], (drag_widget - over) * sizeof(*board) );
        else memmove( &board[drag_widget], &board[drag_widget + 1], (over - drag_widget) * sizeof(*board) );
        board[over] = carried;
        drag_widget = over;
        layout();
    }
    InvalidateRect( panel, NULL, FALSE );
}

static void drag_end(void)
{
    if (!dragging) return;
    dragging = FALSE;
    drag_ended = TRUE;
    drag_widget = -1;
    layer_free( &drag_layer );
    if (GetCapture() == panel) ReleaseCapture();
    widgets_save( HOST_BOARD, board, board_count );
    layout();
    update_hot();
    InvalidateRect( panel, NULL, FALSE );
}

/**********************************************************************
 *          The board: showing it
 */

static BOOL panel_visible(void)
{
    return panel && IsWindowVisible( panel );
}

BOOL widgets_board_has( enum widget_kind kind )
{
    return panel_visible() && board_index( kind ) >= 0;
}

static void stop_typing(void)
{
    board_ask.active = FALSE;
    if (panel) KillTimer( panel, TIMER_CARET );
}

void widgets_hide(void)
{
    if (!panel_visible()) return;
    KillTimer( panel, TIMER_PANEL );
    stop_typing();
    smooth_scroll_stop( &smooth, panel, TIMER_SMOOTH );
    ShowWindow( panel, SW_HIDE );
    hot_spot.kind = pressed_spot.kind = SPOT_NONE;
    panel_hover = FALSE;
    hidden_at = GetTickCount();
    if (button) InvalidateRect( button, NULL, FALSE );
}

/* the panel opens where the button is: at the left of a centred taskbar,
 * at the right when the button stands by the tray */
static void place_panel(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, y, height, avail, width = board_width();
    RECT b;

    GetWindowRect( button, &b );
    GetMonitorInfoW( MonitorFromRect( &b, MONITOR_DEFAULTTONEAREST ), &info );
    avail = info.rcWork.bottom - info.rcWork.top;
    if (style11)
    {
        int margin = px( 12 );

        x = taskbar_centered() ? info.rcWork.left + margin : info.rcWork.right - margin - width;
        y = info.rcWork.top + margin;
        height = avail - 2 * margin;
    }
    else
    {
        BOOL at_bottom = b.top >= (info.rcMonitor.top + info.rcMonitor.bottom) / 2;

        height = min( px( 720 ), avail );
        x = taskbar_centered() ? b.left : b.right - width;
        x = max( info.rcWork.left, min( x, (int)info.rcWork.right - width ) );
        y = at_bottom ? info.rcWork.bottom - height : info.rcWork.top;
    }
    SetWindowPos( panel, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE );
}

static void show_panel( BOOL activate )
{
    DWORD transition, corner;

    style11 = taskbar_style11();
    if (!style11) transition = ARCTIC_TRANSITION_SLIDE_UP;
    else transition = taskbar_centered() ? ARCTIC_TRANSITION_FROM_LEFT : ARCTIC_TRANSITION_FROM_RIGHT;
    corner = style11 ? ARCTIC_FLYOUT_CORNER : 1;  /* DWMWCP_DONOTROUND */
    DwmSetWindowAttribute( panel, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( panel, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    menu_hide();
    menu10_hide();
    menu11_hide();
    clock_hide();
    place_panel();
    look_load_taskbar( &look );
    palette_init( &pal, look_apply( panel, &look ) );
    if (fonts_ready) fonts_free( &panel_fonts );
    fonts_make( &panel_fonts );
    fonts_ready = TRUE;
    board_count = widgets_load( HOST_BOARD, board );
    layout();
    header_dirty = TRUE;
    scroll = 0;
    smooth_scroll_stop( &smooth, panel, TIMER_SMOOTH );
    panel_hover = !activate;
    hot_spot.kind = pressed_spot.kind = SPOT_NONE;
    InvalidateRect( panel, NULL, FALSE );
    ShowWindow( panel, activate ? SW_SHOW : SW_SHOWNOACTIVATE );
    if (activate)
    {
        SetForegroundWindow( panel );
        SetFocus( panel );
    }
    SetTimer( panel, TIMER_PANEL, 250, NULL );
    InvalidateRect( button, NULL, FALSE );
    if (GetTickCount() - fetched_at > 10 * 60 * 1000) start_fetch();
}

/* the lists changed (the widgets' thread posts it to the button's) */
void widgets_board_reload(void)
{
    if (button) PostMessageW( button, WM_BOARD_RELOAD, 0, 0 );
}

void widgets_board_tick( BOOL minute )
{
    if (panel) PostMessageW( panel, WM_BOARD_TICK, minute, 0 );
}

static void board_reload(void)
{
    RECT client;

    board_count = widgets_load( HOST_BOARD, board );
    if (!panel_visible()) return;
    hot_spot.kind = pressed_spot.kind = SPOT_NONE;
    layout();
    GetClientRect( panel, &client );
    scroll = min( scroll, max_scroll( &client ) );
    InvalidateRect( panel, NULL, FALSE );
}

/* a point of the window: the header's button, a card, what is in it */
static struct spot hit_test( POINT pt )
{
    struct spot spot = { SPOT_NONE, -1 };
    RECT client, more;

    if (PtInRect( &add_rect, pt ))
    {
        spot.kind = SPOT_ADD;
        return spot;
    }
    GetClientRect( panel, &client );
    if (pt.y < header_height()) return spot;
    pt.y += shown_scroll( &client ) - header_height();
    for (int i = 0; i < card_count; i++)
    {
        if (!PtInRect( &cards[i].rect, pt )) continue;
        spot.kind = SPOT_CARD;
        spot.card = i;
        widget_more_rect( &cards[i].rect, &more );
        if (PtInRect( &more, pt ))
        {
            spot.kind = SPOT_MORE;
            return spot;
        }
        /* the last that has it: the button is inside the field, and comes after it */
        for (int j = 0; j < target_count; j++)
        {
            if (targets[j].card != i || !PtInRect( &targets[j].target.rect, pt )) continue;
            spot.kind = SPOT_TARGET;
            spot.target = targets[j].target;
        }
        return spot;
    }
    return spot;
}

static BOOL same_spot( const struct spot *a, const struct spot *b )
{
    if (a->kind != b->kind || a->card != b->card) return FALSE;
    return a->kind != SPOT_TARGET || (a->target.hit == b->target.hit && a->target.index == b->target.index);
}

static void update_hot(void)
{
    POINT pt;
    struct spot spot;

    GetCursorPos( &pt );
    ScreenToClient( panel, &pt );
    spot = hit_test( pt );
    if (!same_spot( &spot, &hot_spot ))
    {
        hot_spot = spot;
        InvalidateRect( panel, NULL, FALSE );
    }
}

static void start_typing(void)
{
    board_ask.active = board_ask.caret = TRUE;
    SetTimer( panel, TIMER_CARET, 530, NULL );
    InvalidateRect( panel, NULL, FALSE );
}

/* a menu of ours, over the panel: the panel stays while it is open */
static void card_menu( int card, POINT pt )
{
    if (card < 0 || card >= card_count) return;
    in_popup = TRUE;
    widget_menu( panel, pt, HOST_BOARD, board[cards[card].widget].kind );
    in_popup = FALSE;
    if (panel_visible() && GetForegroundWindow() != panel && !panel_hover) widgets_hide();
}

static void add_menu(void)
{
    POINT pt = { add_rect.left, add_rect.bottom + px( 4 ) };

    ClientToScreen( panel, &pt );
    in_popup = TRUE;
    widget_add_menu( panel, pt, HOST_BOARD );
    in_popup = FALSE;
    if (panel_visible() && GetForegroundWindow() != panel && !panel_hover) widgets_hide();
}

static void ask_result( enum ask_result result )
{
    struct widget_target send = { HIT_SEND };

    switch (result)
    {
    case ASK_SEND:
        widget_activate( W_COPILOT, &send, &board_ask );
        widgets_hide();
        break;
    case ASK_LEAVE:
        widgets_hide();
        break;
    case ASK_CHANGED:
        start_typing();
        break;
    default:
        break;
    }
}

static LRESULT WINAPI panel_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    struct spot spot;

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (dragging)
        {
            drag_move( pt );
            return 0;
        }
        /* a card taken by its body, not by a field or "...", and moved: carried */
        if ((wparam & MK_LBUTTON) && pressed_spot.card >= 0 &&
            (pressed_spot.kind == SPOT_CARD || (pressed_spot.kind == SPOT_TARGET && pressed_spot.target.hit != HIT_ASK)) &&
            (abs( pt.x - drag_start.x ) > GetSystemMetrics( SM_CXDRAG ) ||
             abs( pt.y - drag_start.y ) > GetSystemMetrics( SM_CYDRAG )))
        {
            drag_begin( cards[pressed_spot.card].widget, drag_start );
            pressed_spot.kind = SPOT_NONE;
            drag_move( pt );
            return 0;
        }
        /* no card lights up while the list goes by under the pointer */
        if (!smooth.offset) update_hot();
        return 0;

    case WM_CAPTURECHANGED:
        if (dragging && (HWND)lparam != hwnd) drag_end();
        return 0;

    case WM_SETCURSOR:
    {
        POINT cursor;

        if (dragging)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_SIZEALL ) );
            return TRUE;
        }
        GetCursorPos( &cursor );
        ScreenToClient( hwnd, &cursor );
        spot = hit_test( cursor );
        if (spot.kind == SPOT_TARGET || spot.kind == SPOT_MORE || spot.kind == SPOT_ADD)
        {
            BOOL text = spot.kind == SPOT_TARGET && spot.target.hit == HIT_ASK;

            SetCursor( LoadCursorW( NULL, (const WCHAR *)(text ? IDC_IBEAM : IDC_HAND) ) );
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN:
        drag_ended = FALSE;
        drag_start = pt;
        pressed_spot = hit_test( pt );
        if (pressed_spot.kind == SPOT_TARGET && pressed_spot.target.hit == HIT_ASK)
        {
            SetFocus( hwnd );
            start_typing();
        }
        return 0;

    case WM_LBUTTONUP:
        if (dragging || drag_ended)
        {
            /* where it was put is all the drop does */
            drag_end();
            drag_ended = FALSE;
            pressed_spot.kind = SPOT_NONE;
            return 0;
        }
        spot = hit_test( pt );
        if (!same_spot( &spot, &pressed_spot ))
        {
            pressed_spot.kind = SPOT_NONE;
            return 0;
        }
        pressed_spot.kind = SPOT_NONE;
        if (spot.kind == SPOT_ADD) add_menu();
        else if (spot.kind == SPOT_MORE)
        {
            RECT more = cards[spot.card].rect;
            RECT client;
            POINT at;

            GetClientRect( hwnd, &client );
            OffsetRect( &more, 0, header_height() - shown_scroll( &client ) );
            widget_more_rect( &more, &more );
            at.x = more.left;
            at.y = more.bottom;
            ClientToScreen( hwnd, &at );
            card_menu( spot.card, at );
        }
        else if (spot.kind == SPOT_TARGET && spot.target.hit != HIT_ASK)
        {
            widget_activate( board[cards[spot.card].widget].kind, &spot.target, &board_ask );
            widgets_hide();
        }
        return 0;

    case WM_RBUTTONUP:
        spot = hit_test( pt );
        if (spot.card >= 0)
        {
            ClientToScreen( hwnd, &pt );
            card_menu( spot.card, pt );
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        RECT client;
        int target;

        GetClientRect( hwnd, &client );
        target = max( 0, min( scroll - (short)HIWORD( wparam ) * px( 120 ) / WHEEL_DELTA, max_scroll( &client ) ) );
        if (target != scroll)
        {
            smooth_scroll_add( &smooth, hwnd, TIMER_SMOOTH, target - scroll, view_height( &client ) );
            scroll = target;
            hot_spot.kind = SPOT_NONE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (board_ask.active) ask_result( widget_ask_input( &board_ask, msg, wparam, hwnd ) );
        else if (wparam == VK_ESCAPE) widgets_hide();
        return 0;

    case WM_CHAR:
        /* typing goes to Copilot, where there is one */
        if (!board_ask.active && board_index( W_COPILOT ) < 0) return 0;
        ask_result( widget_ask_input( &board_ask, msg, wparam, hwnd ) );
        return 0;

    case WM_BOARD_TICK:
        if (!panel_visible()) return 0;
        render_live();
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_SMOOTH)
        {
            if (!smooth_scroll_step( &smooth, hwnd, TIMER_SMOOTH )) update_hot();
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (wparam == TIMER_CARET)
        {
            board_ask.caret = !board_ask.caret;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (wparam == TIMER_PANEL)
        {
            static int away, ticks;
            POINT cursor;
            RECT p, b;

            /* what the pointer opened goes when the pointer has left it */
            if (panel_hover && !in_popup)
            {
                GetCursorPos( &cursor );
                GetWindowRect( hwnd, &p );
                GetWindowRect( button, &b );
                away = PtInRect( &p, cursor ) || PtInRect( &b, cursor ) ? 0 : away + 1;
                if (away >= 3)
                {
                    away = 0;
                    widgets_hide();
                    return 0;
                }
            }
            /* the time at the top */
            if (style11 && ++ticks >= 40)
            {
                ticks = 0;
                header_dirty = TRUE;
                InvalidateRect( hwnd, NULL, FALSE );
            }
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !panel_hover && !in_popup) widgets_hide();
        else if (LOWORD( wparam ) != WA_INACTIVE) panel_hover = FALSE;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_panel(void)
{
    WNDCLASSW cls = { 0 };

    if (panel) return TRUE;
    cls.lpfnWndProc = panel_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticWidgetsPanel";
    RegisterClassW( &cls );
    panel = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP, 0, 0, board_width(), px( 400 ),
                             NULL, NULL, startui_instance, NULL );
    return panel != NULL;
}


/**********************************************************************
 *          The town the user sets
 */

#define ID_CITY 100

static void store_town( const WCHAR *city, double lat, double lon )
{
    WCHAR number[32];

    set_string( L"City", city );
    swprintf( number, ARRAY_SIZE(number), L"%.4f", lat );
    set_string( L"Latitude", number );
    swprintf( number, ARRAY_SIZE(number), L"%.4f", lon );
    set_string( L"Longitude", number );
}

static INT_PTR WINAPI city_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    WCHAR text[64], city[64], lang[8], country[8];
    double lat, lon;

    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowTextW( dialog, load_string( IDS_WIDGET_LOCATION ) );
        add_dialog_control( dialog, L"STATIC", load_string( IDS_WIDGET_CITY ), SS_CENTERIMAGE, 7, 7, 40, 14, -1 );
        add_dialog_control( dialog, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 50, 7, 143, 14, ID_CITY );
        add_dialog_control( dialog, L"BUTTON", load_string( IDS_OK ), BS_DEFPUSHBUTTON | WS_TABSTOP, 89, 28, 50, 14, IDOK );
        add_dialog_control( dialog, L"BUTTON", load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, 143, 28, 50, 14, IDCANCEL );
        if (get_string( L"City", text, ARRAY_SIZE(text) )) SetDlgItemTextW( dialog, ID_CITY, text );
        SetFocus( GetDlgItem( dialog, ID_CITY ) );
        return FALSE;

    case WM_COMMAND:
        if (LOWORD( wparam ) == IDCANCEL)
        {
            EndDialog( dialog, 0 );
            return TRUE;
        }
        if (LOWORD( wparam ) == IDOK)
        {
            GetDlgItemTextW( dialog, ID_CITY, text, ARRAY_SIZE(text) );
            if (!text[0])
            {
                /* nothing: the town of this address again */
                set_string( L"City", NULL );
                set_string( L"Latitude", NULL );
                set_string( L"Longitude", NULL );
                EndDialog( dialog, 1 );
                return TRUE;
            }
            user_locale( lang, country );
            lstrcpynW( city, text, ARRAY_SIZE(city) );
            if (find_town( text, lang, city, ARRAY_SIZE(city), &lat, &lon ))
            {
                store_town( city, lat, lon );
                EndDialog( dialog, 1 );
            }
            else MessageBoxW( dialog, load_string( IDS_WIDGET_NOT_FOUND ), load_string( IDS_WIDGET_LOCATION ), MB_OK | MB_ICONINFORMATION );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void widgets_change_location( HWND owner )
{
    static const struct page_template template =
    {
        { DS_SETFONT | DS_MODALFRAME | DS_SETFOREGROUND | DS_CENTER | WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
          0, 0, 0, 0, 200, 49 },
        0, 0, 0, 8, L"MS Shell Dlg",
    };

    if (DialogBoxIndirectParamW( startui_instance, &template.header, owner ? owner : tray, city_proc, 0 ) == 1) start_fetch();
}

void widgets_refresh(void)
{
    start_fetch();
}

/**********************************************************************
 *          The button's window
 */

#define ID_SHOW_BOTH  1
#define ID_SHOW_ICON  2
#define ID_SHOW_OFF   3
#define ID_HOVER      4
#define ID_REFRESH    5
#define ID_LOCATION   6

static void changed_width(void)
{
    PostMessageW( tray, RegisterWindowMessageW( L"ArcticWidgetChanged" ), 0, 0 );
}

static void button_menu( POINT pt )
{
    DWORD show = ArcticWidgetShow();
    BOOL hover = get_dword( L"Hover", 1 ) != 0;
    struct look menu_look;
    const struct popup_item items[] =
    {
        { ID_SHOW_BOTH, load_string( IDS_WIDGET_BOTH ), GLYPH_NONE, show == 2 },
        { ID_SHOW_ICON, load_string( IDS_WIDGET_ICON ), GLYPH_NONE, show == 1 },
        { ID_SHOW_OFF, load_string( IDS_WIDGET_OFF ), GLYPH_NONE, FALSE },
        { 0 },
        { ID_HOVER, load_string( IDS_WIDGET_HOVER ), GLYPH_NONE, hover },
        { ID_LOCATION, load_string( IDS_WIDGET_LOCATION ), GLYPH_NONE },
        { ID_REFRESH, load_string( IDS_WIDGET_REFRESH ), GLYPH_NONE },
    };
    UINT chosen;

    widgets_hide();
    look_load_taskbar( &menu_look );
    chosen = popup_track( GetAncestor( button, GA_ROOT ), pt, TPM_LEFTALIGN | TPM_BOTTOMALIGN, items, ARRAY_SIZE(items),
                          (taskbar_style11() ? POPUP_WIN11 : POPUP_WIN10) | POPUP_TASKBAR, &menu_look );
    switch (chosen)
    {
    case ID_SHOW_BOTH: ArcticWidgetSetShow( 2 ); changed_width(); break;
    case ID_SHOW_ICON: ArcticWidgetSetShow( 1 ); changed_width(); break;
    case ID_SHOW_OFF:  ArcticWidgetSetShow( 0 ); changed_width(); break;
    case ID_HOVER:     set_dword( L"Hover", !hover ); break;
    case ID_REFRESH:   start_fetch(); break;
    case ID_LOCATION:  widgets_change_location( NULL ); break;
    }
}

static void remember( const struct data *d )
{
    set_dword( L"LastTemp", d->temp + 1000 );
    set_dword( L"LastCode", d->code );
    set_dword( L"LastDay", d->is_day );
    set_dword( L"LastImperial", d->imperial );
    set_string( L"LastCity", d->city );
}

static void recall( struct data *d )
{
    DWORD temp = get_dword( L"LastTemp", 0 );

    if (!temp) return;
    d->have_weather = TRUE;
    d->temp = (int)temp - 1000;
    d->feels = d->temp;
    d->code = get_dword( L"LastCode", 3 );
    d->is_day = get_dword( L"LastDay", 1 ) != 0;
    d->imperial = get_dword( L"LastImperial", 0 ) != 0;
    get_string( L"LastCity", d->city, ARRAY_SIZE(d->city) );
}

/* what came from the network: what did not come stays as it was */
static void take_data( struct data *d )
{
    struct data *old = data;

    if (old && !d->have_weather)
    {
        d->have_weather = old->have_weather;
        lstrcpyW( d->city, old->city );
        d->temp = old->temp;
        d->feels = old->feels;
        d->humidity = old->humidity;
        d->wind = old->wind;
        d->code = old->code;
        d->is_day = old->is_day;
        d->imperial = old->imperial;
        d->day_count = old->day_count;
        memcpy( d->days, old->days, sizeof(d->days) );
        SetTimer( button, TIMER_RETRY, 60 * 1000, NULL );
    }
    else remember( d );
    if (old && !d->news_count)
    {
        d->news_count = old->news_count;
        memcpy( d->news, old->news, sizeof(d->news) );
        memset( old->news, 0, sizeof(old->news) );
    }
    if (d->have_weather && d->news_count) fetched_at = GetTickCount();
    /* the desktop's thread may be drawing the old */
    AcquireSRWLockExclusive( &data_lock );
    data = d;
    ReleaseSRWLockExclusive( &data_lock );
    data_free( old );
    changed_width();
    InvalidateRect( button, NULL, FALSE );
    widgets_desktop_data();
    if (panel_visible())
    {
        hot_spot.kind = SPOT_NONE;
        content_dirty = TRUE;
        InvalidateRect( panel, NULL, FALSE );
    }
}

static LRESULT WINAPI button_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!button_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            if (get_dword( L"Hover", 1 ))
            {
                tme.dwFlags |= TME_HOVER;
                tme.dwHoverTime = 350;
            }
            TrackMouseEvent( &tme );
            button_hot = TRUE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSELEAVE:
        button_hot = button_down = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_MOUSEHOVER:
        if (!panel_visible() && create_panel() && !(GetKeyState( VK_LBUTTON ) & 0x8000)) show_panel( FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        button_down = TRUE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        if (!button_down) return 0;
        button_down = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        if (!create_panel()) return 0;
        if (panel_visible())
        {
            /* what the pointer opened stays once it is clicked */
            if (panel_hover)
            {
                panel_hover = FALSE;
                SetForegroundWindow( panel );
                SetFocus( panel );
            }
            else widgets_hide();
        }
        /* the click that took the focus away and closed it */
        else if (GetTickCount() - hidden_at >= 300) show_panel( TRUE );
        return 0;

    case WM_RBUTTONUP:
    {
        POINT pt;

        GetCursorPos( &pt );
        button_menu( pt );
        return 0;
    }

    case WM_TIMER:
        if (wparam == TIMER_FIRST)
        {
            KillTimer( hwnd, TIMER_FIRST );
            SetTimer( hwnd, TIMER_REFRESH, 30 * 60 * 1000, NULL );
            start_fetch();
        }
        else if (wparam == TIMER_RETRY)
        {
            KillTimer( hwnd, TIMER_RETRY );
            start_fetch();
        }
        else if (wparam == TIMER_REFRESH) start_fetch();
        return 0;

    case WM_WIDGET_DATA:
        take_data( (struct data *)lparam );
        return 0;

    case WM_BOARD_RELOAD:
        board_reload();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_button();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* for explorer: the button in the taskbar, which it places */
HWND WINAPI ArcticWidgetCreate( HWND tray_window )
{
    WNDCLASSW cls = { 0 };

    if (button) return button;
    tray = tray_window;
    cls.lpfnWndProc = button_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticWidgetButton";
    RegisterClassW( &cls );
    if ((data = calloc( 1, sizeof(*data) ))) recall( data );
    board_count = widgets_load( HOST_BOARD, board );
    button = CreateWindowExW( 0, cls.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, tray, NULL,
                              startui_instance, NULL );
    if (button) SetTimer( button, TIMER_FIRST, 4000, NULL );
    return button;
}
