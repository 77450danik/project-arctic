// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * win32k: atom tables (from Wine's server/atom.c)
 *
 * Copyright (C) 1993 Alexandre Julliard
 *               1998 Ulrich Weigand
 *
 * As in Windows, both tables belong to win32k: the global one (NtAddAtom,
 * GlobalAddAtom), which Windows keeps for each window station, and the one of
 * USER (classes, clipboard formats, window properties).
 */

#include "user.h"

#define HASH_SIZE     37
#define MIN_HASH_SIZE 4
#define MAX_HASH_SIZE 0x200

#define MIN_STR_ATOM  0xc000
#define MAX_ATOMS     0x4000

struct atom_entry
{
	struct atom_entry *next;   /* hash table list */
	struct atom_entry *prev;   /* hash table list */
	int                count;  /* reference count */
	atom_t             atom;   /* atom handle */
	unsigned short     hash;   /* string hash */
	unsigned short     len;    /* string len */
	WCHAR              str[];  /* atom string */
};

struct atom_table
{
	struct object       obj;                 /* object header */
	int                 count;               /* number of used atoms */
	struct atom_entry  *atoms[MAX_ATOMS];    /* atom entries */
	struct atom_entry  *entries[HASH_SIZE];  /* hash table entries */
};

static void atom_table_destroy(struct object *obj);

static const struct object_ops atom_table_ops =
{
	.size    = sizeof(struct atom_table),
	.destroy = atom_table_destroy,
};

static struct atom_table *global_table;
static struct atom_table *user_table;

static void add_atom_asciiz(struct atom_table *table, const char *name)
{
	WCHAR buffer[MAX_ATOM_LEN + 1];
	struct unicode_str str = { buffer, strlen(name) * sizeof(WCHAR) };

	for (int i = 0; i < str.len / sizeof(WCHAR); i++)
		buffer[i] = name[i];
	add_atom(table, str);
}

/* the tables are made when win32k starts, as wineserver makes them at its own
 * start; a table is large (256 KB), so it comes from vmalloc */
static struct atom_table *create_atom_table(void)
{
	struct atom_table *table = vzalloc(sizeof(*table));

	if (!table)
		return NULL;
	table->obj.refcount = 1;
	table->obj.ops = &atom_table_ops;
	table->count = 1; /* atom 0xc000 is reserved */
	return table;
}

int win32k_atoms_init(void)
{
	static const char *const global_atoms[] =
	{
		"StdExit", "StdNewDocument", "StdOpenDocument", "StdEditDocument", "StdNewfromTemplate",
		"StdCloseDocument", "StdShowItem", "StdDoVerbItem", "System", "OLEsystem", "StdDocumentName",
		"Protocols", "Topics", "Formats", "Status", "EditEnvItems", "True", "False", "Change", "Save",
		"Close", "MSDraw", "CC32SubclassInfo",
	};
	static const char *const user_atoms[] =
	{
		"USER32", "ObjectLink", "OwnerLink", "Native", "Binary", "FileName", "FileNameW",
		"NetworkName", "DataObject", "Embedded Object", "Embed Source", "Custom Link Source",
		"Link Source", "Object Descriptor", "Link Source Descriptor", "OleDraw", "PBrush", "MSDraw",
		"Ole Private Data", "Screen Picture", "OleClipboardPersistOnFlush", "MoreOlePrivateData",
		"Button", "Edit", "Static", "ListBox", "ScrollBar", "ComboBox",
	};

	if (!(global_table = create_atom_table()) || !(user_table = create_atom_table()))
		return -ENOMEM;
	for (size_t i = 0; i < ARRAY_SIZE(global_atoms); i++)
		add_atom_asciiz(global_table, global_atoms[i]);
	for (size_t i = 0; i < ARRAY_SIZE(user_atoms); i++)
		add_atom_asciiz(user_table, user_atoms[i]);
	return 0;
}

struct atom_table *get_global_atom_table(void)
{
	return global_table;
}

struct atom_table *get_user_atom_table(void)
{
	return user_table;
}

/* retrieve an entry pointer from its atom */
static struct atom_entry *get_atom_entry(struct atom_table *table, atom_t atom)
{
	struct atom_entry *entry = NULL;

	if (table && (atom >= MIN_STR_ATOM) && (atom < MIN_STR_ATOM + table->count))
		entry = table->atoms[atom - MIN_STR_ATOM];
	if (!entry)
		set_error(STATUS_INVALID_HANDLE);
	return entry;
}

/* add an atom entry in the table and return its handle */
static atom_t add_atom_entry(struct atom_table *table, struct atom_entry *entry)
{
	int i;

	for (i = 1 /* atom 0xc000 is reserved */; i < table->count; i++)
		if (!table->atoms[i])
			break;
	if (i == ARRAY_SIZE(table->atoms))
		return 0;
	if (i == table->count)
		table->count++;
	table->atoms[i] = entry;
	entry->atom = i + MIN_STR_ATOM;
	return entry->atom;
}

/* destroy the atom table */
static void atom_table_destroy(struct object *obj)
{
	struct atom_table *table = (struct atom_table *)obj;

	for (int i = 0; i < table->count; i++)
		kfree(table->atoms[i]);
}

