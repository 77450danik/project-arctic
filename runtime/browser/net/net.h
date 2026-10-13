/*
 * Arctic web engine: the network (docs/browser.md, stage 2)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTICHTML_NET_H
#define __ARCTICHTML_NET_H

#include <winsock2.h>

/* --- addresses (url.c), UTF-8 and ASCII on the wire --- */

struct url_parts
{
    char        scheme[16];     /* lower case, without the colon */
    char        host[256];      /* lower case, ASCII */
    int         port;           /* the scheme's own when the address has none */
    const char *path;           /* path and query, into the address; "/" when empty */
    size_t      path_len;       /* without the fragment */
};

BOOL url_parse( const char *url, struct url_parts *parts );
/* an address a page writes, against the page's; NULL when it means nothing */
char *url_resolve( const char *base, const char *ref, size_t ref_len );
char *url_from_typed( const WCHAR *typed );

/* --- TLS (tls.c) --- */

struct tls;

struct tls *tls_connect( SOCKET sock, const char *host, char *error, size_t error_size );
BOOL tls_send( struct tls *tls, const char *data, size_t len );
int tls_recv( struct tls *tls, char *data, size_t len );
void tls_close( struct tls *tls );

/* --- fetching (http.c) --- */

enum request_type
{
    REQUEST_DOCUMENT,
    REQUEST_STYLESHEET,
    REQUEST_IMAGE,
    REQUEST_SCRIPT,
    REQUEST_OTHER,
};

struct response
{
    int     status;         /* 200..., 0 when nothing came */
    char   *url;            /* where it came from at the end, after redirects */
    char   *content_type;   /* "text/html; charset=utf-8" */
    char   *body;
    size_t  length;
    BOOL    blocked;        /* the filter took it: there was no request */
    char    error[256];     /* why there is nothing, UTF-8 */
};

/* http:, https:, file:, data:. page is the address of the document the
 * request is for, for the filter and the cookies; NULL for the document */
BOOL net_fetch( const char *url, const char *page, enum request_type type, struct response *response );
/* a form's POST, application/x-www-form-urlencoded; page is the form's document */
BOOL net_post( const char *url, const char *page, const char *body, size_t len, struct response *response );
void response_free( struct response *response );
/* "charset=" of a Content-Type, as a code page; CP_UTF8 when it names none */
UINT content_type_code_page( const char *content_type );

/* --- cookies (cookies.c) --- */

void cookies_set( const struct url_parts *url, const char *header, size_t len );
/* "a=1; b=2" for a request, or NULL; the caller frees */
char *cookies_get( const struct url_parts *url );

/* --- the filter (filter/filter.c) --- */

/* TRUE when a request should not be made */
BOOL filter_blocks( const char *url, const struct url_parts *parts, const char *page, enum request_type type );
void filter_init(void);     /* loads the lists, fetches them in the background when old */
unsigned int filter_rule_count(void);

#endif /* __ARCTICHTML_NET_H */
