/*
 * The weather on the taskbar and what it opens: Windows 10's "News and
 * interests" or Windows 11's widgets, as the taskbar's buttons are set
 *
 * A button in the taskbar shows the weather: explorer puts it at the far left
 * of a centred taskbar and next to the tray otherwise (traywnd.cpp,
 * ArcticPlaceWidget). It opens a panel where it stands: the weather on top,
 * Copilot under it, the news below; from the taskbar's edge in Windows 10's
 * style, with a margin and rounded corners in Windows 11's.
 *
 * Where the data comes from, every half hour and when the panel opens:
 * ipwho.is (the town of this address, unless the user set one),
 * geocoding-api.open-meteo.com (its name in the user's language),
 * api.open-meteo.com (the weather), www.bing.com/news (the headlines and
 * their pictures, in the user's language and country). A link, and what is
 * asked of Copilot, open in the default browser.
 *
 * HKCU\Software\Arctic\Widgets: Show (0 off, 1 the icon, 2 the icon and the
 * text), Hover (the panel opens under the pointer), City, Latitude, Longitude
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

#define TIMER_REFRESH   1
#define TIMER_FIRST     2
#define TIMER_RETRY     3
#define TIMER_PANEL     4
#define TIMER_SMOOTH    5
#define TIMER_CARET     6

#define MAX_NEWS        12
#define MAX_DAYS        5
#define MAX_CARDS       (MAX_NEWS + 2)
#define MAX_TARGETS     32
#define MAX_ASK         500

#define COL_W           px(360)
#define GAP             px(16)
#define PANEL_W         (2 * COL_W + 3 * GAP)
#define IMAGE_H         (COL_W * 9 / 16)
#define WEATHER_H       px(132)
#define COPILOT_H       px(96)
#define HEADER_10       px(52)
#define HEADER_11       px(76)

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
    UINT32 *pixels;     /* premultiplied, COL_W by IMAGE_H, or NULL */
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
    struct news news[MAX_NEWS];
    int news_count;
};

enum kind { K_NONE, K_WEATHER, K_NEWS, K_ASK, K_SEND };
enum card_kind { CARD_WEATHER, CARD_COPILOT, CARD_NEWS };

struct card
{
    enum card_kind kind;
    int index, column, y, height;   /* column -1: across the whole width */
};

struct target
{
    enum kind kind;
    int index;
    RECT rect;
};

struct fonts
{
    HFONT title, body, small_, big, huge_;
};

static HWND button, tray, panel;
static struct data *data;
static BOOL button_hot, button_down, panel_hover, style11;
static DWORD hidden_at, fetched_at;
static LONG fetching;

static struct palette pal;
static struct look look;
static struct card cards[MAX_CARDS];
static int card_count, content_height, scroll;
static struct target targets[MAX_TARGETS], hot_target, pressed_target;
static int target_count;

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

/* a picture, fit to fill the width and height, its middle kept */
static BOOL decode_image( const BYTE *bytes, DWORD size, struct news *n, int width, int height )
{
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
    fit = max( (double)width / src_w, (double)height / src_h );
    scaled_w = max( (UINT)width, (UINT)ceil( src_w * fit ) );
    scaled_h = max( (UINT)height, (UINT)ceil( src_h * fit ) );
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
    int width = COL_W, height = IMAGE_H;
    WCHAR url[1000];
    DWORD size = 0;
    char *body;

    /* bing's own resizer, and over https */
    swprintf( url, ARRAY_SIZE(url), L"https:%s&w=%d&h=%d&c=7", !wcsncmp( address, L"http:", 5 ) ? address + 5 :
              !wcsncmp( address, L"https:", 6 ) ? address + 6 : address, width, height );
    if (!(body = http_get( url, &size ))) return;
    decode_image( (const BYTE *)body, size, n, width, height );
    free( body );
}

