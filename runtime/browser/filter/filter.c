/*
 * Arctic web engine: the filter, in the syntax of uBlock Origin
 * (docs/browser.md, component 3, the network half)
 *
 * The rules are those of EasyList and EasyPrivacy, which uBlock Origin reads
 * too: ||host^, |http..., * and ^, exceptions @@, and the options
 * $third-party, $domain=, the request types, $important and $match-case. A
 * rule with an option this engine does not know is dropped, as uBlock Origin
 * drops it: a rule half understood blocks what it should not. Cosmetic
 * rules (##) are stage 7. None of uBlock Origin's code is here (it is
 * GPL-3.0); only the syntax of the lists and their meaning.
 *
 * A rule that is a host and nothing else - most of the lists - goes into a
 * hash of hosts, and a request asks it for every parent of its host. The
 * rest go into buckets by their longest token, as uBlock Origin does, and a
 * request looks only into the buckets of its own tokens.
 *
 * The lists live in %LOCALAPPDATA%\Arctic\Internet Explorer\Filters, where
 * any user may write on any Windows, and are fetched again in the background
 * when they are older than four days.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "arctichtml_private.h"
#include "../net/net.h"

#include <shlobj.h>

#define LIST_MAX_AGE (4 * 24 * 3600ull * 10000000ull)   /* in FILETIME units */

static const struct { const WCHAR *file; const char *url; } lists[] =
{
    { L"easylist.txt",    "https://easylist.to/easylist/easylist.txt" },
    { L"easyprivacy.txt", "https://easylist.to/easylist/easyprivacy.txt" },
};

enum
{
    TYPE_DOCUMENT   = 1 << REQUEST_DOCUMENT,
    TYPE_STYLESHEET = 1 << REQUEST_STYLESHEET,
    TYPE_IMAGE      = 1 << REQUEST_IMAGE,
    TYPE_SCRIPT     = 1 << REQUEST_SCRIPT,
    TYPE_OTHER      = 1 << REQUEST_OTHER,
    TYPE_ALL_BUT_DOCUMENT = TYPE_STYLESHEET | TYPE_IMAGE | TYPE_SCRIPT | TYPE_OTHER,
};

enum party { PARTY_ANY, PARTY_THIRD, PARTY_FIRST };

struct rule
{
    char        *pattern;       /* lower case unless match_case; NULL for a host rule */
    BOOL         start, end;    /* |...| anchors */
    BOOL         host_anchor;   /* ||... */
    BOOL         exception, important, match_case;
    enum party   party;
    unsigned int types;         /* TYPE_*; 0 = every type but document */
    char        *domains;       /* "a.com|~b.com" of $domain=, or NULL */
};

struct bucket
{
    unsigned int  hash;
    unsigned int *rules;
    unsigned int  count, capacity;
};

struct engine
{
    struct rule   *rules;
    unsigned int   rule_count, rule_capacity;
    struct bucket *buckets;     /* open addressing, by token hash */
    unsigned int   bucket_size;
    struct bucket *hosts;       /* the same, by host hash: rules that are a host */
    unsigned int   host_size;
    unsigned int  *untokenized; /* rules with no token to file them under */
    unsigned int   untokenized_count, untokenized_capacity;
};

static struct engine *current;
static SRWLOCK engine_lock = SRWLOCK_INIT;
static HANDLE loaded_event;

static unsigned int hash_bytes( const char *s, size_t len )
{
    unsigned int h = 2166136261u;

    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ? h : 1;
}

static BOOL is_token_char( char c )
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%' || (c >= 'A' && c <= 'Z');
}

/* --- building --- */

static struct bucket *bucket_for( struct bucket **table, unsigned int *size, unsigned int hash, BOOL create );

static BOOL grow_table( struct bucket **table, unsigned int *size )
{
    unsigned int old_size = *size, new_size = old_size ? old_size * 2 : 4096;
    struct bucket *old = *table, *grown = calloc( new_size, sizeof(*grown) );

    if (!grown) return FALSE;
    *table = grown;
    *size = new_size;
    for (unsigned int i = 0; i < old_size; i++)
    {
        struct bucket *b;

        if (!old[i].hash) continue;
        b = bucket_for( table, size, old[i].hash, TRUE );
        *b = old[i];
    }
    free( old );
    return TRUE;
}