static atom_t get_int_atom_value(struct unicode_str name)
{
	const WCHAR *ptr = name.str;
	const WCHAR *end = ptr + name.len / sizeof(WCHAR);
	unsigned int ret = 0;

	if (*ptr++ != '#')
		return 0;
	while (ptr < end) {
		if (*ptr < '0' || *ptr > '9')
			return 0;
		ret = ret * 10 + *ptr++ - '0';
		if (ret >= MAXINTATOM)
			return 0;
	}
	return ret;
}

/* find an atom entry in its hash list */
static struct atom_entry *find_atom_entry(struct atom_table *table, struct unicode_str str,
					  unsigned short hash)
{
	struct atom_entry *entry = table->entries[hash];

	while (entry) {
		if (entry->len == str.len && !memicmp_strW(entry->str, str.str, str.len))
			break;
		entry = entry->next;
	}
	return entry;
}

/* add an atom to the table */
atom_t add_atom(struct atom_table *table, struct unicode_str str)
{
	struct atom_entry *entry;
	unsigned short hash;
	atom_t atom = 0;

	if (!str.len) {
		set_error(STATUS_OBJECT_NAME_INVALID);
		return 0;
	}
	if (str.len > MAX_ATOM_LEN * sizeof(WCHAR)) {
		set_error(STATUS_INVALID_PARAMETER);
		return 0;
	}
	if ((atom = get_int_atom_value(str)))
		return atom;

	hash = hash_strW(str.str, str.len, ARRAY_SIZE(table->entries));
	if ((entry = find_atom_entry(table, str, hash))) {  /* exists already */
		entry->count++;
		return entry->atom;
	}

	if ((entry = mem_alloc(struct_size(entry, str, str.len / sizeof(WCHAR))))) {
		if ((atom = add_atom_entry(table, entry))) {
			entry->prev = NULL;
			if ((entry->next = table->entries[hash]))
				entry->next->prev = entry;
			table->entries[hash] = entry;
			entry->count = 1;
			entry->hash = hash;
			entry->len = str.len;
			memcpy(entry->str, str.str, str.len);
		} else {
			set_error(STATUS_NO_MEMORY);
			kfree(entry);
		}
	}
	return atom;
}

/* delete an atom from the table */
static void delete_atom(struct atom_table *table, atom_t atom, int if_pinned)
{
	struct atom_entry *entry = get_atom_entry(table, atom);

	if (!entry)
		return;
	if (!--entry->count) {
		if (entry->next)
			entry->next->prev = entry->prev;
		if (entry->prev)
			entry->prev->next = entry->next;
		else
			table->entries[entry->hash] = entry->next;
		table->atoms[atom - MIN_STR_ATOM] = NULL;
		kfree(entry);
	}
}

/* find an atom in the table */
atom_t find_atom(struct atom_table *table, struct unicode_str str)
{
	struct atom_entry *entry;
	unsigned short hash;
	atom_t atom;

	if (!str.len) {
		set_error(STATUS_OBJECT_NAME_INVALID);
		return 0;
	}
	if (str.len > MAX_ATOM_LEN * sizeof(WCHAR)) {
		set_error(STATUS_INVALID_PARAMETER);
		return 0;
	}
	if ((atom = get_int_atom_value(str)))
		return atom;

	hash = hash_strW(str.str, str.len, ARRAY_SIZE(table->entries));
	if (!(entry = find_atom_entry(table, str, hash))) {
		set_error(STATUS_OBJECT_NAME_NOT_FOUND);
		return 0;
	}
	return entry->atom;
}

/* increment the ref count of a global atom; used for window properties */
atom_t grab_atom(struct atom_table *table, atom_t atom)
{
	if (atom >= MIN_STR_ATOM) {
		struct atom_entry *entry = get_atom_entry(table, atom);

		if (!entry)
			return 0;
		entry->count++;
	}
	return atom;
}

/* decrement the ref count of a global atom; used for window properties */
void release_atom(struct atom_table *table, atom_t atom)
{
	if (atom >= MIN_STR_ATOM)
		delete_atom(table, atom, 1);
}

/* add a global atom */
DECL_HANDLER(add_atom)
{
	reply->atom = add_atom(global_table, get_req_unicode_str());
}

/* delete a global atom */
DECL_HANDLER(delete_atom)
{
	delete_atom(global_table, req->atom, 0);
}

/* find a global atom */
DECL_HANDLER(find_atom)
{
	reply->atom = find_atom(global_table, get_req_unicode_str());
}

/* get global atom name */
DECL_HANDLER(get_atom_information)
{
	struct atom_entry *entry;

	if ((entry = get_atom_entry(global_table, req->atom))) {
		set_reply_data(entry->str, min_t(data_size_t, entry->len, get_reply_max_size()));
		reply->count = entry->count;
		reply->pinned = 0;
		reply->total = entry->len;
	} else {
		reply->count = -1;
	}
}

/* add a user atom */
DECL_HANDLER(add_user_atom)
{
	reply->atom = add_atom(user_table, get_req_unicode_str());
}

/* get a user atom name */
DECL_HANDLER(get_user_atom_name)
{
	struct atom_entry *entry;

	if ((entry = get_atom_entry(user_table, req->atom))) {
		set_reply_data(entry->str, min_t(data_size_t, entry->len, get_reply_max_size()));
		reply->total = entry->len;
	}
}
