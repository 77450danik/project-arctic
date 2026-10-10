/*
 * The shares of a computer, asked of it as Windows asks: SMB 2 to its IPC$,
 * the pipe of the Server service (srvsvc), NetrShareEnum over DCE/RPC.
 * Only this list goes this way; the files themselves are the kernel's
 * (cifs, mounted by the host's arctic-smb).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntlanman.h"
#include "bcrypt.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntlanman);

#define SMB2_NEGOTIATE        0
#define SMB2_SESSION_SETUP    1
#define SMB2_TREE_CONNECT     3
#define SMB2_CREATE           5
#define SMB2_CLOSE            6
#define SMB2_READ             8
#define SMB2_IOCTL            11

#define NT_MORE_PROCESSING    0xc0000016
#define NT_BUFFER_OVERFLOW    0x80000005
#define NT_PENDING            0x00000103
#define NT_NO_MEMORY          ((NTSTATUS)0xc0000017)
#define NT_ACCESS_DENIED      ((NTSTATUS)0xc0000022)
#define NT_BAD_RESPONSE       ((NTSTATUS)0xc00000c3)
#define NT_DISCONNECTED       ((NTSTATUS)0xc000020c)

#define NTLM_FLAGS            0xa0888215  /* Unicode, target, sign, NTLM, always sign, NTLM2, target info, 128, 56 */
#define NTLM_ANONYMOUS        0x00000800

#define MAX_FRAGMENT          4280

struct session
{
    SOCKET  sock;
    ULONG64 message_id;
    ULONG64 session_id;
    ULONG   tree_id;
    USHORT  dialect;
    BOOL    must_sign;
    BOOL    sign;
    BYTE    key[16];
    BYTE    file_id[16];
};

static void set16( BYTE *p, UINT v ) { p[0] = v; p[1] = v >> 8; }
static void set32( BYTE *p, UINT v ) { memcpy( p, &v, 4 ); }
static void set64( BYTE *p, ULONG64 v ) { memcpy( p, &v, 8 ); }
static UINT get16( const BYTE *p ) { return p[0] | p[1] << 8; }
static UINT get32( const BYTE *p ) { UINT v; memcpy( &v, p, 4 ); return v; }
static ULONG64 get64( const BYTE *p ) { ULONG64 v; memcpy( &v, p, 8 ); return v; }

static BOOL digest( const WCHAR *algorithm, const BYTE *key, UINT key_len, const BYTE *data, UINT len,
                    BYTE *out, UINT out_len )
{
    BCRYPT_ALG_HANDLE alg;
    NTSTATUS status;

    if (BCryptOpenAlgorithmProvider( &alg, algorithm, NULL, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0 )) return FALSE;
    status = BCryptHash( alg, (UCHAR *)key, key_len, (UCHAR *)data, len, out, out_len );
    BCryptCloseAlgorithmProvider( alg, 0 );
    return !status;
}

