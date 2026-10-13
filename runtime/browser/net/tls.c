/*
 * Arctic web engine: TLS
 *
 * Through SSPI and Schannel, as every Windows program speaks TLS: on
 * Windows it is the system's own, in Arctic Wine's schannel over GnuTLS.
 * The certificate is checked here, with the chain and the SSL policy of
 * crypt32 for the host's name, and not left to Schannel: Wine's does not
 * check it, and the engine must behave the same everywhere.
 *
 * TLS 1.3 sends tickets after the handshake; Schannel reports them as
 * SEC_I_RENEGOTIATE, and the context is fed them again.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define SECURITY_WIN32
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "arctichtml_private.h"
#include "net.h"

#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>

#define TLS_BUFFER (16384 + 1024)

struct tls
{
    SOCKET                    sock;
    CredHandle                cred;
    CtxtHandle                ctx;
    BOOL                      have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;
    char                      in[TLS_BUFFER * 2];   /* what came in and is not decrypted yet */
    size_t                    in_len;
    char                     *plain;                /* what is decrypted and not read yet */
    size_t                    plain_len, plain_capacity;
    BOOL                      closed;
};

static BOOL send_all( SOCKET sock, const char *data, size_t len )
{
    while (len)
    {
        int sent = send( sock, data, (int)min( len, 65536 ), 0 );

        if (sent <= 0) return FALSE;
        data += sent;
        len -= sent;
    }
    return TRUE;
}

static void send_tokens( struct tls *tls, SecBufferDesc *out )
{
    for (unsigned int i = 0; i < out->cBuffers; i++)
    {
        SecBuffer *buffer = &out->pBuffers[i];

        if (buffer->BufferType == SECBUFFER_TOKEN && buffer->cbBuffer && buffer->pvBuffer)
            send_all( tls->sock, buffer->pvBuffer, buffer->cbBuffer );
        if (buffer->pvBuffer) FreeContextBuffer( buffer->pvBuffer );
        buffer->pvBuffer = NULL;
    }
}

static BOOL receive_more( struct tls *tls )
{
    int got;

    if (tls->in_len >= sizeof(tls->in)) return FALSE;
    got = recv( tls->sock, tls->in + tls->in_len, (int)(sizeof(tls->in) - tls->in_len), 0 );
    if (got <= 0) return FALSE;
    tls->in_len += got;
    return TRUE;
}

/* the handshake, from the first ClientHello or from a ticket after it */
static SECURITY_STATUS handshake( struct tls *tls, const WCHAR *host, BOOL started )
{
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                  ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION;
    SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
    DWORD out_flags;

    if (!started)
    {
        SecBuffer out_buffer = { 0, SECBUFFER_TOKEN, NULL };
        SecBufferDesc out = { SECBUFFER_VERSION, 1, &out_buffer };

        status = InitializeSecurityContextW( &tls->cred, NULL, (WCHAR *)host, flags, 0, 0, NULL, 0, &tls->ctx,
                                             &out, &out_flags, NULL );
        if (status != SEC_I_CONTINUE_NEEDED) return status;
        tls->have_ctx = TRUE;
        send_tokens( tls, &out );
    }

    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE || status == SEC_I_INCOMPLETE_CREDENTIALS)
    {
        SecBuffer in_buffers[2], out_buffers[1];
        SecBufferDesc in = { SECBUFFER_VERSION, 2, in_buffers }, out = { SECBUFFER_VERSION, 1, out_buffers };

        /* after a ticket the context goes on with what it has: waiting for
         * the server here would wait for ever */
        if ((!tls->in_len && !started) || status == SEC_E_INCOMPLETE_MESSAGE)
            if (!receive_more( tls )) return SEC_E_INTERNAL_ERROR;
        started = FALSE;

        in_buffers[0] = (SecBuffer){ (ULONG)tls->in_len, SECBUFFER_TOKEN, tls->in };
        in_buffers[1] = (SecBuffer){ 0, SECBUFFER_EMPTY, NULL };
        out_buffers[0] = (SecBuffer){ 0, SECBUFFER_TOKEN, NULL };

        status = InitializeSecurityContextW( &tls->cred, &tls->ctx, (WCHAR *)host, flags, 0, 0, &in, 0, NULL, &out,
                                             &out_flags, NULL );
        if (status == SEC_E_INCOMPLETE_MESSAGE) continue;
        send_tokens( tls, &out );
        if (FAILED(status)) return status;

        /* what is past the handshake is the start of the data */
        if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer)
        {
            memmove( tls->in, tls->in + tls->in_len - in_buffers[1].cbBuffer, in_buffers[1].cbBuffer );
            tls->in_len = in_buffers[1].cbBuffer;
        }
        else tls->in_len = 0;
    }
    return status;
}