static struct bucket *bucket_for( struct bucket **table, unsigned int *size, unsigned int hash, BOOL create )
{
    if (!*size)
    {
        if (!create || !grow_table( table, size )) return NULL;
    }
    for (unsigned int i = hash & (*size - 1), n = 0; n < *size; i = (i + 1) & (*size - 1), n++)
    {
        struct bucket *b = &(*table)[i];

        if (b->hash == hash) return b;
        if (!b->hash)
        {
            if (!create) return NULL;
            b->hash = hash;
            return b;
        }
    }
    return NULL;
}

static void bucket_add( struct bucket **table, unsigned int *size, unsigned int *count_in_table,
                        unsigned int hash, unsigned int rule )
{
    struct bucket *b;

    if ((*count_in_table + 1) * 2 > *size && !grow_table( table, size )) return;
    if (!(b = bucket_for( table, size, hash, TRUE ))) return;
    if (!b->count) (*count_in_table)++;
    if (b->count == b->capacity)
    {
        unsigned int capacity = b->capacity ? b->capacity * 2 : 2;
        unsigned int *grown = realloc( b->rules, capacity * sizeof(*grown) );

        if (!grown) return;
        b->rules = grown;
        b->capacity = capacity;
    }
    b->rules[b->count++] = rule;
}

static unsigned int bucket_fill, host_fill;

static BOOL parse_options( struct rule *rule, char *options )
{
    for (char *option = strtok( options, "," ); option; option = strtok( NULL, "," ))
    {
        BOOL negated = option[0] == '~';
        char *name = negated ? option + 1 : option;
        unsigned int type = 0;

        if (!strcmp( name, "third-party" ) || !strcmp( name, "3p" )) rule->party = negated ? PARTY_FIRST : PARTY_THIRD;
        else if (!strcmp( name, "first-party" ) || !strcmp( name, "1p" )) rule->party = negated ? PARTY_THIRD : PARTY_FIRST;
        else if (!strncmp( name, "domain=", 7 ) && !negated) rule->domains = _strdup( name + 7 );
        else if (!strcmp( name, "important" )) rule->important = TRUE;
        else if (!strcmp( name, "match-case" )) rule->match_case = TRUE;
        else if (!strcmp( name, "script" )) type = TYPE_SCRIPT;
        else if (!strcmp( name, "image" )) type = TYPE_IMAGE;
        else if (!strcmp( name, "stylesheet" ) || !strcmp( name, "css" )) type = TYPE_STYLESHEET;
        else if (!strcmp( name, "document" ) || !strcmp( name, "doc" )) type = TYPE_DOCUMENT;
        else if (!strcmp( name, "xmlhttprequest" ) || !strcmp( name, "xhr" ) || !strcmp( name, "subdocument" ) ||
                 !strcmp( name, "frame" ) || !strcmp( name, "font" ) || !strcmp( name, "media" ) ||
                 !strcmp( name, "object" ) || !strcmp( name, "other" ) || !strcmp( name, "ping" ) ||
                 !strcmp( name, "websocket" ))
            type = TYPE_OTHER;
        else if (!strncmp( name, "redirect=", 9 ) || !strcmp( name, "all" )) { /* blocks as it is */ }
        else return FALSE;  /* an option of meaning unknown here: no rule at all */

        if (type)
        {
            if (negated) rule->types = (rule->types ? rule->types : TYPE_ALL_BUT_DOCUMENT) & ~type;
            else rule->types |= type;
        }
    }
    return TRUE;
}

