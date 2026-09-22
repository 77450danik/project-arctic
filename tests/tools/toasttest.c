/* A program's toast, the way Chrome and Electron show theirs: through
 * Windows.UI.Notifications, from a desktop program with an AppUserModelID.
 *
 *   toasttest [template]
 *
 * By default the toast is ToastGeneric XML loaded into an XmlDocument; with
 * "template" it is ToastText02 from GetTemplateContent, filled in through the
 * DOM. What becomes of it (clicked, closed, timed out) comes up as a message
 * box, and on stdout.
 *
 *   zig cc -target x86_64-windows-gnu -Os -s toasttest.c -o toasttest.exe -lole32 -luser32
 */
#define COBJMACROS
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <stdio.h>

typedef struct { INT64 value; } EventRegistrationToken;

typedef HRESULT (WINAPI *RoInitialize_t)(RO_INIT_TYPE);
typedef HRESULT (WINAPI *RoGetActivationFactory_t)(HSTRING, REFIID, void **);
typedef HRESULT (WINAPI *RoActivateInstance_t)(HSTRING, IInspectable **);
typedef HRESULT (WINAPI *WindowsCreateString_t)(const WCHAR *, UINT32, HSTRING *);
typedef const WCHAR *(WINAPI *WindowsGetStringRawBuffer_t)(HSTRING, UINT32 *);

static RoInitialize_t pRoInitialize;
static RoGetActivationFactory_t pRoGetActivationFactory;
static RoActivateInstance_t pRoActivateInstance;
static WindowsCreateString_t pWindowsCreateString;
static WindowsGetStringRawBuffer_t pWindowsGetStringRawBuffer;

static const GUID IID_IXmlDocumentIO = {0x6cd0e74e,0xee65,0x4489,{0x9e,0xbf,0xca,0x43,0xe8,0x7b,0xa6,0x37}};
static const GUID IID_IXmlNode = {0x1c741d59,0x2122,0x47d5,{0xa8,0x56,0x83,0xf3,0xd4,0x21,0x48,0x75}};
static const GUID IID_IXmlNodeSerializer = {0x5cc5b382,0xe6dd,0x4991,{0xab,0xef,0x06,0xd8,0xd2,0xe7,0xbd,0x0c}};
static const GUID IID_IToastNotificationManagerStatics = {0x50ac103f,0xd235,0x4598,{0xbb,0xef,0x98,0xfe,0x4d,0x1a,0x3a,0xd4}};
static const GUID IID_IToastNotificationFactory = {0x04124b20,0x82c6,0x4229,{0xb1,0x09,0xfd,0x9e,0xd4,0x66,0x2b,0x53}};
static const GUID IID_IToastActivatedEventArgs = {0xe3bf92f3,0xc197,0x436f,{0x82,0x65,0x06,0x25,0x82,0x4f,0x8d,0xac}};
static const GUID IID_ActivatedHandler = {0xab54de2d,0x97d9,0x5528,{0xb6,0xad,0x10,0x5a,0xfe,0x15,0x65,0x30}};
static const GUID IID_DismissedHandler = {0x61c2402f,0x0ed0,0x5a18,{0xab,0x69,0x59,0xf4,0xaa,0x99,0xa3,0x68}};

/* the n-th method of a WinRT interface, counting from the first after IInspectable's */
#define METHOD(obj, n, type) ((type)((*(void ***)(obj))[6 + (n)]))

static HSTRING hs(const WCHAR *s)
{
    HSTRING ret = NULL;
    pWindowsCreateString(s, lstrlenW(s), &ret);
    return ret;
}

static HANDLE done;
static WCHAR result[512];

struct handler
{
    void **vtbl;
    LONG ref;
    BOOL activated;
};

