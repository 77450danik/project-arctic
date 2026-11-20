/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: what of Wine's own headers (ntuser.h, server/user.h, thread.h) the
 * handlers use and windefs.h cannot take (enums, function-like macros).
 */
#ifndef WIN32K_USERDEFS_H
#define WIN32K_USERDEFS_H

#define MAKELONG(low, high)	((u32)(((u16)(low)) | ((u32)((u16)(high))) << 16))
#define LOWORD(l)		((u16)((u32)(l) & 0xffff))
#define HIWORD(l)		((u16)((u32)(l) >> 16))

#define DESKTOP_ATOM		((atom_t)32769)

/* NtUserInitializeClientPfnArrays (Wine's ntuser.h) */
enum ntuser_client_procs
{
	NTUSER_WNDPROC_SCROLLBAR,
	NTUSER_WNDPROC_MESSAGE,
	NTUSER_WNDPROC_MENU,
	NTUSER_WNDPROC_DESKTOP,
	NTUSER_WNDPROC_DEFWND,
	NTUSER_WNDPROC_ICONTITLE,
	NTUSER_WNDPROC_UNKNOWN,
	NTUSER_WNDPROC_BUTTON,
	NTUSER_WNDPROC_COMBO,
	NTUSER_WNDPROC_COMBOLBOX,
	NTUSER_WNDPROC_DIALOG,
	NTUSER_WNDPROC_EDIT,
	NTUSER_WNDPROC_LISTBOX,
	NTUSER_WNDPROC_MDICLIENT,
	NTUSER_WNDPROC_STATIC,
	NTUSER_WNDPROC_IME,
	NTUSER_WNDPROC_GHOST,
	NTUSER_NB_PROCS
};

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef u16 WORD;

/* DPI awareness (windef.h) and contexts (Wine's ntuser.h) */
enum
{
	DPI_AWARENESS_INVALID = -1,
	DPI_AWARENESS_UNAWARE = 0,
	DPI_AWARENESS_SYSTEM_AWARE,
	DPI_AWARENESS_PER_MONITOR_AWARE
};
#define MAKE_NTUSER_DPI_CONTEXT( awareness, version, dpi, flags )  ((awareness) | ((version) << 4) | ((dpi) << 8) | (flags))
#define NTUSER_DPI_CONTEXT_GET_AWARENESS( ctx )                    ((ctx) & 0x0f)
#define NTUSER_DPI_CONTEXT_GET_VERSION( ctx )                      (((ctx) & 0xf0) >> 4)
#define NTUSER_DPI_CONTEXT_GET_DPI( ctx )                          ((((ctx) & 0x1ff00) >> 8))
#define NTUSER_DPI_CONTEXT_GET_FLAGS( ctx )                        ((ctx) & 0xfffe0000)
#define NTUSER_DPI_CONTEXT_IS_MONITOR_AWARE( ctx )  (NTUSER_DPI_CONTEXT_GET_AWARENESS( ctx ) == DPI_AWARENESS_PER_MONITOR_AWARE)
#define NTUSER_DPI_UNAWARE                MAKE_NTUSER_DPI_CONTEXT( DPI_AWARENESS_UNAWARE, 1, USER_DEFAULT_SCREEN_DPI, 0 )
#define NTUSER_DPI_PER_MONITOR_AWARE      MAKE_NTUSER_DPI_CONTEXT( DPI_AWARENESS_PER_MONITOR_AWARE, 1, 0, 0 )
#define NTUSER_DPI_PER_MONITOR_AWARE_V2   MAKE_NTUSER_DPI_CONTEXT( DPI_AWARENESS_PER_MONITOR_AWARE, 2, 0, 0 )

/* where an input message came from (winuser.h) */
enum
{
	IMDT_UNAVAILABLE = 0x00,
	IMDT_KEYBOARD    = 0x01,
	IMDT_MOUSE       = 0x02,
	IMDT_TOUCH       = 0x04,
	IMDT_PEN         = 0x08,
	IMDT_TOUCHPAD    = 0x10,
};
enum
{
	IMO_UNAVAILABLE = 0x00,
	IMO_HARDWARE    = 0x01,
	IMO_INJECTED    = 0x02,
	IMO_SYSTEM      = 0x04,
};

