/*
 * Arctic web engine: its own pages
 *
 * about:arctic and the page that says why a page did not load are HTML like
 * any other, parsed and laid out by the engine itself: they are the first
 * thing a change to the engine shows on.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "arctichtml_private.h"

struct buffer
{
    char   *data;
    size_t  len, size;
};

static void append( struct buffer *buf, const char *text, size_t len )
{
    if (!buf->data && buf->size) return;    /* out of memory before: give up quietly */
    if (buf->len + len + 1 > buf->size)
    {
        size_t size = max( buf->size * 2, buf->len + len + 1024 );
        char *grown = realloc( buf->data, size );

        if (!grown)
        {
            free( buf->data );
            buf->data = NULL;
            buf->size = 1;
            return;
        }
        buf->data = grown;
        buf->size = size;
    }
    memcpy( buf->data + buf->len, text, len );
    buf->len += len;
    buf->data[buf->len] = 0;
}

static void append_str( struct buffer *buf, const char *text )
{
    append( buf, text, strlen( text ) );
}

/* text of ours or of the user's, in UTF-8 and safe inside HTML */
static void append_escaped( struct buffer *buf, const WCHAR *text )
{
    int len = WideCharToMultiByte( CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL );
    char *utf8 = malloc( len );

    if (!utf8) return;
    WideCharToMultiByte( CP_UTF8, 0, text, -1, utf8, len, NULL, NULL );
    for (const char *p = utf8; *p; p++)
    {
        switch (*p)
        {
        case '<': append_str( buf, "&lt;" ); break;
        case '>': append_str( buf, "&gt;" ); break;
        case '&': append_str( buf, "&amp;" ); break;
        case '"': append_str( buf, "&quot;" ); break;
        default:  append( buf, p, 1 ); break;
        }
    }
    free( utf8 );
}

static const char page_style[] =
    "<style>"
    "body { font-family: 'Segoe UI', system-ui, sans-serif; margin: 40px 48px; color: #1a1a1a;"
    "       background: #ffffff; line-height: 1.45 }"
    "h1 { color: #1f3a5f; font-size: 32px; margin: 0 0 12px }"
    "h2 { color: #1f3a5f; font-size: 20px; margin: 28px 0 8px }"
    "hr { border: 0; border-top: 1px solid #d0d7de; margin: 0 0 20px }"
    ".address { background: #f4f6f8; border-left: 3px solid #1f3a5f; padding: 10px 16px; margin: 20px 0 }"
    ".address .label { color: #505a64; font-size: 13px }"
    "code { font-family: 'Courier New', monospace; font-size: 15px }"
    ".search { display: flex; gap: 8px; max-width: 640px; margin: 0 0 24px }"
    ".query { flex: 1; font-size: 16px; padding: 8px 10px; border: 1px solid #8a97a6; border-radius: 4px }"
    ".search button { font-size: 16px; padding: 8px 18px; background: #1f5fa8; color: #fff; border: 0 }"
    ".note { color: #505a64; font-size: 13px }"
    "</style>";

char *page_about( const WCHAR *url, size_t *len )
{
    struct buffer buf = { 0 };

    append_str( &buf,
        "<!DOCTYPE html><html lang=uk><head><meta charset=utf-8><title>Arctic</title>" );
    append_str( &buf, page_style );
    append_str( &buf,
        "</head><body>"
        "<h1>Arctic</h1><hr>"
        "<form class=search action=\"https://www.bing.com/search\" method=get>"
        "<input class=query type=search name=q placeholder=\"Пошук в інтернеті або адреса\">"
        "<button type=submit>Знайти</button></form>"
        "<p>Цю сторінку намалював <b>рушій Arctic</b>, а не Gecko і не Blink. Вона — звичайний HTML: "
        "його розібрав <i>lexbor</i>, стилі зіставив модуль стилів lexbor, а верстку, рядки й "
        "малювання рушій робить сам.</p>"
        "<h2>Що вміє етап 2</h2>"
        "<ul>"
        "<li>сайти з мережі: HTTP/1.1 і HTTPS через Schannel Windows, переадресації, куки</li>"
        "<li>вбудований фільтр із синтаксисом uBlock Origin: EasyList і EasyPrivacy, оновлюються самі</li>"
        "<li>таблиці стилів сторінок: <code>@media</code>, <code>var()</code>, скорочення <code>background</code>, "
        "<code>font</code>, <code>list-style</code>, <code>border-width</code></li>"
        "<li>посилання, «назад» і «вперед», переходи за <code>#якорем</code> у межах сторінки</li>"
        "<li>форми: поля, кнопки, галочки, списки, пошук з адресного рядка (Bing)</li>"
        "<li>flexbox, inline-block, <code>position</code>, <code>overflow</code></li>"
        "<li>сторінки з диска: <code>file:///C:/шлях/сторінка.html</code> або просто <code>C:\\шлях</code></li>"
        "</ul>"
        "<h2>Що далі</h2>"
        "<ol>"
        "<li>текст через DirectWrite, шрифти Noto для всіх письмен — етап 3</li>"
        "<li>JavaScript — етап 4</li>"
        "<li>таблиці, float, grid — етапи 5 і 8</li>"
        "</ol>"
        "<div class=address><div class=label>Адреса, яку дала рамка:</div><code>" );
    append_escaped( &buf, url );
    append_str( &buf,
        "</code></div>"
        "<p class=note>Рушій — звичайна Windows-програма і йде на будь-якій Windows 10 або 11. "
        "План — docs/browser.md.</p>"
        "</body></html>" );
    *len = buf.len;
    return buf.data;
}

char *page_error( const WCHAR *url, const WCHAR *reason, size_t *len )
{
    struct buffer buf = { 0 };

    append_str( &buf,
        "<!DOCTYPE html><html lang=uk><head><meta charset=utf-8><title>Сторінку не відкрито</title>" );
    append_str( &buf, page_style );
    append_str( &buf, "</head><body><h1>Сторінку не відкрито</h1><hr><p>" );
    append_escaped( &buf, reason );
    append_str( &buf, "</p><div class=address><div class=label>Адреса:</div><code>" );
    append_escaped( &buf, url );
    append_str( &buf, "</code></div></body></html>" );
    *len = buf.len;
    return buf.data;
}