static HRESULT WINAPI handler_QueryInterface(struct handler *h, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, h->activated ? &IID_ActivatedHandler : &IID_DismissedHandler))
    {
        *out = h;
        InterlockedIncrement(&h->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI handler_AddRef(struct handler *h) { return InterlockedIncrement(&h->ref); }
static ULONG WINAPI handler_Release(struct handler *h) { return InterlockedDecrement(&h->ref); }

static HRESULT WINAPI activated_Invoke(struct handler *h, IInspectable *sender, IInspectable *args)
{
    IInspectable *activated;
    HSTRING arguments = NULL;

    lstrcpyW(result, L"Activated");
    if (args && SUCCEEDED(IInspectable_QueryInterface(args, &IID_IToastActivatedEventArgs, (void **)&activated)))
    {
        if (SUCCEEDED(METHOD(activated, 0, HRESULT (WINAPI *)(IInspectable *, HSTRING *))(activated, &arguments)))
            wsprintfW(result, L"Activated: %s", pWindowsGetStringRawBuffer(arguments, NULL));
        IInspectable_Release(activated);
    }
    SetEvent(done);
    return S_OK;
}

static HRESULT WINAPI dismissed_Invoke(struct handler *h, IInspectable *sender, IInspectable *args)
{
    static const WCHAR *reasons[] = { L"UserCanceled", L"ApplicationHidden", L"TimedOut" };
    int reason = -1;

    if (args)
        METHOD(args, 0, HRESULT (WINAPI *)(IInspectable *, int *))(args, &reason);
    wsprintfW(result, L"Dismissed: %s", reason >= 0 && reason < 3 ? reasons[reason] : L"?");
    SetEvent(done);
    return S_OK;
}

static void *activated_vtbl[] = { handler_QueryInterface, handler_AddRef, handler_Release, activated_Invoke };
static void *dismissed_vtbl[] = { handler_QueryInterface, handler_AddRef, handler_Release, dismissed_Invoke };
static struct handler on_activated = { activated_vtbl, 1, TRUE };
static struct handler on_dismissed = { dismissed_vtbl, 1, FALSE };

#define CHECK(hr, what) do { if (FAILED(hr)) { printf("%s failed: %#lx\n", what, (long)(hr)); return 1; } } while (0)

int main(int argc, char **argv)
{
    HMODULE combase = LoadLibraryW(L"combase.dll");
    IInspectable *doc = NULL, *io, *manager, *factory, *toast, *notifier, *serializer;
    BOOL template = argc > 1 && !strcmp(argv[1], "template");
    EventRegistrationToken token;
    HSTRING xml = NULL;
    HRESULT hr;

    pRoInitialize = (RoInitialize_t)GetProcAddress(combase, "RoInitialize");
    pRoGetActivationFactory = (RoGetActivationFactory_t)GetProcAddress(combase, "RoGetActivationFactory");
    pRoActivateInstance = (RoActivateInstance_t)GetProcAddress(combase, "RoActivateInstance");
    pWindowsCreateString = (WindowsCreateString_t)GetProcAddress(combase, "WindowsCreateString");
    pWindowsGetStringRawBuffer = (WindowsGetStringRawBuffer_t)GetProcAddress(combase, "WindowsGetStringRawBuffer");
    CHECK(pRoInitialize(RO_INIT_MULTITHREADED), "RoInitialize");
    done = CreateEventW(NULL, TRUE, FALSE, NULL);

    hr = pRoGetActivationFactory(hs(L"Windows.UI.Notifications.ToastNotificationManager"),
                                 &IID_IToastNotificationManagerStatics, (void **)&manager);
    CHECK(hr, "ToastNotificationManager");

    if (template)
    {
        IInspectable *texts, *item, *text, *node, *appended;
        static const WCHAR *lines[] = { L"Тост через шаблон", L"ToastText02, заповнений через XmlDocument" };

        hr = METHOD(manager, 2, HRESULT (WINAPI *)(IInspectable *, int, IInspectable **))(manager, 5 /* ToastText02 */, &doc);
        CHECK(hr, "GetTemplateContent");
        hr = METHOD(doc, 10, HRESULT (WINAPI *)(IInspectable *, HSTRING, IInspectable **))(doc, hs(L"text"), &texts);
        CHECK(hr, "GetElementsByTagName");
        for (UINT32 i = 0; i < 2; i++)
        {
            hr = METHOD(texts, 1, HRESULT (WINAPI *)(IInspectable *, UINT32, IInspectable **))(texts, i, &item);
            CHECK(hr, "Item");
            hr = METHOD(doc, 5, HRESULT (WINAPI *)(IInspectable *, HSTRING, IInspectable **))(doc, hs(lines[i]), &text);
            CHECK(hr, "CreateTextNode");
            CHECK(IInspectable_QueryInterface(text, &IID_IXmlNode, (void **)&node), "IXmlNode");
            hr = METHOD(item, 16, HRESULT (WINAPI *)(IInspectable *, IInspectable *, IInspectable **))(item, node, &appended);
            CHECK(hr, "AppendChild");
        }
    }
    else
    {
        CHECK(pRoActivateInstance(hs(L"Windows.Data.Xml.Dom.XmlDocument"), &doc), "XmlDocument");
        CHECK(IInspectable_QueryInterface(doc, &IID_IXmlDocumentIO, (void **)&io), "IXmlDocumentIO");
        hr = METHOD(io, 0, HRESULT (WINAPI *)(IInspectable *, HSTRING))(io, hs(
            L"<toast launch=\"arctic-test\"><visual><binding template=\"ToastGeneric\">"
            L"<text>Нове повідомлення</text>"
            L"<text>Це тост програми, показаний оболонкою як у Windows 10</text>"
            L"<text placement=\"attribution\">через toasttest</text>"
            L"</binding></visual></toast>"));
        CHECK(hr, "LoadXml");
    }
    if (SUCCEEDED(IInspectable_QueryInterface(doc, &IID_IXmlNodeSerializer, (void **)&serializer)) &&
        SUCCEEDED(METHOD(serializer, 0, HRESULT (WINAPI *)(IInspectable *, HSTRING *))(serializer, &xml)))
        printf("%ls\n", pWindowsGetStringRawBuffer(xml, NULL));

    hr = pRoGetActivationFactory(hs(L"Windows.UI.Notifications.ToastNotification"), &IID_IToastNotificationFactory,
                                 (void **)&factory);
    CHECK(hr, "ToastNotification");
    hr = METHOD(factory, 0, HRESULT (WINAPI *)(IInspectable *, IInspectable *, IInspectable **))(factory, doc, &toast);
    CHECK(hr, "CreateToastNotification");
    METHOD(toast, 3, HRESULT (WINAPI *)(IInspectable *, void *, EventRegistrationToken *))(toast, &on_dismissed, &token);
    METHOD(toast, 5, HRESULT (WINAPI *)(IInspectable *, void *, EventRegistrationToken *))(toast, &on_activated, &token);

    hr = METHOD(manager, 1, HRESULT (WINAPI *)(IInspectable *, HSTRING, IInspectable **))(manager, hs(L"Arctic.ToastTest"),
                                                                                          &notifier);
    CHECK(hr, "CreateToastNotifierWithId");
    hr = METHOD(notifier, 0, HRESULT (WINAPI *)(IInspectable *, IInspectable *))(notifier, toast);
    CHECK(hr, "Show");
    printf("shown\n");
    fflush(stdout);

    if (WaitForSingleObject(done, 60000))
        lstrcpyW(result, L"no event in 60 s");
    printf("%ls\n", result);
    MessageBoxW(NULL, result, L"toasttest", MB_OK | MB_SETFOREGROUND);
    return 0;
}
