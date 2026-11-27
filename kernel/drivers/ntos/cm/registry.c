// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Server-side registry management
 *
 * Copyright (C) 1999 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* To do:
 * - symbolic links
 */

#include "ntos.h"  /* ntos: all that the server's headers and the C library gave */

#ifdef HAVE_SYS_PERSONALITY_H
#endif

struct notify
{
    struct list       entry;    /* entry in list of notifications */
    struct event    **events;   /* events to set when changing this key */
    unsigned int      event_count; /* number of events */
    int               subtree;  /* true if subtree notification */
    unsigned int      filter;   /* which events to notify on */
    obj_handle_t      hkey;     /* hkey associated with this notification */
    struct process   *process;  /* process in which the hkey is valid */
};

static const WCHAR key_name[] = {'K','e','y'};

struct type_descr key_type =
{
    { key_name, sizeof(key_name) },   /* name */
    KEY_ALL_ACCESS | SYNCHRONIZE,     /* valid_access */
    {                                 /* mapping */
        STANDARD_RIGHTS_READ | KEY_NOTIFY | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
        STANDARD_RIGHTS_WRITE | KEY_CREATE_SUB_KEY | KEY_SET_VALUE,
        STANDARD_RIGHTS_EXECUTE | KEY_CREATE_LINK | KEY_NOTIFY | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
        KEY_ALL_ACCESS
    },
};

/* a registry key */
struct nt_key
{
    struct object     obj;         /* object header */
    WCHAR            *class;       /* key class */
    data_size_t       classlen;    /* length of class name */
    int               last_subkey; /* last in use subkey */
    int               nb_subkeys;  /* count of allocated subkeys */
    struct nt_key      **subkeys;     /* subkeys array */
    struct nt_key       *wow6432node; /* Wow6432Node subkey */
    int               last_value;  /* last in use value */
    int               nb_values;   /* count of allocated values in array */
    struct key_value *values;      /* values array */
    unsigned int      flags;       /* flags */
    timeout_t         modif;       /* last modification time */
    struct list       notify_list; /* list of notifications */
    object_shm_t     *shared;      /* Arctic: change counter in session memory, for client caches */
};

/* key flags */
#define KEY_VOLATILE 0x0001  /* key is volatile (not saved to disk) */
#define KEY_DELETED  0x0002  /* key has been deleted */
#define KEY_DIRTY    0x0004  /* key has been modified */
#define KEY_SYMLINK  0x0008  /* key is a symbolic link */
#define KEY_WOWSHARE 0x0010  /* key is a Wow64 shared key (used for Software\Classes) */
#define KEY_PREDEF   0x0020  /* key is marked as predefined */

#define OBJ_KEY_WOW64 0x100000 /* magic flag added to attributes for WoW64 redirection */

/* a key value */
struct key_value
{
    WCHAR            *name;    /* value name */
    unsigned short    namelen; /* length of value name */
    unsigned int      type;    /* value type */
    data_size_t       len;     /* value data length in bytes */
    void             *data;    /* pointer to value data */
};

#define MIN_SUBKEYS  8   /* min. number of allocated subkeys per key */
#define MIN_VALUES   8   /* min. number of allocated values per key */

#define MAX_NAME_LEN  256    /* max. length of a key name */
#define MAX_VALUE_LEN 16383  /* max. length of a value name */

/* the root of the registry tree */
static struct nt_key *root_key;

static const timeout_t ticks_1601_to_1970 = (timeout_t)86400 * (369 * 365 + 89) * TICKS_PER_SEC;
static timeout_t save_period = 30 * -TICKS_PER_SEC;  /* delay between periodic saves */
static struct timeout_user *save_timeout_user;  /* saving timer */
static enum prefix_type { PREFIX_UNKNOWN, PREFIX_32BIT, PREFIX_64BIT } prefix_type;

static const WCHAR wow6432node[] = {'W','o','w','6','4','3','2','N','o','d','e'};
static const WCHAR symlink_value[] = {'S','y','m','b','o','l','i','c','L','i','n','k','V','a','l','u','e'};
static const struct unicode_str symlink_str = { symlink_value, sizeof(symlink_value) };

static void set_periodic_save_timer(void);
static struct key_value *find_value( const struct nt_key *key, struct unicode_str name, int *index );

/* information about where to save a registry branch */
struct save_branch_info
{
    struct nt_key  *key;
    const char  *filename;
};

#define MAX_SAVE_BRANCH_INFO 3
static int save_branch_count;
static struct save_branch_info save_branch_info[MAX_SAVE_BRANCH_INFO];

unsigned int supported_machines_count = 0;
unsigned short supported_machines[8];
unsigned short native_machine = 0;

/* information about a file being loaded */
struct file_load_info
{
    const char *filename; /* input file name */
    FILE       *file;     /* input file */
    char       *buffer;   /* line buffer */
    int         len;      /* buffer length */
    int         line;     /* nt_current input line */
    WCHAR      *tmp;      /* temp buffer to use while parsing input */
    size_t      tmplen;   /* length of temp buffer */
};

struct key_init_data
{
    unsigned int options;
    data_size_t  classlen;
    const WCHAR *class;
    timeout_t    modif;
};

static void key_dump( struct object *obj, int verbose );
static bool nt_key_init( struct object *obj, const void *init_data );
static unsigned int key_map_access( struct object *obj, unsigned int access );
static struct security_descriptor *key_get_sd( struct object *obj );
static WCHAR *key_get_full_name( struct object *obj, data_size_t max, data_size_t *len );
static struct object *key_lookup_name( struct object *obj, struct unicode_str *name,
                                       unsigned int attr, struct object *root );
static int key_link_name( struct object *obj, struct object_name *name, struct object *parent );
static void key_unlink_name( struct object *obj, struct object_name *name );
static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void key_destroy( struct object *obj );

static const struct object_ops key_ops =
{
    .size          = sizeof(struct nt_key),
    .type          = &key_type,
    .dump          = key_dump,
    .init          = nt_key_init,
    .map_access    = key_map_access,
    .get_sd        = key_get_sd,
    .get_full_name = key_get_full_name,
    .lookup_name   = key_lookup_name,
    .link_name     = key_link_name,
    .unlink_name   = key_unlink_name,
    .close_handle  = key_close_handle,
    .destroy       = key_destroy,
};

static inline int is_wow6432node( const WCHAR *name, unsigned int len )
{
    len = get_path_element( name, len );
    return (len == sizeof(wow6432node) && !memicmp_strW( name, wow6432node, sizeof( wow6432node )));
}

static inline struct nt_key *get_parent( const struct nt_key *key )
{
    struct object *parent = key->obj.name->parent;

    if (!parent || parent->ops != &key_ops) return NULL;
    return (struct nt_key *)parent;
}

/*
 * The registry text file format v2 used by this code is similar to the one
 * used by REGEDIT import/export functionality, with the following differences:
 * - strings and key names can contain \x escapes for Unicode
 * - key names use escapes too in order to support Unicode
 * - the modification time optionally follows the key name
 * - REG_EXPAND_SZ and REG_MULTI_SZ are saved as strings instead of hex
 */

/* dump the full path of a key */
static void dump_path( const struct nt_key *key, const struct nt_key *base, FILE *f )
{
    if (get_parent( key ) && get_parent( key ) != base)
    {
        dump_path( get_parent( key ), base, f );
        nt_fprintf( f, "\\\\" );
    }
    dump_strW( key->obj.name->name, key->obj.name->len, f, "[]" );
}

/* dump a value to a text file */
static void dump_value( const struct key_value *value, FILE *f )
{
    unsigned int i, dw;
    int count;

    if (value->namelen)
    {
        nt_fputc( '\"', f );
        count = 1 + dump_strW( value->name, value->namelen, f, "\"\"" );
        count += nt_fprintf( f, "\"=" );
    }
    else count = nt_fprintf( f, "@=" );

    switch(value->type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:
        /* only output properly terminated strings in string format */
        if (value->len < sizeof(WCHAR)) break;
        if (value->len % sizeof(WCHAR)) break;
        if (((WCHAR *)value->data)[value->len / sizeof(WCHAR) - 1]) break;
        if (value->type != REG_SZ) nt_fprintf( f, "str(%x):", value->type );
        nt_fputc( '\"', f );
        dump_strW( (WCHAR *)value->data, value->len - sizeof(WCHAR), f, "\"\"" );
        nt_fprintf( f, "\"\n" );
        return;

    case REG_DWORD:
        if (value->len != sizeof(dw)) break;
        memcpy( &dw, value->data, sizeof(dw) );
        nt_fprintf( f, "dword:%08x\n", dw );
        return;
    }

    if (value->type == REG_BINARY) count += nt_fprintf( f, "hex:" );
    else count += nt_fprintf( f, "hex(%x):", value->type );
    for (i = 0; i < value->len; i++)
    {
        count += nt_fprintf( f, "%02x", *((unsigned char *)value->data + i) );
        if (i < value->len-1)
        {
            nt_fputc( ',', f );
            if (++count > 76)
            {
                nt_fprintf( f, "\\\n  " );
                count = 2;
            }
        }
    }
    nt_fputc( '\n', f );
}

/* find the named child of a given key and return its index */
static struct nt_key *find_subkey( const struct nt_key *key, struct unicode_str name, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_subkey;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->subkeys[i]->obj.name->len, name.len );
        res = memicmp_strW( key->subkeys[i]->obj.name->name, name.str, len );
        if (!res) res = key->subkeys[i]->obj.name->len - name.len;
        if (!res)
        {
            *index = i;
            return key->subkeys[i];
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    *index = min;  /* this is where we should insert it */
    return NULL;
}

/* try to grow the array of subkeys; return 1 if OK, 0 on error */
static int grow_subkeys( struct nt_key *key )
{
    struct nt_key **new_subkeys;
    int nb_subkeys;

    if (key->nb_subkeys)
    {
        nb_subkeys = key->nb_subkeys + (key->nb_subkeys / 2);  /* grow by 50% */
        if (!(new_subkeys = nt_realloc( key->subkeys, nb_subkeys * sizeof(*new_subkeys) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = mem_alloc( nb_subkeys * sizeof(*new_subkeys) ))) return 0;
    }
    key->subkeys    = new_subkeys;
    key->nb_subkeys = nb_subkeys;
    return 1;
}

/* save a registry and all its subkeys to a text file */
static void save_subkeys( const struct nt_key *key, const struct nt_key *base, FILE *f )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    /* save key if it has either some values or no subkeys, or needs special options */
    /* keys with no values but subkeys are saved implicitly by saving the subkeys */
    if ((key->last_value >= 0) || (key->last_subkey == -1) || key->class || (key->flags & KEY_SYMLINK))
    {
        nt_fprintf( f, "\n[" );
        if (key != base) dump_path( key, base, f );
        nt_fprintf( f, "] %u\n", (unsigned int)((key->modif - ticks_1601_to_1970) / TICKS_PER_SEC) );
        nt_fprintf( f, "#time=%x%08x\n", (unsigned int)(key->modif >> 32), (unsigned int)key->modif );
        if (key->class)
        {
            nt_fprintf( f, "#class=\"" );
            dump_strW( key->class, key->classlen, f, "\"\"" );
            nt_fprintf( f, "\"\n" );
        }
        if (key->flags & KEY_SYMLINK) nt_fputs( "#link\n", f );
        for (i = 0; i <= key->last_value; i++) dump_value( &key->values[i], f );
    }
    for (i = 0; i <= key->last_subkey; i++) save_subkeys( key->subkeys[i], base, f );
}

