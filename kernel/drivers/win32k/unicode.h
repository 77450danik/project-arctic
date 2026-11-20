/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: strings as the NT side compares them (Wine's server/unicode.c).
 */
#ifndef WIN32K_UNICODE_H
#define WIN32K_UNICODE_H

#include <linux/types.h>
#include "protocol.h"

extern const u16 w32_casemap[];	/* casemap.c, from l_intl.nls */

static inline WCHAR to_lower(WCHAR ch)
{
	return ch + w32_casemap[w32_casemap[w32_casemap[ch >> 8] + ((ch >> 4) & 0x0f)] + (ch & 0x0f)];
}

struct unicode_str
{
	const WCHAR *str;
	data_size_t len;	/* in bytes */
};

static inline int memicmp_strW(const WCHAR *str1, const WCHAR *str2, data_size_t len)
{
	int ret = 0;

	for (len /= sizeof(WCHAR); len; str1++, str2++, len--)
		if ((ret = to_lower(*str1) - to_lower(*str2)))
			break;
	return ret;
}

static inline unsigned int hash_strW(const WCHAR *str, data_size_t len, unsigned int hash_size)
{
	unsigned int i, hash = 0;

	for (i = 0; i < len / sizeof(WCHAR); i++)
		hash = hash * 65599 + to_lower(str[i]);
	return hash % hash_size;
}

#endif
