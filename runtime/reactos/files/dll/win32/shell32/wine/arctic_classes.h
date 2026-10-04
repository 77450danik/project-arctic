/* Arctic: shell32 classes taken from Wine (C, not ATL); arctic_classes.c
 * hands out their class factories before shell32's ATL object map */
#ifndef ARCTIC_CLASSES_H
#define ARCTIC_CLASSES_H

#ifndef CLSID_NamespaceTreeControl
static const GUID CLSID_NamespaceTreeControl_arctic = { 0xae054212, 0x3535, 0x4430, { 0x83, 0xed, 0xd5, 0x01, 0xaa, 0x66, 0x80, 0xe6 } };
#define CLSID_NamespaceTreeControl CLSID_NamespaceTreeControl_arctic
#endif


/* names ReactOS's headers lack */
static const GUID CLSID_FileOperation_arctic = { 0x3ad05575, 0x8857, 0x4850, { 0x92, 0x77, 0x11, 0xb8, 0x5b, 0xdb, 0x8e, 0x09 } };
#define CLSID_FileOperation CLSID_FileOperation_arctic
#ifndef CWM_GETISHELLBROWSER
#define CWM_GETISHELLBROWSER (WM_USER + 7)
#endif
#ifndef FWF_NONE
#define FWF_NONE 0
#endif
#ifndef FOF_NO_UI
#define FOF_NO_UI (FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR)
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI ExplorerBrowser_Constructor(IUnknown *outer, REFIID riid, void **ppv);
HRESULT WINAPI IFileOperation_Constructor(IUnknown *outer, REFIID riid, void **ppv);
HRESULT Arctic_DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv);

#ifdef __cplusplus
}
#endif

#endif