static void dump_operation( const struct nt_key *key, const struct key_value *value, const char *op )
{
    nt_fprintf( stderr, "%s key ", op );
    if (key) dump_path( key, NULL, stderr );
    else nt_fprintf( stderr, "ERROR" );
    if (value)
    {
        nt_fprintf( stderr, " value ");
        dump_value( value, stderr );
    }
    else nt_fprintf( stderr, "\n" );
}

static void key_dump( struct object *obj, int verbose )
{
    struct nt_key *key = (struct nt_key *)obj;
    assert( obj->ops == &key_ops );
    nt_fprintf( stderr, "Key flags=%x ", key->flags );
    dump_path( key, NULL, stderr );
    nt_fprintf( stderr, "\n" );
}

static bool nt_key_init( struct object *obj, const void *init_data )
{
    struct nt_key *key = (struct nt_key *)obj;
    struct nt_key *parent = get_parent( key );
    const struct key_init_data *data = init_data;

    key->class       = NULL;
    key->flags       = 0;
    key->last_subkey = -1;
    key->nb_subkeys  = 0;
    key->subkeys     = NULL;
    key->wow6432node = NULL;
    key->nb_values   = 0;
    key->last_value  = -1;
    key->values      = NULL;
    key->modif       = data->modif;
    key->shared      = NULL;
    wlist_init( &key->notify_list );

    if ((key->classlen = (data->classlen / sizeof(WCHAR)) * sizeof(WCHAR)) &&
        !(key->class = memdup( data->class, key->classlen ))) return false;

    if (data->options & REG_OPTION_CREATE_LINK) key->flags |= KEY_SYMLINK;
    if (data->options & REG_OPTION_VOLATILE) key->flags |= KEY_VOLATILE;
    else if (parent && parent->flags & KEY_VOLATILE)
    {
        set_error( STATUS_CHILD_MUST_BE_VOLATILE );
        return false;
    }
    else key->flags |= KEY_DIRTY;

    return true;
}

/* notify waiter and maybe delete the notification */
static void do_notification( struct nt_key *key, struct notify *notify, int del )
{
    unsigned int i;

    for (i = 0; i < notify->event_count; ++i)
    {
        set_event( notify->events[i] );
        release_object( notify->events[i] );
    }
    nt_free( notify->events );
    notify->events = NULL;
    notify->event_count = 0;

    if (del)
    {
        wlist_remove( &notify->entry );
        nt_free( notify );
    }
}

static inline struct notify *find_notify( struct nt_key *key, struct process *process, obj_handle_t hkey )
{
    struct notify *notify;

    LIST_FOR_EACH_ENTRY( notify, &key->notify_list, struct notify, entry )
    {
        if (notify->process == process && notify->hkey == hkey) return notify;
    }
    return NULL;
}

static unsigned int key_map_access( struct object *obj, unsigned int access )
{
    access = default_map_access( obj, access );
    /* filter the WOW64 masks, as they aren't real access bits */
    return access & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
}

static struct security_descriptor *key_get_sd( struct object *obj )
{
    static struct security_descriptor *key_default_sd;

    if (obj->sd) return obj->sd;

    if (!key_default_sd)
    {
        struct acl *dacl;
        struct ace *ace;
        struct sid *sid;
        size_t users_sid_len = sid_len( &builtin_users_sid );
        size_t admins_sid_len = sid_len( &builtin_admins_sid );
        size_t dacl_len = sizeof(*dacl) + 2 * sizeof(*ace) + users_sid_len + admins_sid_len;

        key_default_sd = mem_alloc( sizeof(*key_default_sd) + 2 * admins_sid_len + dacl_len );
        key_default_sd->control   = SE_DACL_PRESENT;
        key_default_sd->owner_len = admins_sid_len;
        key_default_sd->group_len = admins_sid_len;
        key_default_sd->sacl_len  = 0;
        key_default_sd->dacl_len  = dacl_len;
        sid = (struct sid *)(key_default_sd + 1);
        sid = copy_sid( sid, &builtin_admins_sid );
        copy_sid( sid, &builtin_admins_sid );

        dacl = (struct acl *)((char *)(key_default_sd + 1) + 2 * admins_sid_len);
        dacl->revision = ACL_REVISION;
        dacl->pad1     = 0;
        dacl->size     = dacl_len;
        dacl->count    = 2;
        dacl->pad2     = 0;
        ace = set_ace( ace_first( dacl ), &builtin_users_sid, ACCESS_ALLOWED_ACE_TYPE,
                       INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE, GENERIC_READ );
        set_ace( ace_next( ace ), &builtin_admins_sid, ACCESS_ALLOWED_ACE_TYPE, 0, KEY_ALL_ACCESS );
    }
    return key_default_sd;
}

static WCHAR *key_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len )
{
    struct nt_key *key = (struct nt_key *) obj;

    if (key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        return NULL;
    }
    return default_get_full_name( obj, max, ret_len );
}

static struct object *key_lookup_name( struct object *obj, struct unicode_str *name,
                                       unsigned int attr, struct object *root )
{
    struct nt_key *found, *key = (struct nt_key *)obj;
    struct unicode_str tmp;
    data_size_t next;
    int index;

    assert( obj->ops == &key_ops );

    if (!name) return NULL;  /* open the key itself */

    if (key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        return NULL;
    }

    if (key->flags & KEY_SYMLINK)
    {
        struct unicode_str name_left;
        struct key_value *value;

        if (!name->len && (attr & OBJ_OPENLINK)) return NULL;

        if (!(value = find_value( key, symlink_str, &index )) ||
            value->len < sizeof(WCHAR) || *(WCHAR *)value->data != '\\')
        {
            set_error( STATUS_OBJECT_NAME_NOT_FOUND );
            return NULL;
        }
        tmp.str = value->data;
        tmp.len = (value->len / sizeof(WCHAR)) * sizeof(WCHAR);
        if ((obj = lookup_named_object( NULL, tmp, OBJ_CASE_INSENSITIVE, &name_left )))
        {
            if (!name->len) *name = name_left;  /* symlink destination can be created if missing */
            else if (name_left.len)  /* symlink must have been resolved completely */
            {
                release_object( obj );
                obj = NULL;
                set_error( STATUS_OBJECT_NAME_NOT_FOUND );
            }
        }

        key = (struct nt_key *)obj;
        if (key && (key->flags & KEY_WOWSHARE) && (attr & OBJ_KEY_WOW64) && !name->str)
        {
            key = get_parent( key );
            release_object( obj );
            return grab_object( key );
        }

        return obj;
    }

    if (!name->str) return NULL;

    tmp.str = name->str;
    tmp.len = get_path_element( name->str, name->len );

    if (tmp.len > MAX_NAME_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return 0;
    }

    /* skip trailing backslashes */
    for (next = tmp.len; next < name->len; next += sizeof(WCHAR))
        if (name->str[next / sizeof(WCHAR)] != '\\') break;

    if (!(found = find_subkey( key, tmp, &index )))
    {
        if ((key->flags & KEY_WOWSHARE) && (attr & OBJ_KEY_WOW64))
        {
            /* try in the 64-bit parent */
            key = get_parent( key );
            if (!(found = find_subkey( key, tmp, &index ))) return grab_object( key );
        }
    }

    if (!found)
    {
        if (next < name->len)  /* path still has elements */
            set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        else  /* only trailing backslashes */
            name->len = tmp.len;
        return NULL;
    }

    if (next < name->len)  /* move to the next element */
    {
        name->str += next / sizeof(WCHAR);
        name->len -= next;
        if ((attr & OBJ_KEY_WOW64) && found->wow6432node && !is_wow6432node( name->str, name->len ))
            found = found->wow6432node;
    }
    else
    {
        name->str = NULL;
        name->len = 0;
    }
    return grab_object( found );
}

static int key_link_name( struct object *obj, struct object_name *name, struct object *parent )
{
    struct nt_key *key = (struct nt_key *)obj;
    struct nt_key *parent_key = (struct nt_key *)parent;
    struct unicode_str tmp;
    int i, index;

    if (parent->ops != &key_ops)
    {
        /* only the root key can be created inside a normal directory */
        if (!root_key) return directory_link_name( obj, name, parent );
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return 0;
    }

    if (parent_key->last_subkey + 1 == parent_key->nb_subkeys)
    {
        /* need to grow the array */
        if (!grow_subkeys( parent_key )) return 0;
    }
    tmp.str = name->name;
    tmp.len = name->len;
    find_subkey( parent_key, tmp, &index );

    for (i = ++parent_key->last_subkey; i > index; i--)
        parent_key->subkeys[i] = parent_key->subkeys[i - 1];
    parent_key->subkeys[index] = (struct nt_key *)grab_object( key );
    if (is_wow6432node( name->name, name->len ) &&
        !is_wow6432node( parent_key->obj.name->name, parent_key->obj.name->len ))
        parent_key->wow6432node = key;
    name->parent = parent;
    return 1;
}

static void key_unlink_name( struct object *obj, struct object_name *name )
{
    struct nt_key *key = (struct nt_key *)obj;
    struct nt_key *parent = (struct nt_key *)name->parent;
    int i, nb_subkeys;

    if (!parent) return;

    if (parent->obj.ops != &key_ops)
    {
        unlink_name( name );
        return;
    }

    for (i = 0; i <= parent->last_subkey; i++) if (parent->subkeys[i] == key) break;
    assert( i <= parent->last_subkey );
    for ( ; i < parent->last_subkey; i++) parent->subkeys[i] = parent->subkeys[i + 1];
    parent->last_subkey--;
    name->parent = NULL;
    if (parent->wow6432node == key) parent->wow6432node = NULL;
    release_object( key );

    /* try to shrink the array */
    nb_subkeys = parent->nb_subkeys;
    if (nb_subkeys > MIN_SUBKEYS && parent->last_subkey < nb_subkeys / 2)
    {
        struct nt_key **new_subkeys;
        nb_subkeys -= nb_subkeys / 3;  /* shrink by 33% */
        if (nb_subkeys < MIN_SUBKEYS) nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = nt_realloc( parent->subkeys, nb_subkeys * sizeof(*new_subkeys) ))) return;
        parent->subkeys = new_subkeys;
        parent->nb_subkeys = nb_subkeys;
    }
}

/* close the notification associated with a handle */
/* Arctic: client caches of key values (ntdll registry.c). A key a client
 * may query through its handle gets a shared object in session memory; its
 * sequence number moves on every change of the key's values, so a client
 * knows its copy of them is still the key's. It goes when the last handle
 * to the key does. */
#define MAX_SHARED_KEYS 16384
static unsigned int shared_keys;

static void key_values_changed( struct nt_key *key )
{
    if (!key->shared) return;
    SHARED_WRITE_BEGIN( key->shared, object_shm_t )
    {
        (void)shared;
    }
    SHARED_WRITE_END;
}

