/*
 * What programs play, for the shell's media controls
 *
 * In Windows a program tells the system what it plays through
 * SystemMediaTransportControls, and the shell shows it beside the volume and
 * sends back the presses of its buttons. In Arctic windows.media.mediacontrol
 * keeps every program's session in this shared section (one row a window
 * that asked for the controls), and the volume overlay and flyout of
 * sndvolsso.dll read it. A press goes back as the registered message
 * ARCTIC_MEDIA_BUTTON_MSG posted to the session's notify window, which a
 * thread of the program keeps: wparam is the SystemMediaTransportControlsButton,
 * lparam the session's id.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_MEDIA_H
#define __WINE_ARCTIC_MEDIA_H

#define ARCTIC_MEDIA_SECTION     L"Global\\__arctic_media"
#define ARCTIC_MEDIA_MUTEX       L"Global\\__arctic_media_lock"
#define ARCTIC_MEDIA_BUTTON_MSG  L"ArcticMediaButton"
#define ARCTIC_MEDIA_VERSION     2
#define ARCTIC_MEDIA_SESSIONS    16

/* the buttons a session takes, 1 << SystemMediaTransportControlsButton */
#define ARCTIC_MEDIA_PLAY        (1 << 0)
#define ARCTIC_MEDIA_PAUSE       (1 << 1)
#define ARCTIC_MEDIA_STOP        (1 << 2)
#define ARCTIC_MEDIA_NEXT        (1 << 6)
#define ARCTIC_MEDIA_PREVIOUS    (1 << 7)

struct arctic_media_session
{
    UINT32 id;               /* 0: a free row */
    UINT32 process;
    UINT32 window;           /* the window the program asked the controls for */
    UINT32 notify;           /* the window that hears the presses of its buttons */
    UINT32 enabled;          /* ISystemMediaTransportControls::IsEnabled */
    UINT32 status;           /* MediaPlaybackStatus */
    UINT32 buttons;          /* ARCTIC_MEDIA_* */
    UINT32 changed;          /* GetTickCount() of the last change */
    WCHAR  title[128];
    WCHAR  artist[128];
    WCHAR  album[128];
    /* the album art (DisplayUpdater.Thumbnail): an image file the program's
     * process wrote, art_serial moving with each new one; "" for none */
    UINT32 art_serial;
    WCHAR  art[260];
};

struct arctic_media_shared
{
    UINT32        version;
    volatile LONG serial;    /* moves after every change */
    UINT32        next_id;
    struct arctic_media_session sessions[ARCTIC_MEDIA_SESSIONS];
};

/* What a RandomAccessStreamReference of windows.storage was made from, for
 * windows.media.mediacontrol to read the album art without OpenReadAsync:
 * the Uri of CreateFromUri, or the IRandomAccessStream of CreateFromStream.
 * Defined where ARCTIC_MEDIA_STREAM_REFERENCE is, after the COM and WinRT
 * headers. */
#ifdef ARCTIC_MEDIA_STREAM_REFERENCE
#ifndef __IArcticStreamReferenceSource_INTERFACE_DEFINED__
#define __IArcticStreamReferenceSource_INTERFACE_DEFINED__
/* in the one file of a module that uses it */
#define DEFINE_ARCTIC_STREAM_REFERENCE_SOURCE_IID \
    static const IID IID_IArcticStreamReferenceSource = \
        { 0x6b1f9c3e, 0x2a4d, 0x4c8e, { 0x9f, 0x17, 0x3d, 0x5a, 0x7e, 0x2b, 0x8c, 0x41 } }
typedef struct IArcticStreamReferenceSource IArcticStreamReferenceSource;
typedef struct IArcticStreamReferenceSourceVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( IArcticStreamReferenceSource *iface, REFIID iid, void **out );
    ULONG   (STDMETHODCALLTYPE *AddRef)( IArcticStreamReferenceSource *iface );
    ULONG   (STDMETHODCALLTYPE *Release)( IArcticStreamReferenceSource *iface );
    /* the URI's text, or NULL */
    HRESULT (STDMETHODCALLTYPE *GetUri)( IArcticStreamReferenceSource *iface, HSTRING *uri );
    /* the IRandomAccessStream, AddRef'ed, or NULL */
    HRESULT (STDMETHODCALLTYPE *GetStream)( IArcticStreamReferenceSource *iface, IUnknown **stream );
} IArcticStreamReferenceSourceVtbl;
struct IArcticStreamReferenceSource { const IArcticStreamReferenceSourceVtbl *lpVtbl; };
#endif
#endif

#endif  /* __WINE_ARCTIC_MEDIA_H */