/* the longest token of a pattern that is whole: not cut by a * at either side */
static BOOL best_token( const char *pattern, unsigned int *hash )
{
    size_t best_len = 0;
    const char *best = NULL;

    for (const char *p = pattern; *p; )
    {
        const char *start;

        while (*p && !is_token_char( *p )) p++;
        start = p;
        while (*p && is_token_char( *p )) p++;
        if (p - start < 2) continue;
        if (start > pattern && start[-1] == '*') continue;
        if (*p == '*') continue;
        if ((size_t)(p - start) > best_len)
        {
            best = start;
            best_len = p - start;
        }
    }
    if (!best) return FALSE;
    *hash = hash_bytes( best, best_len );
    return TRUE;
}

static void add_line( struct engine *engine, char *line )
{
    struct rule rule = { 0 };
    char *dollar, *pattern;
    size_t len;
    unsigned int index, hash;

    while (*line == ' ' || *line == '\t') line++;
    len = strlen( line );
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len || line[0] == '!' || line[0] == '[') return;
    if (strstr( line, "##" ) || strstr( line, "#@#" ) || strstr( line, "#?#" ) || strstr( line, "#$#" ) ||
        strstr( line, "#%#" ))
        return;     /* cosmetic: stage 7 */

    if (line[0] == '@' && line[1] == '@')
    {
        rule.exception = TRUE;
        line += 2;
    }
    if (line[0] == '/' && len > 1 && line[strlen( line ) - 1] == '/') return;     /* a regular expression: later */

    if ((dollar = strrchr( line, '$' )) && dollar[1] && !strchr( dollar, '/' ))
    {
        *dollar = 0;
        if (!parse_options( &rule, dollar + 1 ))
        {
            free( rule.domains );
            return;
        }
    }
    pattern = line;
    if (pattern[0] == '|' && pattern[1] == '|') { rule.host_anchor = TRUE; pattern += 2; }
    else if (pattern[0] == '|') { rule.start = TRUE; pattern++; }
    len = strlen( pattern );
    if (len && pattern[len - 1] == '|') { rule.end = TRUE; pattern[--len] = 0; }
    while (len && pattern[len - 1] == '*') pattern[--len] = 0;      /* a * at the end means nothing */
    if (!rule.match_case) for (char *p = pattern; *p; p++) *p = (char)tolower( (unsigned char)*p );

    if (engine->rule_count == engine->rule_capacity)
    {
        unsigned int capacity = engine->rule_capacity ? engine->rule_capacity * 2 : 4096;
        struct rule *grown = realloc( engine->rules, capacity * sizeof(*grown) );

        if (!grown) { free( rule.domains ); return; }
        engine->rules = grown;
        engine->rule_capacity = capacity;
    }
    index = engine->rule_count;

    /* ||host^ and nothing else: into the hash of hosts */
    if (rule.host_anchor && len > 1 && pattern[len - 1] == '^' && !strpbrk( pattern, "*/|" ) &&
        strcspn( pattern, "^" ) == len - 1)
    {
        engine->rules[engine->rule_count++] = rule;
        bucket_add( &engine->hosts, &engine->host_size, &host_fill, hash_bytes( pattern, len - 1 ), index );
        return;
    }
    if (!len && !rule.domains) { return; }  /* a rule that would take everything */
    rule.pattern = _strdup( pattern );
    engine->rules[engine->rule_count++] = rule;
    if (best_token( pattern, &hash )) bucket_add( &engine->buckets, &engine->bucket_size, &bucket_fill, hash, index );
    else
    {
        if (engine->untokenized_count == engine->untokenized_capacity)
        {
            unsigned int capacity = engine->untokenized_capacity ? engine->untokenized_capacity * 2 : 64;
            unsigned int *grown = realloc( engine->untokenized, capacity * sizeof(*grown) );

            if (!grown) return;
            engine->untokenized = grown;
            engine->untokenized_capacity = capacity;
        }
        engine->untokenized[engine->untokenized_count++] = index;
    }
}

/* --- matching --- */

static BOOL is_separator( char c )
{
    return !c || !(is_token_char( c ) || c == '_' || c == '-' || c == '.' );
}