static void unshare_key( struct nt_key *key )
{
    if (!key->shared) return;
    free_shared_object( key->shared );
    key->shared = NULL;
    shared_keys--;
}

/* the locator of the key's counter, for a handle just opened */
static struct obj_locator share_key( struct nt_key *key, obj_handle_t handle )
{
    struct obj_locator locator = {0};

    if (!handle || key->flags & (KEY_PREDEF | KEY_DELETED)) return locator;
    if (!(get_handle_access( nt_current->process, handle ) & KEY_QUERY_VALUE)) return locator;
    if (!key->shared)
    {
        if (shared_keys >= MAX_SHARED_KEYS) return locator;
        if (!(key->shared = alloc_shared_object( sizeof(object_shm_t) ))) return locator;
        shared_keys++;
    }
    return get_shared_object_locator( key->shared );
}

static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct nt_key * key = (struct nt_key *) obj;
    struct notify *notify = find_notify( key, process, handle );
    if (notify) do_notification( key, notify, 1 );
    if (atomic_read( &obj->handle_count ) == 1) unshare_key( key );  /* the last handle goes */
    return 1;  /* ok to close */
}

static void key_destroy( struct object *obj )
{
    int i;
    struct list *ptr;
    struct nt_key *key = (struct nt_key *)obj;
    assert( obj->ops == &key_ops );

    unshare_key( key );
    nt_free( key->class );
    for (i = 0; i <= key->last_value; i++)
    {
        nt_free( key->values[i].name );
        nt_free( key->values[i].data );
    }
    nt_free( key->values );
    for (i = 0; i <= key->last_subkey; i++)
    {
        key->subkeys[i]->obj.name->parent = NULL;
        release_object( key->subkeys[i] );
    }
    nt_free( key->subkeys );
    /* unconditionally notify everything waiting on this key */
    while ((ptr = wlist_head( &key->notify_list )))
    {
        struct notify *notify = LIST_ENTRY( ptr, struct notify, entry );
        do_notification( key, notify, 1 );
    }
}

/* mark a key and all its parents as dirty (modified) */
static void make_dirty( struct nt_key *key )
{
    while (key)
    {
        if (key->flags & (KEY_DIRTY|KEY_VOLATILE)) return;  /* nothing to do */
        key->flags |= KEY_DIRTY;
        key = get_parent( key );
    }
}

/* mark a key and all its subkeys as clean (not modified) */
static void make_clean( struct nt_key *key )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    if (!(key->flags & KEY_DIRTY)) return;
    key->flags &= ~KEY_DIRTY;
    for (i = 0; i <= key->last_subkey; i++) make_clean( key->subkeys[i] );
}

/* go through all the notifications and send them if necessary */
static void check_notify( struct nt_key *key, unsigned int change, int not_subtree )
{
    struct list *ptr, *next;

    LIST_FOR_EACH_SAFE( ptr, next, &key->notify_list )
    {
        struct notify *n = LIST_ENTRY( ptr, struct notify, entry );
        if ( ( not_subtree || n->subtree ) && ( change & n->filter ) )
            do_notification( key, n, 0 );
    }
}

/* update key modification time */
static void touch_key( struct nt_key *key, unsigned int change )
{
    key->modif = nt_current_time;
    make_dirty( key );
    if (change & REG_NOTIFY_CHANGE_LAST_SET) key_values_changed( key );

    /* do notifications */
    check_notify( key, change, 1 );
    for (key = get_parent( key ); key; key = get_parent( key )) check_notify( key, change, 0 );
}

/* get the wow6432node key if any, grabbing it and releasing the original key */
static struct nt_key *grab_wow6432node( struct nt_key *key )
{
    struct nt_key *ret = key->wow6432node;

    if (!ret) return key;
    if (ret->flags & KEY_WOWSHARE) return key;
    grab_object( ret );
    release_object( key );
    return ret;
}

/* recursively obtain the wow6432node parent key if any */
static struct nt_key *get_wow6432node( struct nt_key *key )
{
    struct nt_key *parent, *ret;
    struct unicode_str name;
    int index;

    if (!key)
        return NULL;

    if (key->wow6432node)
        return key->wow6432node;

    parent = get_parent( key );
    if (parent && key == parent->wow6432node)
        return key;

    if (!(ret = get_wow6432node( parent )))
        return key;

    name.str = key->obj.name->name;
    name.len = key->obj.name->len;
    return find_subkey( ret, name, &index );
}

/* get the real parent key, taking wow6432node into account */
static struct object *get_wow6432_parent( struct nt_key *parent, unsigned int access, struct unicode_str name )
{
    if (parent && !(access & KEY_WOW64_64KEY) && !is_wow6432node( name.str, name.len ))
    {
        struct nt_key *key = get_wow6432node( parent );
        if (key && ((access & KEY_WOW64_32KEY) || (key->flags & KEY_WOWSHARE))) return &key->obj;
    }
    return &parent->obj;
}

/* recursively create a subkey (for internal use only) */
static struct nt_key *create_key_recursive( struct nt_key *root, struct unicode_str name, timeout_t modif )
{
    struct nt_key *key, *parent = (struct nt_key *)grab_object( root );
    struct key_init_data data = { .modif = modif };
    struct object_params params = { .ops = &key_ops, .name = name, .attr = OBJ_OPENIF, .init_data = &data };
    data_size_t len = name.len;

    while (len)
    {
        params.name.len = get_path_element( params.name.str, len );
        params.root = &parent->obj;
        key = create_named_object( &params );
        release_object( parent );
        if (!key) return NULL;
        parent = key;

        /* skip trailing \\ and move to the next element */
        if (params.name.len < len)
        {
            params.name.len += sizeof(WCHAR);
            params.name.str += params.name.len / sizeof(WCHAR);
            len -= params.name.len;
        }
        else break;
    }
    return parent;
}

