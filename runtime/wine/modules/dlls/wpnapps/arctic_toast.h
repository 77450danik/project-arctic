/*
 * A program's toast on its way to the shell, which shows it with the
 * notification area's balloons: WM_COPYDATA to Shell_TrayWnd. What becomes
 * of it comes back to the window in reply, as the registered message
 * ArcticToastEvent with the toast's id and one of ARCTIC_TOAST_*.
 *
 * The same file is in the ReactOS shell (base/shell/explorer).
 */

#ifndef __ARCTIC_TOAST_H
#define __ARCTIC_TOAST_H

#define ARCTIC_TOAST_SHOW   0x54535241 /* COPYDATASTRUCT.dwData, 'ARST': a struct arctic_toast */
#define ARCTIC_TOAST_HIDE   0x48535241 /* 'ARSH': a struct arctic_toast with only reply and id */

#define ARCTIC_TOAST_EVENT  L"ArcticToastEvent"

/* what became of it; the first three are ToastDismissalReason's */
#define ARCTIC_TOAST_USER_CANCELED      0
#define ARCTIC_TOAST_APPLICATION_HIDDEN 1
#define ARCTIC_TOAST_TIMED_OUT          2
#define ARCTIC_TOAST_ACTIVATED          3

#define ARCTIC_TOAST_FOREVER 0xffffffff

struct arctic_toast
{
    DWORD size;
    DWORD reply;          /* the window its events go to */
    DWORD id;
    DWORD duration;       /* milliseconds; 0: the system's; ARCTIC_TOAST_FOREVER: until dismissed */
    DWORD image_circle;   /* hint-crop="circle" */
    WCHAR app[64];        /* the program's name */
    WCHAR icon[260];      /* the file with the program's icon */
    WCHAR title[64];
    WCHAR text[256];
    WCHAR attribution[64];
    WCHAR image[260];     /* appLogoOverride: an image file */
};

#endif