static BOOL send_all( SOCKET sock, const BYTE *data, UINT len )
{
    while (len)
    {
        int n = send( sock, (const char *)data, len, 0 );
        if (n <= 0) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

static BOOL recv_all( SOCKET sock, BYTE *data, UINT len )
{
    while (len)
    {
        int n = recv( sock, (char *)data, len, 0 );
        if (n <= 0) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

/* one request and its answer; the answer is the caller's to free */
static NTSTATUS exchange( struct session *s, UINT command, const BYTE *body, UINT body_len, BYTE **reply,
                          UINT *reply_len )
{
    UINT total = 64 + body_len, len;
    BYTE *msg, *hdr, *buf, prefix[4];
    NTSTATUS status;

    *reply = NULL;
    *reply_len = 0;
    if (!(msg = calloc( 1, 4 + total ))) return NT_NO_MEMORY;
    msg[1] = total >> 16;
    msg[2] = total >> 8;
    msg[3] = total;
    hdr = msg + 4;
    hdr[0] = 0xfe; hdr[1] = 'S'; hdr[2] = 'M'; hdr[3] = 'B';
    set16( hdr + 4, 64 );
    set16( hdr + 6, s->dialect >= 0x210 ? 1 : 0 );
    set16( hdr + 12, command );
    set16( hdr + 14, 16 );
    set32( hdr + 16, s->sign ? 8 : 0 );
    set64( hdr + 24, s->message_id++ );
    set32( hdr + 32, 0xfeff );
    set32( hdr + 36, s->tree_id );
    set64( hdr + 40, s->session_id );
    memcpy( hdr + 64, body, body_len );
    if (s->sign)
    {
        BYTE mac[32];
        if (digest( BCRYPT_SHA256_ALGORITHM, s->key, 16, hdr, total, mac, sizeof(mac) )) memcpy( hdr + 48, mac, 16 );
    }
    if (!send_all( s->sock, msg, 4 + total ))
    {
        free( msg );
        return NT_DISCONNECTED;
    }
    free( msg );

    for (;;)
    {
        if (!recv_all( s->sock, prefix, 4 )) return NT_DISCONNECTED;
        len = prefix[1] << 16 | prefix[2] << 8 | prefix[3];
        if (len < 64 || len > 0x400000 || !(buf = malloc( len ))) return NT_BAD_RESPONSE;
        if (!recv_all( s->sock, buf, len ) || buf[0] != 0xfe || buf[1] != 'S' || buf[2] != 'M' || buf[3] != 'B')
        {
            free( buf );
            return NT_BAD_RESPONSE;
        }
        status = get32( buf + 8 );
        /* "it takes a while": the answer itself comes after */
        if ((UINT)status == NT_PENDING && (get32( buf + 16 ) & 2))
        {
            free( buf );
            continue;
        }
        break;
    }
    *reply = buf;
    *reply_len = len;
    return status;
}

/**********************************************************************
 *          The logon: NTLMv2 inside SPNEGO
 */

/* puts a DER tag and the length in front of what the buffer holds */
static UINT wrap( BYTE *buf, UINT len, BYTE tag )
{
    UINT head = len < 128 ? 2 : len < 256 ? 3 : 4;

    memmove( buf + head, buf, len );
    buf[0] = tag;
    if (len < 128) buf[1] = len;
    else if (len < 256) { buf[1] = 0x81; buf[2] = len; }
    else { buf[1] = 0x82; buf[2] = len >> 8; buf[3] = len; }
    return len + head;
}

static UINT prepend( BYTE *buf, UINT len, const BYTE *data, UINT data_len )
{
    memmove( buf + data_len, buf, len );
    memcpy( buf, data, data_len );
    return len + data_len;
}

static UINT ntlm_negotiate( BYTE *buf )
{
    static const BYTE spnego[] = { 0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02 };
    static const BYTE mechs[] = { 0xa0, 0x0e, 0x30, 0x0c, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37,
                                  0x02, 0x02, 0x0a };
    UINT len = 32;

    memset( buf, 0, 32 );
    memcpy( buf, "NTLMSSP", 8 );
    set32( buf + 8, 1 );
    set32( buf + 12, NTLM_FLAGS );
    set32( buf + 20, 32 );
    set32( buf + 28, 32 );

    len = wrap( buf, len, 0x04 );
    len = wrap( buf, len, 0xa2 );
    len = prepend( buf, len, mechs, sizeof(mechs) );
    len = wrap( buf, len, 0x30 );
    len = wrap( buf, len, 0xa0 );
    len = prepend( buf, len, spnego, sizeof(spnego) );
    return wrap( buf, len, 0x60 );
}

static void field( BYTE *msg, UINT at, UINT *pos, const void *data, UINT len )
{
    set16( msg + at, len );
    set16( msg + at + 2, len );
    set32( msg + at + 4, *pos );
    if (len) memcpy( msg + *pos, data, len );
    *pos += len;
}

/* the answer to the server's challenge; the key of the session with it */
static UINT ntlm_authenticate( BYTE *buf, UINT size, const BYTE *blob, UINT blob_len, const struct logon *logon,
                               BYTE *session_key )
{
    BYTE nt_hash[16], v2_hash[16], proof[16], client_challenge[8], timestamp[8], *temp, *response;
    const BYTE *challenge = NULL, *target_info = NULL;
    WCHAR identity[520], computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD computer_len = ARRAY_SIZE(computer);
    UINT i, target_len = 0, server_flags, flags, pos = 64, temp_len, user_len, domain_len;
    BOOL anonymous = !logon || !logon->user[0];
    FILETIME now;

    for (i = 0; i + 48 <= blob_len; i++)
    {
        if (memcmp( blob + i, "NTLMSSP", 8 ) || get32( blob + i + 8 ) != 2) continue;
        challenge = blob + i;
        blob_len -= i;
        break;
    }
    if (!challenge) return 0;
    server_flags = get32( challenge + 20 );
    if (blob_len >= 48)
    {
        UINT len = get16( challenge + 40 ), off = get32( challenge + 44 );
        if (len && off + len <= blob_len)
        {
            target_info = challenge + off;
            target_len = len;
        }
    }

    GetSystemTimeAsFileTime( &now );
    memcpy( timestamp, &now, 8 );
    /* the server's own clock, when it tells it */
    for (i = 0; i + 4 <= target_len; )
    {
        UINT id = get16( target_info + i ), len = get16( target_info + i + 2 );
        if (!id) break;
        if (id == 7 && len == 8 && i + 12 <= target_len) memcpy( timestamp, target_info + i + 4, 8 );
        i += 4 + len;
    }

    if (!GetComputerNameW( computer, &computer_len )) computer_len = 0;
    flags = (NTLM_FLAGS & server_flags) | 0x201;
    memset( buf, 0, 64 );
    memcpy( buf, "NTLMSSP", 8 );
    set32( buf + 8, 3 );

    if (anonymous)
    {
        static const BYTE zero = 0;

        flags |= NTLM_ANONYMOUS;
        field( buf, 28, &pos, NULL, 0 );
        field( buf, 36, &pos, NULL, 0 );
        field( buf, 44, &pos, computer, computer_len * sizeof(WCHAR) );
        field( buf, 12, &pos, &zero, 1 );
        field( buf, 20, &pos, NULL, 0 );
        field( buf, 52, &pos, NULL, 0 );
        memset( session_key, 0, 16 );
    }
    else
    {
        static const BYTE no_lm[24];

        user_len = wcslen( logon->user );
        domain_len = wcslen( logon->domain );
        temp_len = 28 + target_len + 4;
        if (64 + (user_len + domain_len + computer_len) * sizeof(WCHAR) + 24 + 16 + temp_len + 32 > size) return 0;
        if (!(response = malloc( 16 + 8 + temp_len ))) return 0;
        temp = response + 16 + 8;

        if (!digest( BCRYPT_MD4_ALGORITHM, NULL, 0, (const BYTE *)logon->password,
                     wcslen( logon->password ) * sizeof(WCHAR), nt_hash, 16 ))
        {
            free( response );
            return 0;
        }
        wcscpy( identity, logon->user );
        CharUpperBuffW( identity, user_len );
        wcscpy( identity + user_len, logon->domain );
        digest( BCRYPT_MD5_ALGORITHM, nt_hash, 16, (const BYTE *)identity, (user_len + domain_len) * sizeof(WCHAR),
                v2_hash, 16 );

        BCryptGenRandom( NULL, client_challenge, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
        memset( temp, 0, temp_len );
        temp[0] = temp[1] = 1;
        memcpy( temp + 8, timestamp, 8 );
        memcpy( temp + 16, client_challenge, 8 );
        if (target_len) memcpy( temp + 28, target_info, target_len );

        /* the proof: the server's challenge and all of the above, keyed */
        memcpy( temp - 8, challenge + 24, 8 );
        digest( BCRYPT_MD5_ALGORITHM, v2_hash, 16, temp - 8, 8 + temp_len, proof, 16 );
        digest( BCRYPT_MD5_ALGORITHM, v2_hash, 16, proof, 16, session_key, 16 );
        memcpy( temp - 16, proof, 16 );

        field( buf, 28, &pos, logon->domain, domain_len * sizeof(WCHAR) );
        field( buf, 36, &pos, logon->user, user_len * sizeof(WCHAR) );
        field( buf, 44, &pos, computer, computer_len * sizeof(WCHAR) );
        field( buf, 12, &pos, no_lm, sizeof(no_lm) );
        field( buf, 20, &pos, temp - 16, 16 + temp_len );
        field( buf, 52, &pos, NULL, 0 );
        free( response );
    }
    set32( buf + 60, flags );

    pos = wrap( buf, pos, 0x04 );
    pos = wrap( buf, pos, 0xa2 );
    pos = wrap( buf, pos, 0x30 );
    return wrap( buf, pos, 0xa1 );
}

static NTSTATUS session_setup( struct session *s, const BYTE *token, UINT token_len, BYTE **reply, UINT *reply_len )
{
    BYTE *body;
    NTSTATUS status;

    if (!(body = calloc( 1, 24 + token_len ))) return NT_NO_MEMORY;
    set16( body, 25 );
    body[3] = 1;                    /* signing: can */
    set16( body + 12, 64 + 24 );
    set16( body + 14, token_len );
    memcpy( body + 24, token, token_len );
    status = exchange( s, SMB2_SESSION_SETUP, body, 24 + token_len, reply, reply_len );
    free( body );
    return status;
}

static NTSTATUS log_on( struct session *s, const struct logon *logon )
{
    BYTE *token, *reply;
    UINT len, reply_len, off, blob_len;
    BYTE key[16];
    NTSTATUS status;

    if (!(token = malloc( 8192 ))) return NT_NO_MEMORY;
    len = ntlm_negotiate( token );
    status = session_setup( s, token, len, &reply, &reply_len );
    if ((UINT)status != NT_MORE_PROCESSING)
    {
        free( reply );
        free( token );
        return status ? status : NT_BAD_RESPONSE;
    }
    s->session_id = get64( reply + 40 );
    off = reply_len >= 72 ? get16( reply + 64 + 4 ) : 0;
    blob_len = reply_len >= 72 ? get16( reply + 64 + 6 ) : 0;
    if (!off || off + blob_len > reply_len ||
        !(len = ntlm_authenticate( token, 8192 - 16, reply + off, blob_len, logon, key )))
    {
        free( reply );
        free( token );
        return NT_BAD_RESPONSE;
    }
    free( reply );
    status = session_setup( s, token, len, &reply, &reply_len );
    free( token );
    if (!status && reply_len >= 68)
    {
        UINT flags = get16( reply + 64 + 2 );

        /* a guest and nobody have no key to sign with */
        if (!(flags & 3) && logon && logon->user[0] && s->must_sign)
        {
            memcpy( s->key, key, 16 );
            s->sign = TRUE;
        }
        TRACE( "logged on as %s, flags %#x\n", debugstr_w( logon ? logon->user : NULL ), flags );
    }
    free( reply );
    return status;
}

/**********************************************************************
 *          The pipe of the Server service
 */

static NTSTATUS negotiate( struct session *s )
{
    BYTE body[40] = { 0 }, *reply;
    UINT reply_len;
    NTSTATUS status;

    set16( body, 36 );
    set16( body + 2, 2 );
    set16( body + 4, 1 );
    BCryptGenRandom( NULL, body + 12, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    set16( body + 36, 0x0202 );
    set16( body + 38, 0x0210 );
    status = exchange( s, SMB2_NEGOTIATE, body, sizeof(body), &reply, &reply_len );
    if (!status && reply_len >= 64 + 8)
    {
        s->must_sign = (get16( reply + 64 + 2 ) & 2) != 0;
        s->dialect = get16( reply + 64 + 4 );
        TRACE( "dialect %#x, signing %s\n", s->dialect, s->must_sign ? "required" : "optional" );
    }
    else if (!status) status = NT_BAD_RESPONSE;
    free( reply );
    return status;
}

static NTSTATUS tree_connect( struct session *s, const WCHAR *server )
{
    WCHAR path[300];
    BYTE body[8 + sizeof(path)] = { 0 }, *reply;
    UINT reply_len, bytes;
    NTSTATUS status;

    swprintf( path, ARRAY_SIZE(path), L"\\\\%s\\IPC$", server );
    bytes = wcslen( path ) * sizeof(WCHAR);
    set16( body, 9 );
    set16( body + 4, 64 + 8 );
    set16( body + 6, bytes );
    memcpy( body + 8, path, bytes );
    status = exchange( s, SMB2_TREE_CONNECT, body, 8 + bytes, &reply, &reply_len );
    if (!status) s->tree_id = get32( reply + 36 );
    free( reply );
    return status;
}

static NTSTATUS open_pipe( struct session *s, const WCHAR *name )
{
    BYTE body[56 + 64] = { 0 }, *reply;
    UINT reply_len, bytes = wcslen( name ) * sizeof(WCHAR);
    NTSTATUS status;

    set16( body, 57 );
    set32( body + 4, 2 );               /* impersonation */
    set32( body + 24, 0x0012019f );     /* read and write */
    set32( body + 32, 3 );              /* shared for both */
    set32( body + 36, 1 );              /* open what is there */
    set16( body + 44, 64 + 56 );
    set16( body + 46, bytes );
    memcpy( body + 56, name, bytes );
    status = exchange( s, SMB2_CREATE, body, 56 + bytes, &reply, &reply_len );
    if (!status && reply_len >= 64 + 80) memcpy( s->file_id, reply + 64 + 64, 16 );
    else if (!status) status = NT_BAD_RESPONSE;
    free( reply );
    return status;
}

static void close_pipe( struct session *s )
{
    BYTE body[24] = { 0 }, *reply;
    UINT reply_len;

    set16( body, 24 );
    memcpy( body + 8, s->file_id, 16 );
    exchange( s, SMB2_CLOSE, body, sizeof(body), &reply, &reply_len );
    free( reply );
}

struct bytes
{
    BYTE *data;
    UINT  len;
};

static BOOL append( struct bytes *bytes, const BYTE *data, UINT len )
{
    BYTE *grown = realloc( bytes->data, bytes->len + len + 1 );

    if (!grown) return FALSE;
    memcpy( grown + bytes->len, data, len );
    bytes->data = grown;
    bytes->len += len;
    return TRUE;
}

/* a message written to the pipe and the first of what it answers */
static NTSTATUS transceive( struct session *s, const BYTE *in, UINT in_len, struct bytes *out )
{
    BYTE *body, *reply;
    UINT reply_len;
    NTSTATUS status;

    if (!(body = calloc( 1, 56 + in_len ))) return NT_NO_MEMORY;
    set16( body, 57 );
    set32( body + 4, 0x0011c017 );      /* FSCTL_PIPE_TRANSCEIVE */
    memcpy( body + 8, s->file_id, 16 );
    set32( body + 24, 64 + 56 );
    set32( body + 28, in_len );
    set32( body + 44, 8192 );
    set32( body + 48, 1 );
    memcpy( body + 56, in, in_len );
    status = exchange( s, SMB2_IOCTL, body, 56 + in_len, &reply, &reply_len );
    free( body );
    if ((!status || (UINT)status == NT_BUFFER_OVERFLOW) && reply_len >= 64 + 48)
    {
        UINT off = get32( reply + 64 + 32 ), count = get32( reply + 64 + 36 );

        if (off + count > reply_len || !append( out, reply + off, count )) status = NT_BAD_RESPONSE;
        else status = 0;
    }
    else if (!status) status = NT_BAD_RESPONSE;
    free( reply );
    return status;
}

static NTSTATUS read_pipe( struct session *s, struct bytes *out )
{
    BYTE body[49] = { 0 }, *reply;
    UINT reply_len;
    NTSTATUS status;

    set16( body, 49 );
    body[2] = 0x50;
    set32( body + 4, 8192 );
    memcpy( body + 16, s->file_id, 16 );
    set32( body + 32, 1 );
    status = exchange( s, SMB2_READ, body, sizeof(body), &reply, &reply_len );
    if ((!status || (UINT)status == NT_BUFFER_OVERFLOW) && reply_len >= 64 + 16)
    {
        UINT off = reply[64 + 2], count = get32( reply + 64 + 4 );

        if (!count || off + count > reply_len || !append( out, reply + off, count ))
            status = NT_BAD_RESPONSE;
        else status = 0;
    }
    else if (!status) status = NT_BAD_RESPONSE;
    free( reply );
    return status;
}

/* a call of the interface: the fragments of its answer put together */
static NTSTATUS rpc_call( struct session *s, const BYTE *request, UINT request_len, UINT answer_type,
                          struct bytes *stub )
{
    struct bytes in = { 0 };
    UINT pos = 0;
    NTSTATUS status;

    if ((status = transceive( s, request, request_len, &in ))) goto done;
    for (;;)
    {
        UINT frag;

        while (in.len < pos + 16)
            if ((status = read_pipe( s, &in ))) goto done;
        frag = get16( in.data + pos + 8 );
        if (frag < 16) { status = NT_BAD_RESPONSE; goto done; }
        while (in.len < pos + frag)
            if ((status = read_pipe( s, &in ))) goto done;
        if (in.data[pos + 2] != answer_type)
        {
            WARN( "answer of type %u, not %u\n", in.data[pos + 2], answer_type );
            status = in.data[pos + 2] == 3 ? NT_ACCESS_DENIED : NT_BAD_RESPONSE;
            goto done;
        }
        if (stub && frag > 24 && !append( stub, in.data + pos + 24, frag - 24 ))
        {
            status = NT_NO_MEMORY;
            goto done;
        }
        if (in.data[pos + 3] & 2) break;      /* the last one */
        pos += frag;
    }
done:
    free( in.data );
    return status;
}

static NTSTATUS bind_srvsvc( struct session *s )
{
    static const BYTE bind[72] =
    {
        5, 0, 11, 3, 0x10, 0, 0, 0, 72, 0, 0, 0, 1, 0, 0, 0,
        MAX_FRAGMENT & 0xff, MAX_FRAGMENT >> 8, MAX_FRAGMENT & 0xff, MAX_FRAGMENT >> 8, 0, 0, 0, 0,
        1, 0, 0, 0,
        0, 0, 1, 0,
        /* srvsvc 4b324fc8-1670-01d3-1278-5a47bf6ee188, version 3 */
        0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88, 3, 0, 0, 0,
        /* NDR 8a885d04-1ceb-11c9-9fe8-08002b104860, version 2 */
        0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 2, 0, 0, 0,
    };
    return rpc_call( s, bind, sizeof(bind), 12, NULL );
}

static BOOL ndr_string( const struct bytes *stub, UINT *pos, WCHAR *out, UINT count )
{
    UINT actual, n;

    *pos = (*pos + 3) & ~3;
    if (*pos + 12 > stub->len) return FALSE;
    actual = get32( stub->data + *pos + 8 );
    *pos += 12;
    if (actual > 0x8000 || *pos + actual * sizeof(WCHAR) > stub->len) return FALSE;
    n = min( actual, count - 1 );
    memcpy( out, stub->data + *pos, n * sizeof(WCHAR) );
    out[n] = 0;
    *pos += actual * sizeof(WCHAR);
    return TRUE;
}

/* NetrShareEnum, level 1: names, kinds and remarks */
static NTSTATUS enum_shares( struct session *s, const WCHAR *server, struct share **shares, UINT *count )
{
    BYTE request[24 + 700] = { 5, 0, 0, 3, 0x10, 0, 0, 0 }, *p = request + 24;
    struct bytes stub = { 0 };
    struct share *list = NULL;
    WCHAR name[280];
    UINT chars, len, pos, n, i, at;
    NTSTATUS status;

    swprintf( name, ARRAY_SIZE(name), L"\\\\%s", server );
    chars = wcslen( name ) + 1;
    set32( p, 0x00020000 ); p += 4;
    set32( p, chars ); set32( p + 4, 0 ); set32( p + 8, chars ); p += 12;
    memcpy( p, name, chars * sizeof(WCHAR) ); p += chars * sizeof(WCHAR);
    while ((p - request) & 3) *p++ = 0;
    set32( p, 1 ); set32( p + 4, 1 ); p += 8;                   /* level 1 of the union */
    set32( p, 0x00020004 ); set32( p + 4, 0 ); set32( p + 8, 0 ); p += 12;
    set32( p, 0xffffffff ); p += 4;                             /* all of them */
    set32( p, 0 ); p += 4;                                      /* from the start */
    len = p - request;
    set16( request + 8, len );
    set32( request + 12, 2 );
    set32( request + 16, len - 24 );
    set16( request + 22, 15 );                                  /* NetrShareEnum */

    if ((status = rpc_call( s, request, len, 2, &stub ))) goto done;
    status = NT_BAD_RESPONSE;
    if (stub.len < 20 || !get32( stub.data + 8 )) goto done;
    n = get32( stub.data + 12 );
    if (!get32( stub.data + 16 )) n = 0;
    pos = 24;
    if (n > 4096 || pos + n * 12 > stub.len) goto done;
    if (!(list = calloc( n + 1, sizeof(*list) ))) goto done;
    for (i = 0; i < n; i++) list[i].type = get32( stub.data + pos + i * 12 + 4 );
    at = pos + n * 12;
    for (i = 0; i < n; i++)
    {
        const BYTE *entry = stub.data + pos + i * 12;

        if (get32( entry ) && !ndr_string( &stub, &at, list[i].name, ARRAY_SIZE(list[i].name) )) goto done;
        if (get32( entry + 8 ) && !ndr_string( &stub, &at, list[i].remark, ARRAY_SIZE(list[i].remark) )) goto done;
    }
    *shares = list;
    *count = n;
    list = NULL;
    status = 0;
done:
    free( list );
    free( stub.data );
    return status;
}

static SOCKET connect_to( const char *address )
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons( 445 ) };
    struct timeval wait = { 3, 0 };
    DWORD timeout = 8000;
    u_long on = 1, off = 0;
    fd_set writable;
    SOCKET sock;

    if (inet_pton( AF_INET, address, &to.sin_addr ) != 1) return INVALID_SOCKET;
    if ((sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) == INVALID_SOCKET) return INVALID_SOCKET;
    ioctlsocket( sock, FIONBIO, &on );
    if (connect( sock, (struct sockaddr *)&to, sizeof(to) ))
    {
        FD_ZERO( &writable );
        FD_SET( sock, &writable );
        if (WSAGetLastError() != WSAEWOULDBLOCK || select( 0, NULL, &writable, NULL, &wait ) != 1)
        {
            closesocket( sock );
            return INVALID_SOCKET;
        }
    }
    ioctlsocket( sock, FIONBIO, &off );
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout) );
    setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout) );
    return sock;
}