/* a pattern with * and ^ against the address from a position on */
static BOOL glob( const char *p, const char *s, BOOL end_anchor )
{
    while (*p)
    {
        if (*p == '*')
        {
            while (*p == '*') p++;
            if (!*p) return TRUE;
            for (; *s; s++) if (glob( p, s, end_anchor )) return TRUE;
            return glob( p, s, end_anchor );
        }
        if (*p == '^')
        {
            if (*s && !is_separator( *s )) return FALSE;
            if (*s) s++;
            p++;
            continue;
        }
        if (*p != *s) return FALSE;
        p++;
        s++;
    }
    return !end_anchor || !*s;
}

static BOOL rule_matches_url( const struct rule *rule, const char *url, const char *lower, const char *host_start,
                              const char *host_end )
{
    const char *subject = rule->match_case ? url : lower;
    size_t offset;

    if (rule->host_anchor)
    {
        /* at the start of the host, or after a dot of it */
        for (const char *p = host_start; p < host_end; p++)
        {
            if (p != host_start && p[-1] != '.') continue;
            offset = p - lower;
            if (glob( rule->pattern, subject + offset, rule->end )) return TRUE;
        }
        return FALSE;
    }
    if (rule->start) return glob( rule->pattern, subject, rule->end );
    for (const char *p = subject; *p; p++) if (glob( rule->pattern, p, rule->end )) return TRUE;
    return FALSE;
}

/* example.co.uk and www.example.co.uk are one site; without a list of
 * suffixes the usual two-level ones are known by name */
static const char *site_of( const char *host )
{
    static const char *const second_level[] =
    {
        "co.uk", "org.uk", "ac.uk", "gov.uk", "com.ua", "org.ua", "net.ua", "gov.ua", "in.ua", "kiev.ua", "co.jp",
        "ne.jp", "or.jp", "com.au", "net.au", "org.au", "com.br", "com.cn", "com.tr", "co.kr", "com.pl", "co.in",
    };
    const char *last = strrchr( host, '.' ), *second = NULL;

    if (!last) return host;
    for (const char *p = host; p < last; p++) if (*p == '.') second = p;
    if (!second) return host;
    for (unsigned int i = 0; i < ARRAY_SIZE(second_level); i++)
    {
        if (_stricmp( second + 1, second_level[i] )) continue;
        for (const char *p = second - 1; p >= host; p--) if (*p == '.') return p + 1;
        return host;
    }
    return second + 1;
}

static BOOL host_in( const char *host, const char *domain, size_t len )
{
    size_t host_len = strlen( host );

    if (host_len < len || _strnicmp( host + host_len - len, domain, len )) return FALSE;
    return host_len == len || host[host_len - len - 1] == '.';
}

static BOOL rule_applies( const struct rule *rule, enum request_type type, BOOL third_party, const char *page_host )
{
    unsigned int types = rule->types ? rule->types : TYPE_ALL_BUT_DOCUMENT;

    if (!(types & (1u << type))) return FALSE;
    if (rule->party == PARTY_THIRD && !third_party) return FALSE;
    if (rule->party == PARTY_FIRST && third_party) return FALSE;
    if (rule->domains)
    {
        BOOL any_positive = FALSE, included = FALSE;

        for (const char *d = rule->domains; *d; )
        {
            BOOL negated = *d == '~';
            const char *start = negated ? d + 1 : d;
            size_t len = strcspn( start, "|" );

            if (!negated) any_positive = TRUE;
            if (host_in( page_host, start, len ))
            {
                if (negated) return FALSE;
                included = TRUE;
            }
            d = start + len + (start[len] == '|');
        }
        if (any_positive && !included) return FALSE;
    }
    return TRUE;
}

/* the first rule of a kind that takes the request, or NULL */
static const struct rule *find( const struct engine *engine, BOOL exception, const char *url, const char *lower,
                                const char *host, const char *host_start, const char *host_end,
                                enum request_type type, BOOL third_party, const char *page_host )
{
    const struct bucket *b;