typedef struct { s32 x, y; } POINT;
typedef int BOOL;

#define MAKEWPARAM(low, high)		((lparam_t)MAKELONG(low, high))
#define GET_WHEEL_DELTA_WPARAM(wp)	((short)HIWORD(wp))
#define GET_KEYSTATE_WPARAM(wp)		(LOWORD(wp))
#define UlongToHandle(ul)		((u64)(unsigned long)(ul))

/* kbd.h: a virtual key of the numeric keypad */
#define KBDNUMPAD			0x800

/* hidusage.h */
#define HID_USAGE_PAGE_GENERIC		((u16)0x01)
#define HID_USAGE_GENERIC_MOUSE		((u16)0x02)
#define HID_USAGE_GENERIC_KEYBOARD	((u16)0x06)

/* raw input (winuser.h), as wineserver lays it out: a 64-bit build, so
 * RAWINPUTHEADER has a 64-bit hDevice and wParam; win32u is 64-bit for every
 * process (WoW64 converts above it) */
typedef struct
{
	u32 dwType;
	u32 dwSize;
	u64 hDevice;
	u64 wParam;
} RAWINPUTHEADER;

typedef struct
{
	u16 usFlags;
	union
	{
		u32 ulButtons;
		struct
		{
			u16 usButtonFlags;
			u16 usButtonData;
		};
	};
	u32 ulRawButtons;
	s32 lLastX;
	s32 lLastY;
	u32 ulExtraInformation;
} RAWMOUSE;

typedef struct
{
	u16 MakeCode;
	u16 Flags;
	u16 Reserved;
	u16 VKey;
	u32 Message;
	u32 ExtraInformation;
} RAWKEYBOARD;

typedef struct
{
	u32 dwSizeHid;
	u32 dwCount;
	u8 bRawData[1];
} RAWHID;

typedef struct
{
	RAWINPUTHEADER header;
	union
	{
		RAWMOUSE mouse;
		RAWKEYBOARD keyboard;
		RAWHID hid;
	} data;
} RAWINPUT;

/* the server's mem_append: copies and gives the end */
static inline void *mem_append(void *ptr, const void *src, size_t size)
{
	if (!size)
		return ptr;
	memcpy(ptr, src, size);
	return (char *)ptr + size;
}

/* IMAGE_FILE_MACHINE_* of a 64-bit process (the server's is_machine_64bit) */
static inline int is_machine_64bit(unsigned short machine)
{
	return machine == 0x8664 /* AMD64 */ || machine == 0xaa64 /* ARM64 */;
}

/* Wine's own messages (ntuser.h) */
enum wine_internal_message
{
	WM_WINE_DESTROYWINDOW = 0x80000000,
	WM_WINE_SETWINDOWPOS,
	WM_WINE_SHOWWINDOW,
	WM_WINE_SETPARENT,
	WM_WINE_SETWINDOWLONG,
	WM_WINE_SETSTYLE,
	WM_WINE_SETACTIVEWINDOW,
	WM_WINE_KEYBOARD_LL_HOOK,
	WM_WINE_MOUSE_LL_HOOK,
	WM_WINE_IME_NOTIFY,
	WM_WINE_WINDOW_STATE_CHANGED,
	WM_WINE_UPDATEWINDOWSTATE,
	WM_WINE_TRACKMOUSEEVENT,
	WM_WINE_SETPIXELFORMAT,
	WM_WINE_FIRST_DRIVER_MSG = 0x80001000,  /* range of messages reserved for the USER driver */
	WM_WINE_CLIPCURSOR = 0x80001ff0, /* internal driver notification messages */
	WM_WINE_SETCURSOR,
	WM_WINE_LAST_DRIVER_MSG = 0x80001fff
};

/* a Win32 error as an NTSTATUS, as the server's set_win32_error gives it */
#define set_win32_error(err) set_error((NTSTATUS)(0xc0010000 | (err)))

#endif