/* the chain of the server's certificate, to a root of the machine, for this name */
static BOOL check_certificate( struct tls *tls, const WCHAR *host, char *error, size_t error_size )
{
    CERT_CHAIN_PARA chain_para = { sizeof(chain_para) };
    const CERT_CONTEXT *cert = NULL;
    const CERT_CHAIN_CONTEXT *chain = NULL;
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = { .cbSize = sizeof(ssl), .dwAuthType = AUTHTYPE_SERVER,
                                             .pwszServerName = (WCHAR *)host };
    CERT_CHAIN_POLICY_PARA policy_para = { sizeof(policy_para), 0, &ssl };
    CERT_CHAIN_POLICY_STATUS policy = { sizeof(policy) };
    BOOL ok = FALSE;

    if (QueryContextAttributesW( &tls->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert ) != SEC_E_OK || !cert)
    {
        snprintf( error, error_size, "сервер не надіслав сертифіката" );
        return FALSE;
    }
    if (!CertGetCertificateChain( NULL, cert, NULL, cert->hCertStore, &chain_para, 0, NULL, &chain ))
        snprintf( error, error_size, "не вдалося побудувати ланцюжок сертифіката (%lu)", GetLastError() );
    else if (!CertVerifyCertificateChainPolicy( CERT_CHAIN_POLICY_SSL, chain, &policy_para, &policy ))
        snprintf( error, error_size, "перевірка сертифіката не вдалася (%lu)", GetLastError() );
    else if (policy.dwError)
    {
        const char *why = policy.dwError == CERT_E_CN_NO_MATCH ? "сертифікат виданий іншому імені" :
                          policy.dwError == CERT_E_EXPIRED ? "сертифікат прострочений" :
                          policy.dwError == CERT_E_UNTRUSTEDROOT ? "сертифікат виданий невідомим центром" :
                          "сертифікат не пройшов перевірки";
        snprintf( error, error_size, "%s (0x%08lx)", why, policy.dwError );
    }
    else ok = TRUE;

    if (chain) CertFreeCertificateChain( chain );
    CertFreeCertificateContext( cert );
    return ok;
}

struct tls *tls_connect( SOCKET sock, const char *host_utf8, char *error, size_t error_size )
{
    SCHANNEL_CRED cred = { .dwVersion = SCHANNEL_CRED_VERSION,
                           .dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                                      SCH_USE_STRONG_CRYPTO };
    WCHAR host[256];
    struct tls *tls;
    SECURITY_STATUS status;

    if (!MultiByteToWideChar( CP_UTF8, 0, host_utf8, -1, host, ARRAY_SIZE(host) )) return NULL;
    if (!(tls = calloc( 1, sizeof(*tls) ))) return NULL;
    tls->sock = sock;