static DWORD error_of( NTSTATUS status )
{
    switch ((UINT)status)
    {
    case 0: return ERROR_SUCCESS;
    case 0xc0000022: /* access denied */
    case 0xc00000a2: /* signing required */
        return ERROR_ACCESS_DENIED;
    case 0xc000006d: /* logon failure */
    case 0xc000006a: /* wrong password */
    case 0xc0000064: /* no such user */
    case 0xc000006e: /* account restriction */
    case 0xc000006f: /* logon hours */
    case 0xc0000070: /* this workstation not allowed */
    case 0xc0000071: /* password expired */
    case 0xc0000072: /* account disabled */
    case 0xc000015b: /* logon type not granted */
    case 0xc0000193: /* account expired */
    case 0xc0000224: /* password must change */
    case 0xc0000234: /* account locked out */
        return ERROR_LOGON_FAILURE;
    case 0xc00000cc: return ERROR_BAD_NET_NAME;
    case 0xc00000bb: return ERROR_NOT_SUPPORTED;
    default: return ERROR_UNEXP_NET_ERR;
    }
}

DWORD smb_enum_shares( const WCHAR *server, const char *address, const struct logon *logon,
                       struct share **shares, UINT *count )
{
    struct session s = { 0 };
    NTSTATUS status;

    *shares = NULL;
    *count = 0;
    if ((s.sock = connect_to( address )) == INVALID_SOCKET) return ERROR_BAD_NETPATH;
    if (!(status = negotiate( &s )) && !(status = log_on( &s, logon )) && !(status = tree_connect( &s, server )) &&
        !(status = open_pipe( &s, L"srvsvc" )))
    {
        if (!(status = bind_srvsvc( &s ))) status = enum_shares( &s, server, shares, count );
        close_pipe( &s );
    }
    closesocket( s.sock );
    TRACE( "%s at %s as %s: status %#lx, %u shares\n", debugstr_w( server ), address,
           debugstr_w( logon ? logon->user : NULL ), status, *count );
    return error_of( status );
}