static void fetch_news( struct data *d, const WCHAR *lang, const WCHAR *country )
{
    WCHAR url[512], encoded[128], query[64], language[20], image[900];
    const char *p, *end;
    char *body;

    lstrcpynW( query, load_string( IDS_WIDGET_QUERY ), ARRAY_SIZE(query) );
    url_encode( query, encoded, ARRAY_SIZE(encoded) );
    swprintf( language, ARRAY_SIZE(language), L"%s-%s", lang, country );
    CharLowerW( language );
    swprintf( url, ARRAY_SIZE(url), L"https://www.bing.com/news/search?q=%s&format=rss&setlang=%s&cc=%s", encoded, language, country );
    if (!(body = http_get( url, NULL ))) return;
    for (p = body; d->news_count < MAX_NEWS && (p = strstr( p, "<item>" )); p = end)
    {
        struct news *n = &d->news[d->news_count];

        if (!(end = strstr( p, "</item>" ))) break;
        if (!xml_text( p, end, "title", n->title, ARRAY_SIZE(n->title) ) ||
            !xml_text( p, end, "link", n->url, ARRAY_SIZE(n->url) ))
            continue;
        xml_text( p, end, "News:Source", n->source, ARRAY_SIZE(n->source) );
        if (xml_text( p, end, "News:Image", image, ARRAY_SIZE(image) )) fetch_image( n, image );
        d->news_count++;
    }
    free( body );
}

static void data_free( struct data *d )
{
    if (!d) return;
    for (int i = 0; i < MAX_NEWS; i++) free( d->news[i].pixels );
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
}