    status = AcquireCredentialsHandleW( NULL, (WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL,
                                        &tls->cred, NULL );
    if (status != SEC_E_OK)
    {
        snprintf( error, error_size, "TLS недоступний: Schannel відмовив (0x%08lx)", status );
        free( tls );
        return NULL;
    }
    tls->have_cred = TRUE;

    if ((status = handshake( tls, host, FALSE )) != SEC_E_OK)
    {
        snprintf( error, error_size, "рукостискання TLS не вдалося (0x%08lx)", status );
        tls_close( tls );
        return NULL;
    }
    if (!check_certificate( tls, host, error, error_size ))
    {
        tls_close( tls );
        return NULL;
    }
    QueryContextAttributesW( &tls->ctx, SECPKG_ATTR_STREAM_SIZES, &tls->sizes );
    return tls;
}

BOOL tls_send( struct tls *tls, const char *data, size_t len )
{
    char *message = malloc( tls->sizes.cbHeader + tls->sizes.cbMaximumMessage + tls->sizes.cbTrailer );
    BOOL ok = TRUE;

    if (!message) return FALSE;
    while (len && ok)
    {
        ULONG chunk = (ULONG)min( len, tls->sizes.cbMaximumMessage );
        SecBuffer buffers[4] =
        {
            { tls->sizes.cbHeader, SECBUFFER_STREAM_HEADER, message },
            { chunk, SECBUFFER_DATA, message + tls->sizes.cbHeader },
            { tls->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, message + tls->sizes.cbHeader + chunk },
            { 0, SECBUFFER_EMPTY, NULL },
        };
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, buffers };

        memcpy( message + tls->sizes.cbHeader, data, chunk );
        ok = EncryptMessage( &tls->ctx, 0, &desc, 0 ) == SEC_E_OK &&
             send_all( tls->sock, message, buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer );
        data += chunk;
        len -= chunk;
    }
    free( message );
    return ok;
}

static BOOL keep_plain( struct tls *tls, const char *data, size_t len )
{
    if (tls->plain_len + len > tls->plain_capacity)
    {
        size_t capacity = max( tls->plain_capacity * 2, tls->plain_len + len + 4096 );
        char *grown = realloc( tls->plain, capacity );

        if (!grown) return FALSE;
        tls->plain = grown;
        tls->plain_capacity = capacity;
    }
    memcpy( tls->plain + tls->plain_len, data, len );
    tls->plain_len += len;
    return TRUE;
}

/* up to len bytes of the stream; 0 at its end */
int tls_recv( struct tls *tls, char *data, size_t len )
{
    while (!tls->plain_len && !tls->closed)
    {
        SecBuffer buffers[4];
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, buffers };
        SECURITY_STATUS status;

        if (!tls->in_len && !receive_more( tls )) { tls->closed = TRUE; break; }

        buffers[0] = (SecBuffer){ (ULONG)tls->in_len, SECBUFFER_DATA, tls->in };
        for (int i = 1; i < 4; i++) buffers[i] = (SecBuffer){ 0, SECBUFFER_EMPTY, NULL };
        status = DecryptMessage( &tls->ctx, &desc, 0, NULL );

        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (!receive_more( tls )) tls->closed = TRUE;
            continue;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) { tls->closed = TRUE; break; }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE)
        {
            WARN( "DecryptMessage: 0x%08lx\n", status );
            tls->closed = TRUE;
            break;
        }

        {
            SecBuffer *extra = NULL;

            for (int i = 1; i < 4; i++)
            {
                if (buffers[i].BufferType == SECBUFFER_DATA) keep_plain( tls, buffers[i].pvBuffer, buffers[i].cbBuffer );
                if (buffers[i].BufferType == SECBUFFER_EXTRA) extra = &buffers[i];
            }
            if (extra)
            {
                memmove( tls->in, tls->in + tls->in_len - extra->cbBuffer, extra->cbBuffer );
                tls->in_len = extra->cbBuffer;
            }
            else tls->in_len = 0;
        }
        if (status == SEC_I_RENEGOTIATE)
        {
            /* a ticket of TLS 1.3, or a real renegotiation: the context takes it */
            status = handshake( tls, NULL, TRUE );
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
            {
                WARN( "after SEC_I_RENEGOTIATE: 0x%08lx\n", status );
                tls->closed = TRUE;
            }
        }
    }
    if (!tls->plain_len) return 0;
    len = min( len, tls->plain_len );
    memcpy( data, tls->plain, len );
    memmove( tls->plain, tls->plain + len, tls->plain_len - len );
    tls->plain_len -= len;
    return (int)len;
}

void tls_close( struct tls *tls )
{
    if (!tls) return;
    if (tls->have_ctx) DeleteSecurityContext( &tls->ctx );
    if (tls->have_cred) FreeCredentialsHandle( &tls->cred );
    free( tls->plain );
    free( tls );
}