    /* the host and its parents in the hash of hosts */
    for (const char *h = host; h; h = strchr( h, '.' ) ? strchr( h, '.' ) + 1 : NULL)
    {
        struct bucket *table = engine->hosts;
        unsigned int size = engine->host_size;

        if (!(b = bucket_for( &table, &size, hash_bytes( h, strlen( h ) ), FALSE ))) continue;
        for (unsigned int i = 0; i < b->count; i++)
        {
            const struct rule *rule = &engine->rules[b->rules[i]];

            if (rule->exception == exception && rule_applies( rule, type, third_party, page_host )) return rule;
        }
    }
    /* the buckets of the address's tokens */
    for (const char *p = lower; *p; )
    {
        const char *start;
        struct bucket *table = engine->buckets;
        unsigned int size = engine->bucket_size;

        while (*p && !is_token_char( *p )) p++;
        start = p;
        while (*p && is_token_char( *p )) p++;
        if (p - start < 2) continue;
        if (!(b = bucket_for( &table, &size, hash_bytes( start, p - start ), FALSE ))) continue;
        for (unsigned int i = 0; i < b->count; i++)
        {
            const struct rule *rule = &engine->rules[b->rules[i]];

            if (rule->exception == exception && rule_applies( rule, type, third_party, page_host ) &&
                rule_matches_url( rule, url, lower, host_start, host_end ))
                return rule;
        }
    }
    for (unsigned int i = 0; i < engine->untokenized_count; i++)
    {
        const struct rule *rule = &engine->rules[engine->untokenized[i]];

        if (rule->exception == exception && rule_applies( rule, type, third_party, page_host ) &&
            rule_matches_url( rule, url, lower, host_start, host_end ))
            return rule;
    }
    return NULL;
}

BOOL filter_blocks( const char *url, const struct url_parts *parts, const char *page, enum request_type type )
{
    struct url_parts page_parts;
    const struct rule *block, *allow;
    const char *host_start, *host_end;
    char *lower;
    BOOL third_party, blocked = FALSE;

    if (loaded_event) WaitForSingleObject( loaded_event, 2000 );    /* the lists on the disk, not the download */
    if (!page || !url_parse( page, &page_parts )) return FALSE;
    if (!(lower = _strdup( url ))) return FALSE;
    for (char *p = lower; *p; p++) *p = (char)tolower( (unsigned char)*p );
    host_start = strstr( lower, "//" );
    host_start = host_start ? host_start + 2 : lower;
    host_end = host_start + strcspn( host_start, "/?#:" );
    third_party = _stricmp( site_of( parts->host ), site_of( page_parts.host ) ) != 0;

    AcquireSRWLockShared( &engine_lock );
    if (current &&
        (block = find( current, FALSE, url, lower, parts->host, host_start, host_end, type, third_party, page_parts.host )))
    {
        blocked = TRUE;
        if (!block->important &&
            (allow = find( current, TRUE, url, lower, parts->host, host_start, host_end, type, third_party,
                           page_parts.host )))
            blocked = FALSE;
    }
    ReleaseSRWLockShared( &engine_lock );
    if (blocked) TRACE( "blocked %s\n", url );
    free( lower );
    return blocked;
}

unsigned int filter_rule_count(void)
{
    unsigned int count;

    AcquireSRWLockShared( &engine_lock );
    count = current ? current->rule_count : 0;
    ReleaseSRWLockShared( &engine_lock );
    return count;
}

/* --- the lists --- */

static BOOL lists_folder( WCHAR path[MAX_PATH] )
{
    WCHAR *local;

    if (FAILED(SHGetKnownFolderPath( &FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &local ))) return FALSE;
    swprintf( path, MAX_PATH, L"%s\\Arctic\\Internet Explorer\\Filters", local );
    CoTaskMemFree( local );
    SHCreateDirectoryExW( NULL, path, NULL );
    return TRUE;
}

static void add_text( struct engine *engine, char *text )
{
    for (char *line = text, *next; line && *line; line = next)
    {
        next = strchr( line, '\n' );
        if (next) *next++ = 0;
        add_line( engine, line );
    }
}