static void fonts_free( struct fonts *f )
{
    DeleteObject( f->title );
    DeleteObject( f->body );
    DeleteObject( f->small_ );
    DeleteObject( f->big );
    DeleteObject( f->huge_ );
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
 *          The panel: where everything goes
 *
 * The weather and Copilot across the whole width, the news under them in
 * two columns. What scrolls is painted once into a bitmap of its full
 * height and only copied while it moves.
 */

struct layer
{
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
};

static struct layer content_layer, header_layer;
static struct fonts panel_fonts;
static BOOL fonts_ready, content_dirty, header_dirty, ask_active, caret_on;
static struct smooth_scroll smooth;
static WCHAR ask[MAX_ASK + 1];
static COLORREF panel_accent;
static RECT ask_rect, send_rect;   /* in the content's coordinates */

static void layer_free( struct layer *l )
{
    if (l->dc && l->old) SelectObject( l->dc, l->old );
    if (l->bitmap) DeleteObject( l->bitmap );
    if (l->dc) DeleteDC( l->dc );
    memset( l, 0, sizeof(*l) );
}

static BOOL layer_make( struct layer *l, int width, int height )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), max( width, 1 ), -max( height, 1 ), 1, 32, BI_RGB } };
    size_t count = (size_t)max( width, 1 ) * max( height, 1 );
    HDC screen = GetDC( NULL );
    UINT32 *bits = NULL;

    layer_free( l );
    l->dc = CreateCompatibleDC( screen );
    l->bitmap = CreateDIBSection( screen, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    ReleaseDC( NULL, screen );
    if (!l->dc || !l->bitmap)
    {
        layer_free( l );
        return FALSE;
    }
    l->old = SelectObject( l->dc, l->bitmap );
    /* without alpha: black, or white, is the acrylic seen through */
    for (size_t i = 0; i < count; i++) bits[i] = pal.light ? 0x00ffffff : 0;
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

static int news_height( HDC hdc, const struct fonts *f, const struct news *n )
{
    int width = COL_W - 2 * px( 12 ), height = n->pixels ? n->height : 0;

    height += px( 10 ) + text_height( hdc, f->title, n->title, width, 3 );
    return height + px( 6 ) + px( 16 ) + px( 12 );
}

static void add_card( enum card_kind kind, int index, int column, int y, int height )
{
    if (card_count >= MAX_CARDS) return;
    cards[card_count].kind = kind;
    cards[card_count].index = index;
    cards[card_count].column = column;
    cards[card_count].y = y;
    cards[card_count].height = height;
    card_count++;
}

static void layout(void)
{
    int top = WEATHER_H + GAP + COPILOT_H + GAP, columns[2] = { top, top };
    HDC hdc;

    card_count = 0;
    if (!data || !(hdc = GetDC( panel ))) return;
    add_card( CARD_WEATHER, 0, -1, 0, WEATHER_H );
    add_card( CARD_COPILOT, 0, -1, WEATHER_H + GAP, COPILOT_H );
    for (int i = 0; i < data->news_count; i++)
    {
        int column = columns[1] < columns[0], height = news_height( hdc, &panel_fonts, &data->news[i] );

        add_card( CARD_NEWS, i, column, columns[column], height );
        columns[column] += height + GAP;
    }
    ReleaseDC( panel, hdc );
    content_height = max( columns[0], columns[1] ) + px( 4 );
    content_dirty = TRUE;
}

/**********************************************************************
 *          The panel: painting
 */

static void add_target( enum kind kind, int index, const RECT *rect )
{
    if (target_count >= MAX_TARGETS) return;
    targets[target_count].kind = kind;
    targets[target_count].index = index;
    targets[target_count].rect = *rect;
    target_count++;
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

/* a picture on top of a card: only the card's upper corners are rounded */
static void draw_picture( HDC hdc, const struct news *n, int x, int y, int radius )
{
    UINT32 *pixels = malloc( n->width * n->height * sizeof(*pixels) );

    if (!pixels) return;
    memcpy( pixels, n->pixels, n->width * n->height * sizeof(*pixels) );
    for (int py = 0; py < min( radius, n->height ); py++)
    {
        for (int px_ = 0; px_ < n->width; px_++)
        {
            UINT32 a = corner_coverage( px_, py, n->width, n->height, radius, FALSE ), p = pixels[py * n->width + px_];

            if (a < 255)
                pixels[py * n->width + px_] = (((p >> 24) * a / 255) << 24) | ((((p >> 16) & 0xff) * a / 255) << 16) |
                                              ((((p >> 8) & 0xff) * a / 255) << 8) | ((p & 0xff) * a / 255);
        }
    }
    blend_bits( hdc, x, y, n->width, n->height, pixels );
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

static int card_radius(void)
{
    return style11 ? px( 8 ) : 0;
}

static BYTE card_alpha(void)
{
    return pal.light ? 0x0a : 0x10;
}

/* one tile: the place and the weather now at the left, the days at the right */
static void paint_weather( HDC hdc, const struct fonts *f, const RECT *r )
{
    int radius = card_radius(), pad = px( 16 ), x = r->left + pad, y = r->top + px( 12 ), icon = px( 64 );
    int split = (r->left + r->right) / 2, detail = x + icon + px( 108 );
    BOOL opaque = style11;
    COLORREF text = opaque ? RGB( 255, 255, 255 ) : pal.text, dim = opaque ? RGB( 0xe4, 0xee, 0xf8 ) : pal.dim;
    WCHAR line[64];
    RECT t;

    if (!data->have_weather)
    {
        fill_round_rect( hdc, r, (float)radius, pal.ink, card_alpha() );
        t = *r;
        paint_text( hdc, load_string( IDS_WIDGET_NO_DATA ), &t, f->body, pal.dim, DT_CENTER );
        return;
    }
    if (opaque)
    {
        COLORREF top, bottom;

        weather_colors( &top, &bottom );
        gradient_card( hdc, r, radius, top, bottom );
    }
    else fill_round_rect( hdc, r, (float)radius, pal.ink, card_alpha() );

    SetRect( &t, x, y, split, y + px( 20 ) );
    card_text( hdc, opaque, data->city, &t, f->title, text, DT_LEFT );
    y += px( 26 );

    draw_weather( hdc, data->code, data->is_day, x, y, icon, !opaque && pal.light );
    degrees( line, ARRAY_SIZE(line), data->temp, FALSE );
    SetRect( &t, x + icon + px( 8 ), y, detail - px( 4 ), y + icon );
    card_text( hdc, opaque, line, &t, f->huge_, text, DT_LEFT );

    SetRect( &t, detail, y - px( 2 ), split, y + px( 20 ) );
    card_text( hdc, opaque, weather_text( data->code, data->is_day ), &t, f->body, text, DT_LEFT );
    swprintf( line, ARRAY_SIZE(line), load_string( IDS_WIDGET_FEELS ), data->feels );
    SetRect( &t, detail, y + px( 20 ), split, y + px( 36 ) );
    card_text( hdc, opaque, line, &t, f->small_, dim, DT_LEFT );
    swprintf( line, ARRAY_SIZE(line), load_string( IDS_WIDGET_HUMIDITY ), data->humidity );
    SetRect( &t, detail, y + px( 36 ), split, y + px( 52 ) );
    card_text( hdc, opaque, line, &t, f->small_, dim, DT_LEFT );
    swprintf( line, ARRAY_SIZE(line), load_string( data->imperial ? IDS_WIDGET_WIND_MPH : IDS_WIDGET_WIND ), data->wind );
    SetRect( &t, detail, y + px( 52 ), split, y + px( 68 ) );
    card_text( hdc, opaque, line, &t, f->small_, dim, DT_LEFT );

    /* the days */
    if (data->day_count)
    {
        int left = split + px( 8 ), cell = (r->right - pad - left) / data->day_count, mini = px( 32 ), top = r->top + px( 10 );

        for (int i = 0; i < data->day_count; i++)
        {
            int cx = left + i * cell;

            SetRect( &t, cx, top, cx + cell, top + px( 18 ) );
            card_text( hdc, opaque, data->days[i].name, &t, f->small_, dim, DT_CENTER );
            draw_weather( hdc, data->days[i].code, TRUE, cx + (cell - mini) / 2, top + px( 20 ), mini, !opaque && pal.light );
            swprintf( line, ARRAY_SIZE(line), L"%d\x00b0 / %d\x00b0", data->days[i].high, data->days[i].low );
            SetRect( &t, cx, top + px( 54 ), cx + cell, top + px( 72 ) );
            card_text( hdc, opaque, line, &t, f->small_, text, DT_CENTER );
        }
    }
    SetRect( &t, split, r->bottom - px( 26 ), r->right - pad, r->bottom - px( 8 ) );
    card_text( hdc, opaque, load_string( IDS_WIDGET_FORECAST ), &t, f->small_, dim, DT_RIGHT );
}

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

/* the tile with its field; what is typed is drawn over it, frame by frame */
static void paint_copilot( HDC hdc, const struct fonts *f, const RECT *r )
{
    int radius = card_radius(), pad = px( 16 ), mark = px( 22 ), y = r->top + px( 11 ), side = px( 32 ), inset = px( 4 );
    RECT t;

    fill_round_rect( hdc, r, (float)radius, pal.ink, card_alpha() );
    draw_sparkle( hdc, r->left + pad, y, mark );
    SetRect( &t, r->left + pad + mark + px( 8 ), y, r->right - pad, y + mark );
    paint_text( hdc, L"Copilot", &t, f->title, pal.text, DT_LEFT );

    SetRect( &ask_rect, r->left + pad, r->top + px( 42 ), r->right - pad, r->top + px( 82 ) );
    fill_round_rect( hdc, &ask_rect, (float)(style11 ? px( 6 ) : 0), pal.ink, pal.light ? 0x0d : 0x14 );
    add_target( K_ASK, 0, &ask_rect );

    SetRect( &send_rect, ask_rect.right - inset - side, ask_rect.top + inset, ask_rect.right - inset, ask_rect.bottom - inset );
    add_target( K_SEND, 0, &send_rect );
}

static void paint_news( HDC hdc, const struct fonts *f, const struct news *n, const RECT *r )
{
    int radius = card_radius(), y = r->top, pad = px( 12 ), title;
    RECT t;

    fill_round_rect( hdc, r, (float)radius, pal.ink, card_alpha() );
    if (n->pixels)
    {
        draw_picture( hdc, n, r->left, y, radius );
        y += n->height;
    }
    y += px( 10 );
    title = text_height( hdc, f->title, n->title, r->right - r->left - 2 * pad, 3 );
    SetRect( &t, r->left + pad, y, r->right - pad, y + title );
    paint_text( hdc, n->title, &t, f->title, pal.text, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS );
    y += title + px( 6 );
    SetRect( &t, r->left + pad, y, r->right - pad, y + px( 16 ) );
    paint_text( hdc, n->source, &t, f->small_, pal.dim, DT_LEFT );
}

/* everything that scrolls, once */
static void render_content( const RECT *client )
{
    target_count = 0;
    content_dirty = FALSE;
    if (!layer_make( &content_layer, client->right, content_height )) return;
    for (int i = 0; i < card_count; i++)
    {
        const struct card *c = &cards[i];
        int x = c->column < 0 ? GAP : GAP + c->column * (COL_W + GAP), width = c->column < 0 ? PANEL_W - 2 * GAP : COL_W;
        RECT r;

        SetRect( &r, x, c->y, x + width, c->y + c->height );
        switch (c->kind)
        {
        case CARD_WEATHER:
            add_target( K_WEATHER, 0, &r );
            paint_weather( content_layer.dc, &panel_fonts, &r );
            break;
        case CARD_COPILOT:
            paint_copilot( content_layer.dc, &panel_fonts, &r );
            break;
        case CARD_NEWS:
            add_target( K_NEWS, c->index, &r );
            paint_news( content_layer.dc, &panel_fonts, &data->news[c->index], &r );
            break;
        }
    }
}

static void render_header( const RECT *client )
{
    int header = header_height();
    WCHAR text[64];
    RECT r;

    header_dirty = FALSE;
    if (!layer_make( &header_layer, client->right, header )) return;
    if (style11)
    {
        /* the time in the middle, the user at the right */
        GetTimeFormatW( LOCALE_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, text, ARRAY_SIZE(text) );
        SetRect( &r, 0, px( 18 ), client->right, px( 62 ) );
        paint_text( header_layer.dc, text, &r, panel_fonts.big, pal.text, DT_CENTER );
        avatar_draw( header_layer.dc, client->right - px( 48 ), px( 24 ), px( 32 ), &pal );
    }
    else
    {
        SetRect( &r, GAP, 0, client->right / 2, header );
        paint_text( header_layer.dc, load_string( IDS_WIDGETS_TITLE ), &r, panel_fonts.title, pal.text, DT_LEFT );
        GetDateFormatW( LOCALE_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, text, ARRAY_SIZE(text) );
        SetRect( &r, client->right / 2, 0, client->right - GAP, header );
        paint_text( header_layer.dc, text, &r, panel_fonts.small_, pal.dim, DT_RIGHT );
    }
}

/* where the content's top stands now: the list is on its way there */
static int shown_scroll( const RECT *client )
{
    return max( 0, min( scroll - smooth_scroll_offset( &smooth ), max_scroll( client ) ) );
}

static void paint_ask( HDC hdc, int shift )
{
    const WCHAR *shown = ask;
    RECT field = ask_rect, send = send_rect, t;
    int width, arrow = px( 18 );

    OffsetRect( &field, 0, shift );
    OffsetRect( &send, 0, shift );
    if (ask[0]) fill_round_rect( hdc, &send, (float)(style11 ? px( 4 ) : 0), panel_accent, 255 );
    else if (hot_target.kind == K_SEND) fill_round_rect( hdc, &send, (float)(style11 ? px( 4 ) : 0), pal.ink, 0x14 );
    draw_arrow( hdc, (send.left + send.right - arrow) / 2, (send.top + send.bottom - arrow) / 2, arrow,
                ask[0] ? RGB( 255, 255, 255 ) : pal.dim );
    SetRect( &t, field.left + px( 12 ), field.top, field.right - px( 44 ), field.bottom );
    width = t.right - t.left;
    if (!ask[0]) paint_text( hdc, load_string( IDS_WIDGET_ASK ), &t, panel_fonts.body, pal.dim, DT_LEFT );
    else
    {
        /* the end of a long question: that is where the typing is */
        while (*shown && text_width( hdc, panel_fonts.body, shown ) > width) shown++;
        paint_text( hdc, shown, &t, panel_fonts.body, pal.text, DT_LEFT );
    }
    if (ask_active && caret_on)
    {
        int x = t.left + (ask[0] ? text_width( hdc, panel_fonts.body, shown ) + 1 : 0);
        RECT caret;

        SetRect( &caret, x, field.top + px( 10 ), x + 1, field.bottom - px( 10 ) );
        fill_alpha( hdc, &caret, pal.ink, 255 );
    }
}

/* a card under the pointer, a little lighter: only what has alpha, the card
 * itself and its picture. GDI's text has none, dwm.exe keys it by brightness,
 * and alpha laid over it would turn it into a faint shadow. */
static void tint_card( HDC hdc, const RECT *r, int top, COLORREF color, BYTE alpha )
{
    UINT32 sr = GetRValue( color ) * alpha / 255, sg = GetGValue( color ) * alpha / 255, sb = GetBValue( color ) * alpha / 255;
    UINT32 inverse = 255 - alpha;
    DIBSECTION dib;
    HGDIOBJ bitmap = GetCurrentObject( hdc, OBJ_BITMAP );

    GdiFlush();
    if (!bitmap || GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !dib.dsBm.bmBits || dib.dsBm.bmBitsPixel != 32) return;
    for (int y = max( r->top, top ); y < min( r->bottom, (int)dib.dsBm.bmHeight ); y++)
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

static void on_paint(void)
{
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client;
    HDC mem;
    int header, view, shown, rows;

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
    if (hot_target.kind == K_WEATHER || hot_target.kind == K_NEWS)
    {
        RECT r = hot_target.rect;
        BOOL white = hot_target.kind == K_WEATHER && style11;

        OffsetRect( &r, 0, header - shown );
        tint_card( mem, &r, header, white ? RGB( 255, 255, 255 ) : pal.ink, white ? 0x14 : 0x0c );
    }
    paint_ask( mem, header - shown );
    RestoreDC( mem, -1 );

    /* how far down it is */
    if (content_height > view)
    {
        int track = view - px( 8 ), length = max( px( 32 ), track * view / content_height );
        int top = header + px( 4 ) + (track - length) * shown / (content_height - view);
        RECT r;

        SetRect( &r, client.right - px( 6 ), top, client.right - px( 3 ), top + length );
        fill_round_rect( mem, &r, 1.5f, pal.ink, 0x70 );
    }
    paint_end( panel, &ps, mem, bitmap, old );
}

/**********************************************************************
 *          The panel: showing it
 */

static BOOL panel_visible(void)
{
    return panel && IsWindowVisible( panel );
}

void widgets_hide(void)
{
    if (!panel_visible()) return;
    KillTimer( panel, TIMER_PANEL );
    KillTimer( panel, TIMER_CARET );
    smooth_scroll_stop( &smooth, panel, TIMER_SMOOTH );
    ShowWindow( panel, SW_HIDE );
    hot_target.kind = pressed_target.kind = K_NONE;
    panel_hover = ask_active = FALSE;
    hidden_at = GetTickCount();
    if (button) InvalidateRect( button, NULL, FALSE );
}

/* the panel opens where the button is: at the left of a centred taskbar,
 * at the right when the button stands by the tray */
static void place_panel(void)
{
    MONITORINFO info = { sizeof(info) };
    RECT b;
    int x, y, height, avail;

    GetWindowRect( button, &b );
    GetMonitorInfoW( MonitorFromRect( &b, MONITOR_DEFAULTTONEAREST ), &info );
    avail = info.rcWork.bottom - info.rcWork.top;
    if (style11)
    {
        int margin = px( 12 );

        x = taskbar_centered() ? info.rcWork.left + margin : info.rcWork.right - margin - PANEL_W;
        y = info.rcWork.top + margin;
        height = avail - 2 * margin;
    }
    else
    {
        BOOL at_bottom = b.top >= (info.rcMonitor.top + info.rcMonitor.bottom) / 2;

        height = min( px( 720 ), avail );
        x = taskbar_centered() ? b.left : b.right - PANEL_W;
        x = max( info.rcWork.left, min( x, (int)info.rcWork.right - PANEL_W ) );
        y = at_bottom ? info.rcWork.bottom - height : info.rcWork.top;
    }
    SetWindowPos( panel, HWND_TOPMOST, x, y, PANEL_W, height, SWP_NOACTIVATE );
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
    panel_accent = accent_color();
    if (fonts_ready) fonts_free( &panel_fonts );
    fonts_make( &panel_fonts );
    fonts_ready = TRUE;
    layout();
    header_dirty = TRUE;
    scroll = 0;
    smooth_scroll_stop( &smooth, panel, TIMER_SMOOTH );
    panel_hover = !activate;
    hot_target.kind = pressed_target.kind = K_NONE;
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

static void open_link( const WCHAR *url )
{
    widgets_hide();
    ShellExecuteW( NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL );
}

/* Copilot has no interface to talk to from here, and its own page takes no
 * question from the address: without a sign-in it goes to the login and the
 * text is lost. Copilot Search runs what the address carries. */
static void send_ask(void)
{
    WCHAR *encoded, *url, lang[8], country[8], language[20];
    int size = (MAX_ASK + 1) * 12;

    if (!ask[0]) return;
    encoded = malloc( size * sizeof(WCHAR) );
    url = malloc( (size + 128) * sizeof(WCHAR) );
    if (encoded && url)
    {
        user_locale( lang, country );
        swprintf( language, ARRAY_SIZE(language), L"%s-%s", lang, country );
        CharLowerW( language );
        url_encode( ask, encoded, size );
        swprintf( url, size + 128, L"https://www.bing.com/copilotsearch?q=%s&setlang=%s&cc=%s", encoded, language, country );
        ask[0] = 0;
        open_link( url );
    }
    free( encoded );
    free( url );
}

static void activate_target( const struct target *t )
{
    WCHAR url[128], lang[8], country[8];

    switch (t->kind)
    {
    case K_NEWS:
        open_link( data->news[t->index].url );
        break;
    case K_WEATHER:
        user_locale( lang, country );
        swprintf( url, ARRAY_SIZE(url), L"https://www.msn.com/%s-%s/weather/forecast", lang, country );
        open_link( url );
        break;
    case K_SEND:
        send_ask();
        break;
    default:
        break;
    }
}

/* a point of the window, among what is drawn in the content */
static struct target hit_test( POINT pt )
{
    struct target none = { K_NONE, -1 };
    RECT client;
    int found = -1;

    GetClientRect( panel, &client );
    if (pt.y < header_height()) return none;
    pt.y += shown_scroll( &client ) - header_height();
    /* the button is inside the field, and comes after it */
    for (int i = 0; i < target_count; i++)
        if (PtInRect( &targets[i].rect, pt )) found = i;
    return found >= 0 ? targets[found] : none;
}

static void update_hot(void)
{
    POINT pt;
    struct target t;

    GetCursorPos( &pt );
    ScreenToClient( panel, &pt );
    t = hit_test( pt );
    if (t.kind != hot_target.kind || t.index != hot_target.index)
    {
        hot_target = t;
        InvalidateRect( panel, NULL, FALSE );
    }
}

static void start_typing(void)
{
    ask_active = caret_on = TRUE;
    SetTimer( panel, TIMER_CARET, 530, NULL );
    InvalidateRect( panel, NULL, FALSE );
}

static void paste_text(void)
{
    HANDLE handle;
    const WCHAR *text;
    int len = lstrlenW( ask );

    if (!OpenClipboard( panel )) return;
    if ((handle = GetClipboardData( CF_UNICODETEXT )) && (text = GlobalLock( handle )))
    {
        for (; *text && len < MAX_ASK; text++)
            if (*text >= 0x20 && *text != 0x7f) ask[len++] = *text;
        ask[len] = 0;
        GlobalUnlock( handle );
    }
    CloseClipboard();
    start_typing();
}

static LRESULT WINAPI panel_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    struct target t;

    switch (msg)
    {
    case WM_MOUSEMOVE:
        /* no card lights up while the list goes by under the pointer */
        if (!smooth.offset) update_hot();
        return 0;

    case WM_SETCURSOR:
    {
        POINT cursor;

        GetCursorPos( &cursor );
        ScreenToClient( hwnd, &cursor );
        t = hit_test( cursor );
        if (t.kind != K_NONE)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)(t.kind == K_ASK ? IDC_IBEAM : IDC_HAND) ) );
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN:
        pressed_target = hit_test( pt );
        if (pressed_target.kind == K_ASK)
        {
            SetFocus( hwnd );
            start_typing();
        }
        return 0;

    case WM_LBUTTONUP:
        t = hit_test( pt );
        if (t.kind != K_NONE && t.kind == pressed_target.kind && t.index == pressed_target.index)
            activate_target( &t );
        pressed_target.kind = K_NONE;
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
            hot_target.kind = K_NONE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wparam)
        {
        case VK_ESCAPE:
            if (ask[0])
            {
                ask[0] = 0;
                InvalidateRect( hwnd, NULL, FALSE );
            }
            else widgets_hide();
            break;
        case VK_RETURN:
            send_ask();
            break;
        case VK_BACK:
            if (ask[0])
            {
                ask[lstrlenW( ask ) - 1] = 0;
                start_typing();
            }
            break;
        case 'V':
            if (GetKeyState( VK_CONTROL ) & 0x8000) paste_text();
            break;
        }
        return 0;

    case WM_CHAR:
        if (wparam >= 0x20 && wparam != 0x7f)
        {
            int len = lstrlenW( ask );

            if (len < MAX_ASK)
            {
                ask[len] = (WCHAR)wparam;
                ask[len + 1] = 0;
                start_typing();
            }
        }
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_SMOOTH)
        {
            if (!smooth_scroll_step( &smooth, hwnd, TIMER_SMOOTH )) update_hot();
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (wparam == TIMER_CARET)
        {
            caret_on = !caret_on;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (wparam == TIMER_PANEL)
        {
            static int away, ticks;
            POINT cursor;
            RECT p, b;

            /* what the pointer opened goes when the pointer has left it */
            if (panel_hover)
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
        if (LOWORD( wparam ) == WA_INACTIVE && !panel_hover) widgets_hide();
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
    panel = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP, 0, 0, PANEL_W, px( 400 ),
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

static void change_location(void)
{
    static const struct page_template template =
    {
        { DS_SETFONT | DS_MODALFRAME | DS_SETFOREGROUND | DS_CENTER | WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
          0, 0, 0, 0, 200, 49 },
        0, 0, 0, 8, L"MS Shell Dlg",
    };

    if (DialogBoxIndirectParamW( startui_instance, &template.header, tray, city_proc, 0 ) == 1) start_fetch();
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
                          taskbar_style11() ? POPUP_WIN11 : POPUP_WIN10, &menu_look );
    switch (chosen)
    {
    case ID_SHOW_BOTH: ArcticWidgetSetShow( 2 ); changed_width(); break;
    case ID_SHOW_ICON: ArcticWidgetSetShow( 1 ); changed_width(); break;
    case ID_SHOW_OFF:  ArcticWidgetSetShow( 0 ); changed_width(); break;
    case ID_HOVER:     set_dword( L"Hover", !hover ); break;
    case ID_REFRESH:   start_fetch(); break;
    case ID_LOCATION:  change_location(); break;
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
    if (data && !d->have_weather)
    {
        d->have_weather = data->have_weather;
        lstrcpyW( d->city, data->city );
        d->temp = data->temp;
        d->feels = data->feels;
        d->humidity = data->humidity;
        d->wind = data->wind;
        d->code = data->code;
        d->is_day = data->is_day;
        d->imperial = data->imperial;
        d->day_count = data->day_count;
        memcpy( d->days, data->days, sizeof(d->days) );
        SetTimer( button, TIMER_RETRY, 60 * 1000, NULL );
    }
    else remember( d );
    if (data && !d->news_count)
    {
        d->news_count = data->news_count;
        memcpy( d->news, data->news, sizeof(d->news) );
        memset( data->news, 0, sizeof(data->news) );
    }
    if (d->have_weather && d->news_count) fetched_at = GetTickCount();
    data_free( data );
    data = d;
    changed_width();
    InvalidateRect( button, NULL, FALSE );
    if (panel_visible())
    {
        RECT client;

        hot_target.kind = K_NONE;
        layout();
        GetClientRect( panel, &client );
        scroll = min( scroll, max_scroll( &client ) );
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
    button = CreateWindowExW( 0, cls.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, tray, NULL,
                              startui_instance, NULL );
    if (button) SetTimer( button, TIMER_FIRST, 4000, NULL );
    return button;
}