/* query information about a key or a subkey */
static void enum_key( struct nt_key *key, int index, int info_class, struct enum_key_reply *reply )
{
    int i;
    data_size_t len, namelen, classlen;
    data_size_t max_subkey = 0, max_class = 0;
    data_size_t max_value = 0, max_data = 0;
    WCHAR *fullname = NULL;
    char *data;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (index != -1)  /* -1 means use the specified key directly */
    {
        if ((index < 0) || (index > key->last_subkey))
        {
            set_error( STATUS_NO_MORE_ENTRIES );
            return;
        }
        key = key->subkeys[index];
    }

    namelen = key->obj.name->len;
    classlen = key->classlen;

    switch(info_class)
    {
    case KeyNameInformation:
        if (!(fullname = key_get_full_name( &key->obj, ~0u, &namelen ))) return;
        fallthrough;
    case KeyBasicInformation:
        classlen = 0; /* only return the name */
        fallthrough;
    case KeyNodeInformation:
        reply->max_subkey = 0;
        reply->max_class  = 0;
        reply->max_value  = 0;
        reply->max_data   = 0;
        break;
    case KeyFullInformation:
    case KeyCachedInformation:
        for (i = 0; i <= key->last_subkey; i++)
        {
            if (key->subkeys[i]->obj.name->len > max_subkey) max_subkey = key->subkeys[i]->obj.name->len;
            if (key->subkeys[i]->classlen > max_class) max_class = key->subkeys[i]->classlen;
        }
        for (i = 0; i <= key->last_value; i++)
        {
            if (key->values[i].namelen > max_value) max_value = key->values[i].namelen;
            if (key->values[i].len > max_data) max_data = key->values[i].len;
        }
        reply->max_subkey = max_subkey;
        reply->max_class  = max_class;
        reply->max_value  = max_value;
        reply->max_data   = max_data;
        reply->namelen    = namelen;
        if (info_class == KeyCachedInformation)
            classlen = 0; /* don't return any data, only its size */
        namelen = 0;  /* don't return name */
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    reply->subkeys = key->last_subkey + 1;
    reply->values  = key->last_value + 1;
    reply->modif   = key->modif;
    reply->total   = namelen + classlen;

    len = min( reply->total, get_reply_max_size() );
    if (len && (data = set_reply_data_size( len )))
    {
        if (len > namelen)
        {
            reply->namelen = namelen;
            data = mem_append( data, key->obj.name->name, namelen );
            memcpy( data, key->class, len - namelen );
        }
        else if (info_class == KeyNameInformation)
        {
            reply->namelen = namelen;
            memcpy( data, fullname, len );
        }
        else
        {
            reply->namelen = len;
            memcpy( data, key->obj.name->name, len );
        }
    }
    nt_free( fullname );
    if (debug_level > 1) dump_operation( key, NULL, "Enum" );
}

/* rename a key and its values */
static void rename_key( struct nt_key *key, struct unicode_str new_name )
{
    struct object_name *new_name_ptr;
    struct nt_key *parent = get_parent( key );
    data_size_t len;
    int i, index, cur_index;

    /* changing to a path is not allowed */
    len = get_path_element( new_name.str, new_name.len );
    if (!len || len != new_name.len || len > MAX_NAME_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    /* check for existing subkey with the same name */
    if (!parent || find_subkey( parent, new_name, &index ))
    {
        set_error( STATUS_CANNOT_DELETE );
        return;
    }

    if (key == parent->wow6432node || is_wow6432node( new_name.str, new_name.len ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!(new_name_ptr = mem_alloc( offsetof( struct object_name, name[new_name.len / sizeof(WCHAR)] ))))
        return;

    new_name_ptr->obj = &key->obj;
    new_name_ptr->len = new_name.len;
    new_name_ptr->parent = &parent->obj;
    memcpy( new_name_ptr->name, new_name.str, new_name.len );

    for (cur_index = 0; cur_index <= parent->last_subkey; cur_index++)
        if (parent->subkeys[cur_index] == key) break;

    if (cur_index < index)
    {
        --index;
        for (i = cur_index; i < index; ++i) parent->subkeys[i] = parent->subkeys[i+1];
    }
    else if (cur_index > index)
    {
        for (i = cur_index; i > index; --i) parent->subkeys[i] = parent->subkeys[i-1];
    }
    parent->subkeys[index] = key;

    nt_free( key->obj.name );
    key->obj.name = new_name_ptr;

    if (debug_level > 1) dump_operation( key, NULL, "Rename" );
    touch_key( key, REG_NOTIFY_CHANGE_NAME );
}

/* delete a key and its values */
static int delete_key( struct nt_key *key, int recurse )
{
    struct nt_key *parent;

    if (key->flags & KEY_DELETED) return 1;

    if (!(parent = get_parent( key )))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return 0;
    }

    if (recurse)
    {
        while (key->last_subkey >= 0)
            if (!delete_key( key->subkeys[key->last_subkey], 1 )) return 0;
    }
    else if (key->last_subkey >= 0)  /* we can only delete a key that has no subkeys */
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    if (debug_level > 1) dump_operation( key, NULL, "Delete" );
    key->flags |= KEY_DELETED;
    key_values_changed( key );  /* a cached copy of its values says nothing now */
    unlink_named_object( &key->obj );
    touch_key( parent, REG_NOTIFY_CHANGE_NAME );
    return 1;
}

/* try to grow the array of values; return 1 if OK, 0 on error */
static int grow_values( struct nt_key *key )
{
    struct key_value *new_val;
    int nb_values;

    if (key->nb_values)
    {
        nb_values = key->nb_values + (key->nb_values / 2);  /* grow by 50% */
        if (!(new_val = nt_realloc( key->values, nb_values * sizeof(*new_val) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_values = MIN_VALUES;
        if (!(new_val = mem_alloc( nb_values * sizeof(*new_val) ))) return 0;
    }
    key->values = new_val;
    key->nb_values = nb_values;
    return 1;
}

/* find the named value of a given key and return its index in the array */
static struct key_value *find_value( const struct nt_key *key, struct unicode_str name, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_value;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->values[i].namelen, name.len );
        res = memicmp_strW( key->values[i].name, name.str, len );
        if (!res) res = key->values[i].namelen - name.len;
        if (!res)
        {
            *index = i;
            return &key->values[i];
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    *index = min;  /* this is where we should insert it */
    return NULL;
}

/* insert a new value; the index must have been returned by find_value */
static struct key_value *insert_value( struct nt_key *key, struct unicode_str name, int index )
{
    struct key_value *value;
    WCHAR *new_name = NULL;
    int i;

    if (name.len > MAX_VALUE_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_NAME_TOO_LONG );
        return NULL;
    }
    if (key->last_value + 1 == key->nb_values)
    {
        if (!grow_values( key )) return NULL;
    }
    if (name.len && !(new_name = memdup( name.str, name.len ))) return NULL;
    for (i = ++key->last_value; i > index; i--) key->values[i] = key->values[i - 1];
    value = &key->values[index];
    value->name    = new_name;
    value->namelen = name.len;
    value->len     = 0;
    value->data    = NULL;
    return value;
}

/* set a key value */
static void set_value( struct nt_key *key, struct unicode_str name,
                       int type, const void *data, data_size_t len )
{
    struct key_value *value;
    void *ptr = NULL;
    int index;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if ((value = find_value( key, name, &index )))
    {
        /* check if the new value is identical to the existing one */
        if (value->type == type && value->len == len &&
            value->data && !memcmp( value->data, data, len ))
        {
            if (debug_level > 1) dump_operation( key, value, "Skip setting" );
            return;
        }
    }

    if (key->flags & KEY_SYMLINK)
    {
        if (type != REG_LINK || name.len != symlink_str.len ||
            memicmp_strW( name.str, symlink_str.str, name.len ))
        {
            set_error( STATUS_ACCESS_DENIED );
            return;
        }
    }

    if (len && !(ptr = memdup( data, len ))) return;

    if (!value)
    {
        if (!(value = insert_value( key, name, index )))
        {
            nt_free( ptr );
            return;
        }
    }
    else nt_free( value->data ); /* already existing, free previous data */

    value->type  = type;
    value->len   = len;
    value->data  = ptr;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );
    if (debug_level > 1) dump_operation( key, value, "Set" );
}

/* get a key value */
static void get_value( struct nt_key *key, struct unicode_str name, int *type, data_size_t *len )
{
    struct key_value *value;
    int index;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if ((value = find_value( key, name, &index )))
    {
        *type = value->type;
        *len  = value->len;
        if (value->data) set_reply_data( value->data, min( value->len, get_reply_max_size() ));
        if (debug_level > 1) dump_operation( key, value, "Get" );
    }
    else
    {
        *type = -1;
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
    }
}

/* enumerate a key value */
static void enum_value( struct nt_key *key, int i, int info_class, struct enum_key_value_reply *reply )
{
    struct key_value *value;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (i < 0 || i > key->last_value) set_error( STATUS_NO_MORE_ENTRIES );
    else
    {
        void *data;
        data_size_t namelen, maxlen;

        value = &key->values[i];
        reply->type = value->type;
        namelen = value->namelen;

        switch(info_class)
        {
        case KeyValueBasicInformation:
            reply->total = namelen;
            break;
        case KeyValueFullInformation:
            reply->total = namelen + value->len;
            break;
        case KeyValuePartialInformation:
            reply->total = value->len;
            namelen = 0;
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }

        maxlen = min( reply->total, get_reply_max_size() );
        if (maxlen && ((data = set_reply_data_size( maxlen ))))
        {
            if (maxlen > namelen)
            {
                reply->namelen = namelen;
                data = mem_append( data, value->name, namelen );
                memcpy( data, value->data, maxlen - namelen );
            }
            else
            {
                reply->namelen = maxlen;
                memcpy( data, value->name, maxlen );
            }
        }
        if (debug_level > 1) dump_operation( key, value, "Enum" );
    }
}

/* delete a value */
static void delete_value( struct nt_key *key, struct unicode_str name )
{
    struct key_value *value;
    int i, index, nb_values;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (!(value = find_value( key, name, &index )))
    {
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return;
    }
    if (debug_level > 1) dump_operation( key, value, "Delete" );
    nt_free( value->name );
    nt_free( value->data );
    for (i = index; i < key->last_value; i++) key->values[i] = key->values[i + 1];
    key->last_value--;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );

    /* try to shrink the array */
    nb_values = key->nb_values;
    if (nb_values > MIN_VALUES && key->last_value < nb_values / 2)
    {
        struct key_value *new_val;
        nb_values -= nb_values / 3;  /* shrink by 33% */
        if (nb_values < MIN_VALUES) nb_values = MIN_VALUES;
        if (!(new_val = nt_realloc( key->values, nb_values * sizeof(*new_val) ))) return;
        key->values = new_val;
        key->nb_values = nb_values;
    }
}

/* get the registry key corresponding to an hkey handle */
static struct nt_key *get_hkey_obj( obj_handle_t hkey, unsigned int access )
{
    struct nt_key *key = (struct nt_key *)get_handle_obj( nt_current->process, hkey, access, &key_ops );

    if (key && key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        release_object( key );
        key = NULL;
    }
    return key;
}

/* read a line from the input file */
static int read_next_line( struct file_load_info *info )
{
    char *newbuf;
    int newlen, pos = 0;

    info->line++;
    for (;;)
    {
        if (!nt_fgets( info->buffer + pos, info->len - pos, info->file ))
            return (pos != 0);  /* EOF */
        pos = strlen(info->buffer);
        if (info->buffer[pos-1] == '\n')
        {
            /* got a full line */
            info->buffer[--pos] = 0;
            if (pos > 0 && info->buffer[pos-1] == '\r') info->buffer[pos-1] = 0;
            return 1;
        }
        if (pos < info->len - 1) return 1;  /* EOF but something was read */

        /* need to enlarge the buffer */
        newlen = info->len + info->len / 2;
        if (!(newbuf = nt_realloc( info->buffer, newlen )))
        {
            set_error( STATUS_NO_MEMORY );
            return -1;
        }
        info->buffer = newbuf;
        info->len = newlen;
    }
}

/* make sure the temp buffer holds enough space */
static int get_file_tmp_space( struct file_load_info *info, size_t size )
{
    WCHAR *tmp;
    if (info->tmplen >= size) return 1;
    if (!(tmp = nt_realloc( info->tmp, size )))
    {
        set_error( STATUS_NO_MEMORY );
        return 0;
    }
    info->tmp = tmp;
    info->tmplen = size;
    return 1;
}

/* report an error while loading an input file */
static void file_read_error( const char *err, struct file_load_info *info )
{
    if (info->filename)
        nt_fprintf( stderr, "%s:%d: %s '%s'\n", info->filename, info->line, err, info->buffer );
    else
        nt_fprintf( stderr, "<fd>:%d: %s '%s'\n", info->line, err, info->buffer );
}

/* convert a data type tag to a value type */
static int get_data_type( const char *buffer, int *type, int *parse_type )
{
    struct data_type { const char *tag; int len; int type; int parse_type; };

    static const struct data_type data_types[] =
    {                   /* actual type */  /* type to assume for parsing */
        { "\"",        1,   REG_SZ,              REG_SZ },
        { "str:\"",    5,   REG_SZ,              REG_SZ },
        { "str(2):\"", 8,   REG_EXPAND_SZ,       REG_SZ },
        { "str(7):\"", 8,   REG_MULTI_SZ,        REG_SZ },
        { "hex:",      4,   REG_BINARY,          REG_BINARY },
        { "dword:",    6,   REG_DWORD,           REG_DWORD },
        { "hex(",      4,   -1,                  REG_BINARY },
        { NULL,        0,    0,                  0 }
    };

    const struct data_type *ptr;
    char *end;

    for (ptr = data_types; ptr->tag; ptr++)
    {
        if (strncmp( ptr->tag, buffer, ptr->len )) continue;
        *parse_type = ptr->parse_type;
        if ((*type = ptr->type) != -1) return ptr->len;
        /* "hex(xx):" is special */
        *type = (int)nt_strtoul( buffer + 4, &end, 16 );
        if ((end <= buffer) || strncmp( end, "):", 2 )) return 0;
        return end + 2 - buffer;
    }
    return 0;
}

/* load and create a key from the input file */
static struct nt_key *load_key( struct nt_key *base, const char *buffer, int prefix_len,
                             struct file_load_info *info, timeout_t *modif )
{
    WCHAR *p;
    struct unicode_str name;
    int res;
    unsigned int mod;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;

    len = info->tmplen;
    if ((res = parse_strW( info->tmp, &len, buffer, ']' )) == -1)
    {
        file_read_error( "Malformed key", info );
        return NULL;
    }
    if (sscanf( buffer + res, " %u", &mod ) == 1)
        *modif = (timeout_t)mod * TICKS_PER_SEC + ticks_1601_to_1970;
    else
        *modif = nt_current_time;

    p = info->tmp;
    while (prefix_len && *p) { if (*p++ == '\\') prefix_len--; }

    if (!*p)
    {
        if (prefix_len > 1)
        {
            file_read_error( "Malformed key", info );
            return NULL;
        }
        /* empty key name, return base key */
        return (struct nt_key *)grab_object( base );
    }
    name.str = p;
    name.len = len - (p - info->tmp + 1) * sizeof(WCHAR);
    return create_key_recursive( base, name, 0 );
}

/* update the modification time of a key (and its parents) after it has been loaded from a file */
static void update_key_time( struct nt_key *key, timeout_t modif )
{
    while (key && !key->modif)
    {
        key->modif = modif;
        key = get_parent( key );
    }
}

/* load a global option from the input file */
static int load_global_option( const char *buffer, struct file_load_info *info )
{
    const char *p;

    if (!strncmp( buffer, "#arch=", 6 ))
    {
        enum prefix_type type;
        p = buffer + 6;
        if (!strcmp( p, "win32" )) type = PREFIX_32BIT;
        else if (!strcmp( p, "win64" )) type = PREFIX_64BIT;
        else
        {
            file_read_error( "Unknown architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
        if (prefix_type == PREFIX_UNKNOWN) prefix_type = type;
        else if (type != prefix_type)
        {
            file_read_error( "Mismatched architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
    }
    /* ignore unknown options */
    return 1;
}

/* load a key option from the input file */
static int load_key_option( struct nt_key *key, const char *buffer, struct file_load_info *info )
{
    const char *p;
    data_size_t len;

    if (!strncmp( buffer, "#time=", 6 ))
    {
        timeout_t modif = 0;
        for (p = buffer + 6; *p; p++)
        {
            if (*p >= '0' && *p <= '9') modif = (modif << 4) | (*p - '0');
            else if (*p >= 'A' && *p <= 'F') modif = (modif << 4) | (*p - 'A' + 10);
            else if (*p >= 'a' && *p <= 'f') modif = (modif << 4) | (*p - 'a' + 10);
            else break;
        }
        update_key_time( key, modif );
    }
    if (!strncmp( buffer, "#class=", 7 ))
    {
        p = buffer + 7;
        if (*p++ != '"') return 0;
        if (!get_file_tmp_space( info, strlen(p) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if (parse_strW( info->tmp, &len, p, '\"' ) == -1) return 0;
        nt_free( key->class );
        if (!(key->class = memdup( info->tmp, len ))) len = 0;
        key->classlen = len;
    }
    if (!strncmp( buffer, "#link", 5 )) key->flags |= KEY_SYMLINK;
    /* ignore unknown options */
    return 1;
}

/* parse a comma-separated list of hex digits */
static int parse_hex( unsigned char *dest, data_size_t *len, const char *buffer )
{
    const char *p = buffer;
    data_size_t count = 0;
    char *end;

    while (isxdigit(*p))
    {
        unsigned int val = nt_strtoul( p, &end, 16 );
        if (end == p || val > 0xff) return -1;
        if (count++ >= *len) return -1;  /* dest buffer overflow */
        *dest++ = val;
        p = end;
        while (isspace(*p)) p++;
        if (*p == ',') p++;
        while (isspace(*p)) p++;
    }
    *len = count;
    return p - buffer;
}

/* parse a value name and create the corresponding value */
static struct key_value *parse_value_name( struct nt_key *key, const char *buffer, data_size_t *len,
                                           struct file_load_info *info )
{
    struct key_value *value;
    struct unicode_str name;
    int index;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;
    name.str = info->tmp;
    name.len = info->tmplen;
    if (buffer[0] == '@')
    {
        name.len = 0;
        *len = 1;
    }
    else
    {
        int r = parse_strW( info->tmp, &name.len, buffer + 1, '\"' );
        if (r == -1) goto error;
        *len = r + 1; /* for initial quote */
        name.len -= sizeof(WCHAR);  /* terminating null */
    }
    while (isspace(buffer[*len])) (*len)++;
    if (buffer[*len] != '=') goto error;
    (*len)++;
    while (isspace(buffer[*len])) (*len)++;
    if (!(value = find_value( key, name, &index ))) value = insert_value( key, name, index );
    return value;

 error:
    file_read_error( "Malformed value name", info );
    return NULL;
}

/* load a value from the input file */
static int load_value( struct nt_key *key, const char *buffer, struct file_load_info *info )
{
    DWORD dw;
    void *ptr, *newptr;
    int res, type, parse_type;
    data_size_t maxlen, len;
    struct key_value *value;

    if (!(value = parse_value_name( key, buffer, &len, info ))) return 0;
    key_values_changed( key );  /* loading a hive over a key a client has open */
    if (!(res = get_data_type( buffer + len, &type, &parse_type ))) goto error;
    buffer += len + res;

    switch(parse_type)
    {
    case REG_SZ:
        if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if (parse_strW( info->tmp, &len, buffer, '\"' ) == -1) goto error;
        ptr = info->tmp;
        break;
    case REG_DWORD:
        dw = nt_strtoul( buffer, NULL, 16 );
        ptr = &dw;
        len = sizeof(dw);
        break;
    case REG_BINARY:  /* hex digits */
        len = 0;
        for (;;)
        {
            maxlen = 1 + strlen(buffer) / 2;  /* at least 2 chars for one hex byte */
            if (!get_file_tmp_space( info, len + maxlen )) return 0;
            if ((res = parse_hex( (unsigned char *)info->tmp + len, &maxlen, buffer )) == -1) goto error;
            len += maxlen;
            buffer += res;
            while (isspace(*buffer)) buffer++;
            if (!*buffer) break;
            if (*buffer != '\\') goto error;
            if (read_next_line( info) != 1) goto error;
            buffer = info->buffer;
            while (isspace(*buffer)) buffer++;
        }
        ptr = info->tmp;
        break;
    default:
        assert(0);
        ptr = NULL;  /* keep compiler quiet */
        break;
    }

    if (!len) newptr = NULL;
    else if (!(newptr = memdup( ptr, len ))) return 0;

    nt_free( value->data );
    value->data = newptr;
    value->len  = len;
    value->type = type;
    return 1;

 error:
    file_read_error( "Malformed value", info );
    nt_free( value->data );
    value->data = NULL;
    value->len  = 0;
    value->type = REG_NONE;
    return 0;
}

/* return the length (in path elements) of name that is part of the key name */
/* for instance if key is USER\foo\bar and name is foo\bar\baz, return 2 */
static int get_prefix_len( struct nt_key *key, const char *name, struct file_load_info *info )
{
    WCHAR *p;
    int res;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(name) * sizeof(WCHAR) )) return 0;

    len = info->tmplen;
    if (parse_strW( info->tmp, &len, name, ']' ) == -1)
    {
        file_read_error( "Malformed key", info );
        return 0;
    }
    for (p = info->tmp; *p; p++) if (*p == '\\') break;
    len = (p - info->tmp) * sizeof(WCHAR);
    for (res = 1; key != root_key; res++)
    {
        if (len == key->obj.name->len && !memicmp_strW( info->tmp, key->obj.name->name, len )) break;
        key = get_parent( key );
    }
    if (key == root_key) res = 0;  /* no matching name */
    return res;
}

/* load all the keys from the input file */
/* prefix_len is the number of key name prefixes to skip, or -1 for autodetection */
static void load_keys( struct nt_key *key, const char *filename, FILE *f, int prefix_len )
{
    struct nt_key *subkey = NULL;
    struct file_load_info info;
    timeout_t modif = nt_current_time;
    char *p;

    info.filename = filename;
    info.file   = f;
    info.len    = 4;
    info.tmplen = 4;
    info.line   = 0;
    if (!(info.buffer = mem_alloc( info.len ))) return;
    if (!(info.tmp = mem_alloc( info.tmplen )))
    {
        nt_free( info.buffer );
        return;
    }

    if ((read_next_line( &info ) != 1) ||
        strcmp( info.buffer, "WINE REGISTRY Version 2" ))
    {
        set_error( STATUS_NOT_REGISTRY_FILE );
        goto done;
    }

    while (read_next_line( &info ) == 1)
    {
        p = info.buffer;
        while (*p && isspace(*p)) p++;
        switch(*p)
        {
        case '[':   /* new key */
            if (subkey)
            {
                update_key_time( subkey, modif );
                release_object( subkey );
            }
            if (prefix_len == -1) prefix_len = get_prefix_len( key, p + 1, &info );
            if (!(subkey = load_key( key, p + 1, prefix_len, &info, &modif )))
                file_read_error( "Error creating key", &info );
            break;
        case '@':   /* default value */
        case '\"':  /* value */
            if (subkey) load_value( subkey, p, &info );
            else file_read_error( "Value without key", &info );
            break;
        case '#':   /* option */
            if (subkey) load_key_option( subkey, p, &info );
            else if (!load_global_option( p, &info )) goto done;
            break;
        case ';':   /* comment */
        case 0:     /* empty line */
            break;
        default:
            file_read_error( "Unrecognized input", &info );
            break;
        }
    }

 done:
    if (subkey)
    {
        update_key_time( subkey, modif );
        release_object( subkey );
    }
    nt_free( info.buffer );
    nt_free( info.tmp );
}

/* load a part of the registry from a file */
static void load_registry( struct nt_key *key, obj_handle_t handle )
{
    struct nt_file *file;
    int fd;

    if (!(file = get_file_obj( nt_current->process, handle, FILE_READ_DATA ))) return;
    fd = nt_dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = nt_fdopen( fd, "r" );
        if (f)
        {
            load_keys( key, NULL, f, -1 );
            nt_fclose( f );
        }
        else file_set_error();
    }
}

/* load one of the initial registry files */
static int load_init_registry_from_file( const char *filename, struct nt_key *key )
{
    FILE *f;

    if ((f = nt_fopen( filename, "r" )))
    {
        load_keys( key, filename, f, 0 );
        nt_fclose( f );
        if (get_error() == STATUS_NOT_REGISTRY_FILE)
        {
            nt_fprintf( stderr, "%s is not a valid registry file\n", filename );
            return 1;
        }
    }

    assert( save_branch_count < MAX_SAVE_BRANCH_INFO );

    save_branch_info[save_branch_count].filename = filename;
    save_branch_info[save_branch_count++].key = (struct nt_key *)grab_object( key );
    make_object_permanent( &key->obj );
    return (f != NULL);
}

static WCHAR *format_user_registry_path( const struct sid *sid, struct unicode_str *path )
{
    char buffer[7 + 11 + 11 + 11 * ARRAY_SIZE(sid->sub_auth)];
    unsigned int i;
    int len;

    len = snprintf( buffer, sizeof(buffer), "User\\S-%u-%u", sid->revision,
                    ((unsigned int)sid->id_auth[2] << 24) |
                    ((unsigned int)sid->id_auth[3] << 16) |
                    ((unsigned int)sid->id_auth[4] << 8) |
                    ((unsigned int)sid->id_auth[5]) );
    for (i = 0; i < sid->sub_count; i++) len += snprintf( buffer + len, sizeof(buffer) - len, "-%u", sid->sub_auth[i] );
    return ascii_to_unicode_str( buffer, path );
}

#ifdef __aarch64__
static bool supports_aarch32(void)
{
#if defined(HAVE_SYS_PERSONALITY_H)
    int old = personality( PER_LINUX32 );
    if (old == -1) return false;
    personality( old );
    return true;
#endif
    return false;
}
#endif

static void init_supported_machines(void)
{
    unsigned int count = 0;
#ifdef __i386__
    if (prefix_type == PREFIX_32BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
#elif defined(__x86_64__)
    if (prefix_type == PREFIX_64BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_AMD64;
    supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
#elif defined(__arm__)
    if (prefix_type == PREFIX_32BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_ARMNT;
#elif defined(__aarch64__)
    if (prefix_type == PREFIX_64BIT)
    {
        supported_machines[count++] = IMAGE_FILE_MACHINE_ARM64;
        supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
        if (supports_aarch32()) supported_machines[count++] = IMAGE_FILE_MACHINE_ARMNT;
        supported_machines[count++] = IMAGE_FILE_MACHINE_AMD64;
    }
#else
#error Unsupported machine
#endif
    supported_machines_count = count;
    native_machine = supported_machines[0];
}

/* registry initialisation */
void init_registry(void)
{
    static const WCHAR REGISTRY[] = {'\\','R','E','G','I','S','T','R','Y'};
    static const WCHAR HKLM[] = { 'M','a','c','h','i','n','e' };
    static const WCHAR HKU_default[] = { 'U','s','e','r','\\','.','D','e','f','a','u','l','t' };
    static const WCHAR classes_i386[] = {'S','o','f','t','w','a','r','e','\\',
                                         'C','l','a','s','s','e','s','\\',
                                         'W','o','w','6','4','3','2','N','o','d','e'};
    static const WCHAR classes_arm[] = {'S','o','f','t','w','a','r','e','\\',
                                        'C','l','a','s','s','e','s','\\',
                                        'W','o','w','A','A','3','2','N','o','d','e'};
    static const WCHAR perflib[] = {'S','o','f','t','w','a','r','e','\\',
                                    'M','i','c','r','o','s','o','f','t','\\',
                                    'W','i','n','d','o','w','s',' ','N','T','\\',
                                    'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
                                    'P','e','r','f','l','i','b','\\',
                                    '0','0','9'};
    static const WCHAR controlset[] = {'S','y','s','t','e','m','\\',
                                       'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t'};
    static const WCHAR controlset001_path[] = {'\\','R','e','g','i','s','t','r','y','\\',
                                               'M','a','c','h','i','n','e','\\',
                                               'S','y','s','t','e','m','\\',
                                               'C','o','n','t','r','o','l','S','e','t','0','0','1'};
    static const struct unicode_str root_name = { REGISTRY, sizeof(REGISTRY) };
    static const struct unicode_str HKLM_name = { HKLM, sizeof(HKLM) };
    static const struct unicode_str HKU_name = { HKU_default, sizeof(HKU_default) };
    static const struct unicode_str perflib_name = { perflib, sizeof(perflib) };
    static const struct unicode_str controlset_name = { controlset, sizeof(controlset) };

    WCHAR *current_user_path;
    struct unicode_str current_user_str;
    struct nt_key *key, *hklm, *hkcu;
    unsigned int i;
    char *p;
    struct key_init_data data = { .modif = nt_current_time };
    struct object_params params = { .ops = &key_ops, .name = root_name,
                                    .attr = OBJ_PERMANENT, .init_data = &data };

    /* Arctic: how often the registry is saved, NT_REGISTRY_SAVE_PERIOD
     * seconds; on a slow stick, seldom (what a pulled stick loses) */
    if ((p = nt_getenv( "NT_REGISTRY_SAVE_PERIOD" )) && nt_atoi( p ) > 0)
        save_period = (timeout_t)nt_atoi( p ) * -TICKS_PER_SEC;

    /* switch to the config dir */

    /* ntos: fatal_error returns; the executive then does not start */
    if (nt_fchdir( config_dir_fd ) == -1)
    {
        fatal_error( "chdir to config dir: %s\n", nt_strerror( errno ));
        return;
    }

    /* create the root key */
    root_key = create_named_object( &params );
    assert( root_key );
    release_object( root_key );

    /* load system.reg into Registry\Machine */

    if (!(hklm = create_key_recursive( root_key, HKLM_name, nt_current_time )))
    {
        fatal_error( "could not create Machine registry key\n" );
        return;
    }

    if (!load_init_registry_from_file( "system.reg", hklm ))
    {
        if ((p = nt_getenv( "NTARCH" )) && !strcmp( p, "win32" ))
            prefix_type = PREFIX_32BIT;
        else
            prefix_type = sizeof(void *) > sizeof(int) ? PREFIX_64BIT : PREFIX_32BIT;

        if ((key = create_key_recursive( hklm, controlset_name, nt_current_time )))
        {
            key->flags |= KEY_SYMLINK;
            set_value( key, symlink_str, REG_LINK, controlset001_path, sizeof(controlset001_path) );
            release_object( key );
        }
    }
    else if (prefix_type == PREFIX_UNKNOWN)
        prefix_type = PREFIX_32BIT;

    init_supported_machines();

    /* load userdef.reg into Registry\User\.Default */

    if (!(key = create_key_recursive( root_key, HKU_name, nt_current_time )))
    {
        fatal_error( "could not create User\\.Default registry key\n" );
        return;
    }

    load_init_registry_from_file( "userdef.reg", key );
    release_object( key );

    /* load user.reg into HKEY_CURRENT_USER */

    /* FIXME: match default user in token.c. should get from process token instead */
    current_user_path = format_user_registry_path( &local_user_sid, &current_user_str );
    if (!current_user_path ||
        !(hkcu = create_key_recursive( root_key, current_user_str, nt_current_time )))
    {
        fatal_error( "could not create HKEY_CURRENT_USER registry key\n" );
        nt_free( current_user_path );
        return;
    }
    nt_free( current_user_path );
    load_init_registry_from_file( "user.reg", hkcu );

    /* set the shared flag on Software\Classes\Wow6432Node for all platforms */
    for (i = 1; i < supported_machines_count; i++)
    {
        struct unicode_str name;

        switch (supported_machines[i])
        {
        case IMAGE_FILE_MACHINE_I386:  name.str = classes_i386;  name.len = sizeof(classes_i386);  break;
        case IMAGE_FILE_MACHINE_ARMNT: name.str = classes_arm;   name.len = sizeof(classes_arm);   break;
        default: continue;
        }
        if ((key = create_key_recursive( hklm, name, nt_current_time )))
        {
            key->flags |= KEY_WOWSHARE;
            release_object( key );
        }
        /* FIXME: handle HKCU too */
    }

    if ((key = create_key_recursive( hklm, perflib_name, nt_current_time )))
    {
        key->flags |= KEY_PREDEF;
        release_object( key );
    }

    release_object( hklm );
    release_object( hkcu );

    /* start the periodic save timer */
    set_periodic_save_timer();

    /* create windows directories */

    if (!nt_mkdir( "drive_c/windows", 0777 ))
    {
        nt_mkdir( "drive_c/windows/system32", 0777 );
        for (i = 1; i < supported_machines_count; i++)
        {
            switch (supported_machines[i])
            {
            case IMAGE_FILE_MACHINE_I386:  nt_mkdir( "drive_c/windows/syswow64", 0777 ); break;
            case IMAGE_FILE_MACHINE_ARMNT: nt_mkdir( "drive_c/windows/sysarm32", 0777 ); break;
            }
        }
    }

    /* go back to the server dir */
    if (nt_fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", nt_strerror( errno ));
}

/* save a registry branch to a file */
static void save_all_subkeys( struct nt_key *key, FILE *f )
{
    nt_fprintf( f, "WINE REGISTRY Version 2\n" );
    nt_fprintf( f, ";; All keys relative to " );
    dump_path( key, NULL, f );
    nt_fprintf( f, "\n" );
    switch (prefix_type)
    {
    case PREFIX_32BIT:
        nt_fprintf( f, "\n#arch=win32\n" );
        break;
    case PREFIX_64BIT:
        nt_fprintf( f, "\n#arch=win64\n" );
        break;
    default:
        break;
    }
    save_subkeys( key, key, f );
}

/* save a registry branch to a file handle */
static void save_registry( struct nt_key *key, obj_handle_t handle )
{
    struct nt_file *file;
    int fd;

    if (!(file = get_file_obj( nt_current->process, handle, FILE_WRITE_DATA ))) return;
    fd = nt_dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = nt_fdopen( fd, "w" );
        if (f)
        {
            save_all_subkeys( key, f );
            if (nt_fclose( f )) file_set_error();
        }
        else
        {
            file_set_error();
            nt_close( fd );
        }
    }
}

/* save a registry branch to a file */
static int save_branch( struct nt_key *key, const char *filename )
{
    struct nt_stat st;
    /* ntos: 8 KB, not on the 16 KB kernel stack; saves are made one at a
     * time, under the executive's lock */
    static char tmp[PATH_MAX + 32], target[PATH_MAX];
    int fd, count = 0, ret = 0, dir_len = 0;
    FILE *f;

    if (!(key->flags & KEY_DIRTY))
    {
        if (debug_level > 1) dump_operation( key, NULL, "Not saving clean" );
        return 1;
    }
    tmp[0] = 0;

    /* test the file type */

    if ((fd = nt_open( filename, O_WRONLY )) != -1)
    {
        /* Arctic: the hives live on C:, in C:\Windows\System32\config, and
         * the prefix links to them. A hive there is replaced whole, through a
         * temp file next to it, so a stick pulled out mid-save leaves the old
         * one rather than half of the new one. */
        if (!nt_lstat( filename, &st ) && S_ISLNK(st.st_mode) && nt_realpath( filename, target ))
        {
            nt_close( fd );
            filename = target;
            if (strrchr( filename, '/' )) dir_len = strrchr( filename, '/' ) - filename + 1;
        }
        /* if file is not a regular file or has multiple links or is accessed
         * via symbolic links, write directly into it; otherwise use a temp file */
        else if (!nt_lstat( filename, &st ) && (!S_ISREG(st.st_mode) || st.st_nlink > 1))
        {
            nt_ftruncate( fd, 0 );
            goto save;
        }
        else nt_close( fd );
    }

    /* create a temp file */

    for (;;)
    {
        snprintf( tmp, sizeof(tmp), "%.*sreg%lx%04x.tmp", dir_len, filename, (long) nt_getpid(), count++ );
        if ((fd = nt_open( tmp, O_CREAT | O_EXCL | O_WRONLY, 0666 )) != -1) break;
        if (errno != EEXIST) goto done;
        nt_close( fd );
    }

    /* now save to it */

 save:
    if (!(f = nt_fdopen( fd, "w" )))
    {
        if (tmp[0]) nt_unlink( tmp );
        nt_close( fd );
        goto done;
    }

    if (debug_level > 1)
    {
        nt_fprintf( stderr, "%s: ", filename );
        dump_operation( key, NULL, "saving" );
    }

    save_all_subkeys( key, f );
    /* on the disk before the rename makes it the hive */
    ret = !nt_fflush( f ) && (!tmp[0] || !nt_fsync( nt_fileno( f ) ));
    ret = !nt_fclose(f) && ret;

    if (tmp[0])
    {
        /* if successfully written, rename to final name */
        if (ret) ret = !nt_rename( tmp, filename );
        if (!ret) nt_unlink( tmp );
    }

done:
    if (ret) make_clean( key );
    return ret;
}

/* ntos: is a save still being written? (in wineserver, a child) */
static int save_child_running(void)
{
    return nt_saver_busy();
}

/*
 * ntos: a snapshot of a branch, the raw names and data of its keys, taken
 * under the executive's lock with nothing but copies; the saver turns it
 * into the hive's text outside the lock (format_branch). Formatted under the
 * lock, the text of the hives held every NT call for 80 ms (docs/ntos.md
 * «Замки»); a copy is what Windows' lazy flush holds its lock for too.
 *
 * The snapshot: the header's text (u32 length, text), then a record per key
 * save_subkeys would write: u32 count of its path's names, each (u32 length,
 * WCHARs) from the key up to the base; s64 modif; u32 class length; u32
 * flags (1 class, 2 link); u32 values; the class; each value (u32 name
 * length, u32 type, u32 length, name, data). Every field starts 4-aligned.
 */
#define SNAP_ALIGN(len) (((size_t)(len) + 3) & ~(size_t)3)

/* one pass sizes the snapshot, the second fills it: a field at a time
 * through stdio and a buffer doubled as it grew took 30-40 ms for 18000 keys */
static inline void snap_put( char **p, const void *data, size_t len )
{
    if (len) memcpy( *p, data, len );
    if (len & 3) memset( *p + len, 0, 4 - (len & 3) );
    *p += SNAP_ALIGN( len );
}

static inline void snap_u32( char **p, unsigned int v )
{
    memcpy( *p, &v, sizeof(v) );
    *p += sizeof(v);
}

static inline int snap_saved( const struct nt_key *key )
{
    return (key->last_value >= 0) || (key->last_subkey == -1) || key->class || (key->flags & KEY_SYMLINK);
}

/* how many names dump_path would print for the key, the key's up */
static inline unsigned int snap_depth( const struct nt_key *key, const struct nt_key *base )
{
    const struct nt_key *k;
    unsigned int n;

    if (key == base) return 0;
    for (n = 1, k = key; get_parent( k ) && get_parent( k ) != base; k = get_parent( k )) n++;
    return n;
}

static size_t snap_size( const struct nt_key *key, const struct nt_key *base )
{
    size_t size = 0;
    unsigned int n;
    const struct nt_key *k;
    int i;

    if (key->flags & KEY_VOLATILE) return 0;
    if (snap_saved( key ))
    {
        n = snap_depth( key, base );
        size += 4;
        for (k = key; n--; k = get_parent( k )) size += 4 + SNAP_ALIGN( k->obj.name->len );
        size += sizeof(key->modif) + 3 * 4;
        if (key->class) size += SNAP_ALIGN( key->classlen );
        for (i = 0; i <= key->last_value; i++)
            size += 3 * 4 + SNAP_ALIGN( key->values[i].namelen ) + SNAP_ALIGN( key->values[i].len );
    }
    for (i = 0; i <= key->last_subkey; i++) size += snap_size( key->subkeys[i], base );
    return size;
}

static void snap_fill( const struct nt_key *key, const struct nt_key *base, char **p )
{
    const struct nt_key *k;
    unsigned int n;
    int i;

    if (key->flags & KEY_VOLATILE) return;
    if (snap_saved( key ))
    {
        n = snap_depth( key, base );
        snap_u32( p, n );
        for (k = key; n--; k = get_parent( k ))
        {
            snap_u32( p, k->obj.name->len );
            snap_put( p, k->obj.name->name, k->obj.name->len );
        }
        memcpy( *p, &key->modif, sizeof(key->modif) );
        *p += sizeof(key->modif);
        snap_u32( p, key->class ? key->classlen : 0 );
        snap_u32( p, (key->class ? 1 : 0) | ((key->flags & KEY_SYMLINK) ? 2 : 0) );
        snap_u32( p, key->last_value + 1 );
        if (key->class) snap_put( p, key->class, key->classlen );
        for (i = 0; i <= key->last_value; i++)
        {
            const struct key_value *value = &key->values[i];

            snap_u32( p, value->namelen );
            snap_u32( p, value->type );
            snap_u32( p, value->len );
            snap_put( p, value->name, value->namelen );
            snap_put( p, value->data, value->len );
        }
    }
    for (i = 0; i <= key->last_subkey; i++) snap_fill( key->subkeys[i], base, p );
}

/* the snapshot's next field, or NULL past its end */
static const void *snap_get( const char **p, const char *end, size_t len )
{
    const char *data = *p;
    size_t step = (len + 3) & ~(size_t)3;

    if (step < len || (size_t)(end - data) < step) return NULL;
    *p = data + step;
    return data;
}

static int snap_get_u32( const char **p, const char *end, unsigned int *v )
{
    const void *data = snap_get( p, end, sizeof(*v) );

    if (!data) return 0;
    memcpy( v, data, sizeof(*v) );
    return 1;
}

/* in the saver, without the lock: the snapshot as save_all_subkeys' text */
static int format_branch( const void *raw, size_t raw_size, void **text, size_t *text_size )
{
    const char *p = raw, *end = p + raw_size;
    struct { const WCHAR *name; unsigned int len; } small[32], *names = small;
    unsigned int n, len, i, classlen, flags, count;
    const void *data;
    timeout_t modif;
    FILE *f;

    if (!(f = nt_open_memstream())) return 0;
    if (!snap_get_u32( &p, end, &len ) || !(data = snap_get( &p, end, len ))) goto bad;
    nt_fwrite( data, len, 1, f );
    while (p < end)
    {
        if (!snap_get_u32( &p, end, &n )) goto bad;
        if (n > ARRAY_SIZE(small) && !(names = kmalloc_array( n, sizeof(*names), GFP_KERNEL ))) goto bad;
        for (i = 0; i < n; i++)
            if (!snap_get_u32( &p, end, &names[i].len ) || !(names[i].name = snap_get( &p, end, names[i].len )))
                goto bad;
        nt_fprintf( f, "\n[" );
        for (i = n; i--; )   /* from the base down, as dump_path */
        {
            dump_strW( names[i].name, names[i].len, f, "[]" );
            if (i) nt_fprintf( f, "\\\\" );
        }
        if (names != small) kfree( names );
        names = small;

        if (!(data = snap_get( &p, end, sizeof(modif) ))) goto bad;
        memcpy( &modif, data, sizeof(modif) );
        if (!snap_get_u32( &p, end, &classlen ) || !snap_get_u32( &p, end, &flags ) ||
            !snap_get_u32( &p, end, &count )) goto bad;
        nt_fprintf( f, "] %u\n", (unsigned int)((modif - ticks_1601_to_1970) / TICKS_PER_SEC) );
        nt_fprintf( f, "#time=%x%08x\n", (unsigned int)(modif >> 32), (unsigned int)modif );
        if (flags & 1)
        {
            if (!(data = snap_get( &p, end, classlen ))) goto bad;
            nt_fprintf( f, "#class=\"" );
            dump_strW( data, classlen, f, "\"\"" );
            nt_fprintf( f, "\"\n" );
        }
        if (flags & 2) nt_fputs( "#link\n", f );
        for (i = 0; i < count; i++)
        {
            struct key_value value;
            unsigned int namelen;

            if (!snap_get_u32( &p, end, &namelen ) || !snap_get_u32( &p, end, &value.type ) ||
                !snap_get_u32( &p, end, &value.len )) goto bad;
            value.namelen = namelen;
            if (!(value.name = (WCHAR *)snap_get( &p, end, namelen ))) goto bad;
            if (!(value.data = (void *)snap_get( &p, end, value.len ))) goto bad;
            dump_value( &value, f );
        }
    }
    return nt_memstream_take( f, text, text_size );

bad:
    if (names != small) kfree( names );
    if (nt_memstream_take( f, text, text_size )) kvfree( *text );
    pr_warn( "ntos: a registry snapshot could not be read\n" );
    return 0;
}

/* ntos: a branch as it is now, put in memory as a snapshot, and given to
 * the kernel's writer: it formats it, writes a temp file beside the hive,
 * flushes it and renames it over the hive, while the executive goes on
 * (wineserver forked a child that wrote from its copy) */
/* the snapshot of a branch: the header as save_all_subkeys writes it, then the keys */
static char *snap_branch( const struct nt_key *key, size_t *size )
{
    ntos_FILE *head;
    void *header;
    char *data, *p;
    size_t header_size;

    if (!(head = nt_open_memstream())) return NULL;
    nt_fprintf( head, "WINE REGISTRY Version 2\n" );
    nt_fprintf( head, ";; All keys relative to " );
    dump_path( key, NULL, head );
    nt_fprintf( head, "\n" );
    if (prefix_type == PREFIX_32BIT) nt_fprintf( head, "\n#arch=win32\n" );
    else if (prefix_type == PREFIX_64BIT) nt_fprintf( head, "\n#arch=win64\n" );
    if (!nt_memstream_take( head, &header, &header_size )) return NULL;
    *size = 4 + SNAP_ALIGN( header_size ) + snap_size( key, key );
    if (!(data = kvmalloc( *size, GFP_KERNEL )))
    {
        kvfree( header );
        return NULL;
    }
    p = data;
    snap_u32( &p, header_size );
    snap_put( &p, header, header_size );
    kvfree( header );
    snap_fill( key, key, &p );
    if (WARN_ON_ONCE( p != data + *size ))
    {
        kvfree( data );
        return NULL;
    }
    return data;
}

/* ntos.verify_registry=1 (VM tests): at the last save, the branch's text
 * the old way and through a snapshot must be the same, byte for byte */
static void verify_snapshot( const struct nt_key *key, const char *filename )
{
    void *a = NULL, *b = NULL;
    size_t na = 0, nb = 0, nraw, i;
    char *raw;
    FILE *f;

    if (!(f = nt_open_memstream())) return;
    save_all_subkeys( (struct nt_key *)key, f );
    if (!nt_memstream_take( f, &a, &na )) return;
    if ((raw = snap_branch( key, &nraw )) && format_branch( raw, nraw, &b, &nb ) &&
        na == nb && !memcmp( a, b, na ))
        pr_notice( "ntos: %s: the snapshot's text is the same (%zu bytes)\n", filename, na );
    else if (b)
    {
        for (i = 0; i < min( na, nb ) && ((char *)a)[i] == ((char *)b)[i]; i++) ;
        pr_warn( "ntos: %s: the snapshot's text differs at %zu (%zu, %zu bytes): \"%.40s\" / \"%.40s\"\n",
                 filename, i, na, nb, i < na ? (char *)a + i : "", i < nb ? (char *)b + i : "" );
    }
    else pr_warn( "ntos: %s: no snapshot text\n", filename );
    kvfree( raw );
    kvfree( a );
    kvfree( b );
}

static void save_branch_later( struct nt_key *key, const char *filename )
{
    char *data;
    size_t size;

    if (!(key->flags & KEY_DIRTY)) return;
    if (!(data = snap_branch( key, &size ))) return;
    if (!nt_saver_queue( config_dir_fd, filename, data, size, format_branch )) return;
    make_clean( key );
}

/* periodic saving of the registry */
static void periodic_save( void *arg )
{
    int i;

    save_timeout_user = NULL;
    /* Arctic: the periodic save is written beside the executive, from the
     * registry as it is at this moment: a save that waits for a slow stick
     * (3 MB through fsync, seconds) must not stop every NT call meanwhile.
     * What changes from now on is dirty again and goes in the next save. */
    if (save_child_running())
    {
        set_periodic_save_timer();
        return;
    }
    {
        /* ntos: how long the copy in memory holds the executive's lock (it
         * is the one part of a save that does, docs/ntos.md «Замки») */
        u64 t0 = ktime_get_ns();

        for (i = 0; i < save_branch_count; i++)
            save_branch_later( save_branch_info[i].key, save_branch_info[i].filename );
        t0 = ktime_get_ns() - t0;
        if (t0 > 5 * NSEC_PER_MSEC)
            pr_notice( "ntos: the registry copied for its save in %llu ms\n", t0 / NSEC_PER_MSEC );
    }
    set_periodic_save_timer();
}

static void set_periodic_save_timer(void)
{
    if (save_timeout_user) remove_timeout_user( save_timeout_user );
    save_timeout_user = add_timeout_user( save_period, periodic_save, NULL );
}

/* Arctic: RegFlushKey wants the hive on the disk now; Windows writes it
 * before returning. Here it brings the periodic save forward: a child writes
 * the changed branches within a second, the flushes of that second together,
 * and wineserver waits for no disk (see periodic_save). A drive letter that
 * mountmgr gave and flushed was lost when the power went before the next
 * periodic save, up to half a minute (four on a stick) later. */
static struct timeout_user *flush_timeout_user;

static void flush_soon( void *arg )
{
    flush_timeout_user = NULL;
    /* a save running now copied the registry before the change: after it */
    if (save_child_running())
    {
        flush_timeout_user = add_timeout_user( -TICKS_PER_SEC / 4, flush_soon, NULL );
        return;
    }
    if (save_timeout_user) remove_timeout_user( save_timeout_user );
    save_timeout_user = NULL;
    periodic_save( NULL );
}

/* save the modified registry branches to disk */
void flush_registry(void)
{
    int i;

    /* Arctic: a save still being written finishes first, or it could put
     * its older copy over this one */
    nt_saver_wait();
    if (nt_fchdir( config_dir_fd ) == -1) return;
    if (ntos_verify_registry)
        for (i = 0; i < save_branch_count; i++)
            verify_snapshot( save_branch_info[i].key, save_branch_info[i].filename );
    for (i = 0; i < save_branch_count; i++)
    {
        if (!save_branch( save_branch_info[i].key, save_branch_info[i].filename ))
        {
            nt_fprintf( stderr, "wineserver: could not save registry branch to %s",
                     save_branch_info[i].filename );
            nt_perror( " " );
        }
    }
    if (nt_fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", nt_strerror( errno ));
}

/* create a registry key */
DECL_HANDLER(create_key)
{
    struct nt_key *key, *parent = NULL;
    unsigned int access = req->access;
    struct key_init_data data = { .options = req->options, .modif = nt_current_time };
    struct object_params params = { .ops = &key_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );
    data.class = get_req_data_after_objattr( &params, &data.classlen );

    if (!is_wow64_process( nt_current->process )) access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
    if (!(access & KEY_WOW64_64KEY)) params.attr |= OBJ_KEY_WOW64;
    if (req->options & REG_OPTION_CREATE_LINK) params.attr = (params.attr & ~OBJ_OPENIF) | OBJ_OPENLINK;

    if (params.objattr->rootdir)
    {
        if (!(parent = get_hkey_obj( params.objattr->rootdir, 0 ))) return;
        params.root = get_wow6432_parent( parent, access, params.name );
    }

    if ((key = params.name.len ? create_named_object( &params ) : open_named_object( &params )))
    {
        if (!params.name.len || get_error() == STATUS_OBJECT_NAME_EXISTS)
        {
            if (key->flags & KEY_PREDEF) set_error( STATUS_PREDEFINED_HANDLE );
            if (!(access & KEY_WOW64_64KEY)) key = grab_wow6432node( key );
            reply->hkey = alloc_handle( nt_current->process, key, access, params.attr );
        }
        else
        {
            reply->hkey = alloc_handle_no_access_check( nt_current->process, key,
                                                        access, params.attr );
            if (parent) touch_key( get_parent( key ), REG_NOTIFY_CHANGE_NAME );
            if (debug_level > 1) dump_operation( key, NULL, "Create" );
        }
        reply->shm = share_key( key, reply->hkey );
        release_object( key );
    }

    if (parent) release_object( parent );
}

/* open a registry key */
DECL_HANDLER(open_key)
{
    struct nt_key *key, *parent = NULL;
    unsigned int access = req->access;
    struct object_params params = { .ops = &key_ops, .name = get_req_unicode_str(),
                                    .attr = req->attributes };

    if (!is_wow64_process( nt_current->process )) access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
    if (!(access & KEY_WOW64_64KEY)) params.attr |= OBJ_KEY_WOW64;

    if (req->parent)
    {
        if (!(parent = get_hkey_obj( req->parent, 0 ))) return;
        params.root = get_wow6432_parent( parent, access, params.name );
    }

    if ((key = open_named_object( &params )))
    {
        if (!(access & KEY_WOW64_64KEY)) key = grab_wow6432node( key );
        if (key->flags & KEY_PREDEF) set_error( STATUS_PREDEFINED_HANDLE );
        reply->hkey = alloc_handle( nt_current->process, key, access, req->attributes );
        reply->shm = share_key( key, reply->hkey );
        if (debug_level > 1) dump_operation( key, NULL, "Open" );
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* delete a registry key */
DECL_HANDLER(delete_key)
{
    struct nt_key *key = (struct nt_key *)get_handle_obj( nt_current->process, req->hkey, DELETE, &key_ops );

    if (key)
    {
        delete_key( key, 0 );
        release_object( key );
    }
}

/* flush a registry key */
DECL_HANDLER(flush_key)
{
    struct nt_key *key = get_hkey_obj( req->hkey, 0 );
    if (key)
    {
        if (!flush_timeout_user) flush_timeout_user = add_timeout_user( -TICKS_PER_SEC, flush_soon, NULL );
        release_object( key );
    }
}

/* enumerate registry subkeys */
DECL_HANDLER(enum_key)
{
    struct nt_key *key;

    if ((key = get_hkey_obj( req->hkey, req->index == -1 ? 0 : KEY_ENUMERATE_SUB_KEYS )))
    {
        enum_key( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* set a value of a registry key */
DECL_HANDLER(set_key_value)
{
    struct nt_key *key;
    struct unicode_str name;

    if (req->namelen > get_req_data_size())
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    name.str = get_req_data();
    name.len = (req->namelen / sizeof(WCHAR)) * sizeof(WCHAR);

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        data_size_t datalen = get_req_data_size() - req->namelen;
        const char *data = (const char *)get_req_data() + req->namelen;

        set_value( key, name, req->type, data, datalen );
        release_object( key );
    }
}

/* retrieve the value of a registry key */
DECL_HANDLER(get_key_value)
{
    struct nt_key *key;
    struct unicode_str name = get_req_unicode_str();

    reply->total = 0;
    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        get_value( key, name, &reply->type, &reply->total );
        release_object( key );
    }
}

/* Arctic: every value of a registry key, for the client's cache */
DECL_HANDLER(get_key_values)
{
    struct key_value_entry *entry;
    struct nt_key *key;
    data_size_t total = 0;
    char *ptr;
    int i;

    if (!(key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE ))) return;
    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        release_object( key );
        return;
    }
    for (i = 0; i <= key->last_value; i++)
        total += (sizeof(*entry) + key->values[i].namelen + key->values[i].len + 7) & ~7;
    reply->count = key->last_value + 1;
    reply->total = total;
    reply->seq   = key->shared ? CONTAINING_RECORD( key->shared, shared_object_t, shm )->seq : 0;
    if (total > get_reply_max_size()) set_error( STATUS_BUFFER_OVERFLOW );
    else if (total && (ptr = set_reply_data_size( total )))
    {
        for (i = 0; i <= key->last_value; i++)
        {
            const struct key_value *value = &key->values[i];
            data_size_t size = (sizeof(*entry) + value->namelen + value->len + 7) & ~7;

            memset( ptr, 0, size );
            entry = (struct key_value_entry *)ptr;
            entry->type    = value->type;
            entry->namelen = value->namelen;
            entry->len     = value->len;
            memcpy( entry + 1, value->name, value->namelen );
            if (value->len) memcpy( (char *)(entry + 1) + value->namelen, value->data, value->len );
            ptr += size;
        }
    }
    release_object( key );
}

/* enumerate the value of a registry key */
DECL_HANDLER(enum_key_value)
{
    struct nt_key *key;

    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        enum_value( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* delete a value of a registry key */
DECL_HANDLER(delete_key_value)
{
    struct nt_key *key;
    struct unicode_str name = get_req_unicode_str();

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        delete_value( key, name );
        release_object( key );
    }
}

/* load a registry branch from a file */
DECL_HANDLER(load_registry)
{
    struct nt_key *key, *parent = NULL;
    struct key_init_data data = { .modif = nt_current_time };
    struct object_params params = { .ops = &key_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );

    if (!thread_single_check_privilege( nt_current, SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }
    if (params.objattr->rootdir)
    {
        if (!(parent = get_hkey_obj( params.objattr->rootdir, 0 ))) return;
        params.root = &parent->obj;
    }

    if ((key = create_named_object( &params )))
    {
        load_registry( key, req->file );
        release_object( key );
    }
    if (parent) release_object( parent );
}

DECL_HANDLER(unload_registry)
{
    struct nt_key *key, *parent = NULL;
    struct object_params params = { .ops = &key_ops, .name = get_req_unicode_str(),
                                    .attr = req->attributes };

    if (!thread_single_check_privilege( nt_current, SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if (req->parent)
    {
        if (!(parent = get_hkey_obj( req->parent, 0 ))) return;
        params.root = &parent->obj;
    }

    if ((key = open_named_object( &params )))
    {
        if (atomic_read( &key->obj.handle_count ))
            set_error( STATUS_CANNOT_DELETE );
        else if (key->obj.is_permanent)
            set_error( STATUS_ACCESS_DENIED );
        else
            delete_key( key, 1 );     /* FIXME */
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* save a registry branch to a file */
DECL_HANDLER(save_registry)
{
    struct nt_key *key;

    if (!thread_single_check_privilege( nt_current, SeBackupPrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if ((key = get_hkey_obj( req->hkey, 0 )))
    {
        save_registry( key, req->file );
        release_object( key );
    }
}

/* add a registry key change notification */
DECL_HANDLER(set_registry_notification)
{
    struct nt_key *key;
    struct event *event;
    struct notify *notify;

    key = get_hkey_obj( req->hkey, KEY_NOTIFY );
    if (key)
    {
        event = get_event_obj( nt_current->process, req->event, SYNCHRONIZE );
        if (event)
        {
            notify = find_notify( key, nt_current->process, req->hkey );
            if (!notify)
            {
                notify = mem_alloc( sizeof(*notify) );
                if (notify)
                {
                    notify->events  = NULL;
                    notify->event_count = 0;
                    notify->subtree = req->subtree;
                    notify->filter  = req->filter;
                    notify->hkey    = req->hkey;
                    notify->process = nt_current->process;
                    wlist_add_head( &key->notify_list, &notify->entry );
                }
            }
            if (notify)
            {
                struct event **new_array;

                if ((new_array = nt_realloc( notify->events, (notify->event_count + 1) * sizeof(*notify->events) )))
                {
                    notify->events = new_array;
                    notify->events[notify->event_count++] = (struct event *)grab_object( event );
                    reset_event( event );
                    set_error( STATUS_PENDING );
                }
                else set_error( STATUS_NO_MEMORY );
            }
            release_object( event );
        }
        release_object( key );
    }
}

/* rename a registry key */
DECL_HANDLER(rename_key)
{
    struct unicode_str name = get_req_unicode_str();
    struct nt_key *key;

    key = get_hkey_obj( req->hkey, KEY_WRITE );
    if (key)
    {
        rename_key( key, name );
        release_object( key );
    }
}