static struct engine *build( const WCHAR *folder )
{
    struct engine *engine = calloc( 1, sizeof(*engine) );

    if (!engine) return NULL;
    bucket_fill = host_fill = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(lists); i++)
    {
        WCHAR path[MAX_PATH];
        HANDLE file;
        LARGE_INTEGER size;
        DWORD got;
        char *text;

        swprintf( path, MAX_PATH, L"%s\\%s", folder, lists[i].file );
        file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        if (GetFileSizeEx( file, &size ) && size.QuadPart < 64 * 1024 * 1024 &&
            (text = malloc( (size_t)size.QuadPart + 1 )))
        {
            if (ReadFile( file, text, (DWORD)size.QuadPart, &got, NULL ))
            {
                text[got] = 0;
                add_text( engine, text );
            }
            free( text );
        }
        CloseHandle( file );
    }
    return engine;
}

static void engine_free( struct engine *engine )
{
    if (!engine) return;
    for (unsigned int i = 0; i < engine->rule_count; i++)
    {
        free( engine->rules[i].pattern );
        free( engine->rules[i].domains );
    }
    for (unsigned int i = 0; i < engine->bucket_size; i++) free( engine->buckets[i].rules );
    for (unsigned int i = 0; i < engine->host_size; i++) free( engine->hosts[i].rules );
    free( engine->rules );
    free( engine->buckets );
    free( engine->hosts );
    free( engine->untokenized );
    free( engine );
}

static void install( struct engine *engine )
{
    struct engine *old;

    AcquireSRWLockExclusive( &engine_lock );
    old = current;
    current = engine;
    ReleaseSRWLockExclusive( &engine_lock );
    engine_free( old );
}

static BOOL list_is_old( const WCHAR *path )
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    FILETIME now;
    ULARGE_INTEGER a, b;

    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &data )) return TRUE;
    GetSystemTimeAsFileTime( &now );
    a.LowPart = data.ftLastWriteTime.dwLowDateTime; a.HighPart = data.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart - a.QuadPart > LIST_MAX_AGE;
}

static DWORD WINAPI load_thread( void *arg )
{
    WCHAR folder[MAX_PATH];
    ULONGLONG start = GetTickCount64();
    BOOL fetched = FALSE;

    if (!lists_folder( folder ))
    {
        SetEvent( loaded_event );
        return 0;
    }
    install( build( folder ) );
    TRACE( "%u rules from the disk in %I64u ms\n", filter_rule_count(), GetTickCount64() - start );
    SetEvent( loaded_event );

    for (unsigned int i = 0; i < ARRAY_SIZE(lists); i++)
    {
        WCHAR path[MAX_PATH], temp[MAX_PATH];
        struct response response;
        HANDLE file;
        DWORD written;

        swprintf( path, MAX_PATH, L"%s\\%s", folder, lists[i].file );
        if (!list_is_old( path )) continue;
        if (!net_fetch( lists[i].url, NULL, REQUEST_OTHER, &response ) || response.status != 200 ||
            response.length < 1024)
        {
            WARN( "%s: %d %s\n", lists[i].url, response.status, response.error );
            response_free( &response );
            continue;
        }
        swprintf( temp, MAX_PATH, L"%s.new", path );
        file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (file != INVALID_HANDLE_VALUE)
        {
            BOOL ok = WriteFile( file, response.body, (DWORD)response.length, &written, NULL ) &&
                      written == response.length;

            CloseHandle( file );
            if (ok && MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING )) fetched = TRUE;
            else DeleteFileW( temp );
        }
        response_free( &response );
    }
    if (fetched)
    {
        start = GetTickCount64();
        install( build( folder ) );
        TRACE( "%u rules after the download, in %I64u ms\n", filter_rule_count(), GetTickCount64() - start );
    }
    return 0;
}

void filter_init(void)
{
    HANDLE thread;

    loaded_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if ((thread = CreateThread( NULL, 0, load_thread, NULL, 0, NULL ))) CloseHandle( thread );
    else SetEvent( loaded_event );
}
