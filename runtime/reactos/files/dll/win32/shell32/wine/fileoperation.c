/*
 * IFileOperation (move only, as Wine has it)
 *
 * Copyright 2000 Juergen Schmied
 * Copyright 2002 Andriy Palamarchuk
 * Copyright 2004 Dietrich Teickner (from Code Weavers)
 * Copyright 2004 Rolf Kalbermatter
 * Copyright 2019 Jactry Zeng for CodeWeavers
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

/* Arctic: taken from Wine 10's shlfileop.c. Chromium moves a finished
 * download from .crdownload to its name with IFileOperation::MoveItem */

#include <stdarg.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <debughlp.h>

#include <wine/list.h>
#include <wine/debug.h>

#include "arctic_classes.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#ifndef COPYENGINE_S_DONT_PROCESS_CHILDREN
#define COPYENGINE_S_DONT_PROCESS_CHILDREN _HRESULT_TYPEDEF_(0x00270008)
#define COPYENGINE_S_NOT_HANDLED           _HRESULT_TYPEDEF_(0x00270003)
#define COPYENGINE_E_FLD_IS_FILE_DEST      _HRESULT_TYPEDEF_(0x8027000b)
#define COPYENGINE_E_FILE_IS_FLD_DEST      _HRESULT_TYPEDEF_(0x8027000c)
#endif

#define IsAttrib(x, y)  ((INVALID_FILE_ATTRIBUTES != (x)) && ((x) & (y)))
#define IsAttribFile(x) (!((x) & FILE_ATTRIBUTE_DIRECTORY))
#define IsAttribDir(x)  IsAttrib(x, FILE_ATTRIBUTE_DIRECTORY)

enum copy_engine_opcode
{
    COPY_ENGINE_MOVE,
    COPY_ENGINE_REMOVE_DIRECTORY_SILENT,
};

#define TSF_UNKNOWN_MEGRE_FLAG 0x1000

struct copy_engine_operation
{
    struct list entry;
    IFileOperationProgressSink *sink;
    enum copy_engine_opcode opcode;
    IShellItem *folder;
    PIDLIST_ABSOLUTE item_pidl;
    WCHAR *name;
    DWORD tsf;
};

struct file_operation_sink
{
    struct list entry;
    IFileOperationProgressSink *sink;
    DWORD cookie;
};

struct file_operation
{
    IFileOperation IFileOperation_iface;
    LONG ref;
    struct list sinks;
    DWORD next_cookie;
    struct list ops;
    DWORD flags;
    BOOL aborted;
    unsigned int progress_total, progress_sofar;
};

static void free_file_operation_ops(struct file_operation *operation)
{
    struct copy_engine_operation *op, *next;

    LIST_FOR_EACH_ENTRY_SAFE(op, next, &operation->ops, struct copy_engine_operation, entry)
    {
        if (op->sink)
            IFileOperationProgressSink_Release(op->sink);
        ILFree(op->item_pidl);
        if (op->folder)
            IShellItem_Release(op->folder);
        CoTaskMemFree(op->name);
        list_remove(&op->entry);
        free(op);
    }
}

static HRESULT add_operation(struct file_operation *operation, enum copy_engine_opcode opcode, IShellItem *item,
        IShellItem *folder, const WCHAR *name, IFileOperationProgressSink *sink, DWORD tsf, struct list *add_after)
{
    struct copy_engine_operation *op;
    HRESULT hr;

    if (!name)
        name = L"";

    if (!(op = calloc(1, sizeof(*op))))
        return E_OUTOFMEMORY;

    op->opcode = opcode;
    if (item && FAILED((hr = SHGetIDListFromObject((IUnknown *)item, &op->item_pidl))))
    {
        hr = E_INVALIDARG;
        goto error;
    }

    op->tsf = tsf;
    if (folder)
    {
        IShellItem_AddRef(folder);
        op->folder = folder;
    }
    if (!(op->name = wcsdup(name)))
    {
        hr = E_OUTOFMEMORY;
        goto error;
    }
    if (sink)
    {
        IFileOperationProgressSink_AddRef(sink);
        op->sink = sink;
    }
    if (add_after)
        list_add_after(add_after, &op->entry);
    else
        list_add_tail(&operation->ops, &op->entry);
    return S_OK;

error:
    ILFree(op->item_pidl);
    if (op->folder)
        IShellItem_Release(op->folder);
    if (op->sink)
        IFileOperationProgressSink_Release(sink);
    CoTaskMemFree(op->name);
    free(op);
    return hr;
}

static HRESULT file_operation_notify(struct file_operation *operation, struct copy_engine_operation *op, BOOL post_notif,
        void *context,
        HRESULT (*callback)(IFileOperationProgressSink *, struct file_operation *, struct copy_engine_operation *op, void *))
{
    struct file_operation_sink *op_sink;
    HRESULT hr = S_OK;

    if (op && op->sink && post_notif)
    {
        if (FAILED(hr = callback(op->sink, operation, op, context)))
            goto done;
    }
    LIST_FOR_EACH_ENTRY(op_sink, &operation->sinks, struct file_operation_sink, entry)
    {
        if (FAILED(hr = callback(op_sink->sink, operation, op, context)))
            goto done;
    }
    if (op && op->sink && !post_notif)
        hr = callback(op->sink, operation, op, context);

done:
    if (FAILED(hr))
        WARN("sink returned %#lx.\n", hr);
    return hr;
}

static HRESULT notify_start_operations(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    return IFileOperationProgressSink_StartOperations(sink);
}

static HRESULT notify_reset_timer(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    return IFileOperationProgressSink_ResetTimer(sink);
}

static HRESULT notify_finish_operations(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    return IFileOperationProgressSink_FinishOperations(sink, S_OK);
}

static HRESULT notify_update_progress(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    return IFileOperationProgressSink_UpdateProgress(sink, operations->progress_total, operations->progress_sofar);
}

struct notify_move_item_param
{
    IShellItem *item;
    IShellItem *new_item;
    HRESULT result;
};

static HRESULT notify_pre_move_item(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    struct notify_move_item_param *p = context;

    return IFileOperationProgressSink_PreMoveItem(sink, op->tsf & ~TSF_UNKNOWN_MEGRE_FLAG, p->item, op->folder, op->name);
}

static HRESULT notify_post_move_item(IFileOperationProgressSink *sink, struct file_operation *operations,
        struct copy_engine_operation *op, void *context)
{
    struct notify_move_item_param *p = context;

    return IFileOperationProgressSink_PostMoveItem(sink, op->tsf, p->item, op->folder, op->name, p->result, p->new_item);
}

static void set_file_operation_progress(struct file_operation *operation, unsigned int total, unsigned int sofar)
{
    operation->progress_total = total;
    operation->progress_sofar = sofar;
    file_operation_notify(operation, NULL, FALSE, NULL, notify_update_progress);
}

static HRESULT copy_engine_merge_dir(struct file_operation *operation, struct copy_engine_operation *op,
        const WCHAR *src_dir_path, IShellItem *dest_folder, struct list **add_after)
{
    struct list *add_files_after, *curr_add_after;
    WIN32_FIND_DATAW wfd;
    WCHAR path[MAX_PATH];
    IShellItem *item;
    HRESULT hr = S_OK;
    HANDLE fh;

    if (!PathCombineW(path, src_dir_path, L"*.*"))
        return HRESULT_FROM_WIN32(GetLastError());

    fh = FindFirstFileW(path, &wfd);
    if (fh == INVALID_HANDLE_VALUE) return S_OK;
    add_files_after = *add_after;
    do
    {
        if (!wcscmp(wfd.cFileName, L".") || !wcscmp(wfd.cFileName, L".."))
            continue;
        if (!PathCombineW(path, src_dir_path, wfd.cFileName))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (FAILED(hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&item)))
            break;

        /* Queue files before directories. */
        curr_add_after = IsAttribFile(wfd.dwFileAttributes) ? add_files_after : *add_after;
        hr = add_operation(operation, COPY_ENGINE_MOVE, item, dest_folder, NULL, op->sink,
                (op->tsf & ~TSF_COPY_LOCALIZED_NAME) | TSF_UNKNOWN_MEGRE_FLAG, curr_add_after);
        IShellItem_Release(item);
        if (FAILED(hr))
            break;
        if (curr_add_after == *add_after)
            *add_after = (*add_after)->next;
        if (IsAttribFile(wfd.dwFileAttributes))
            add_files_after = add_files_after->next;
    } while (FindNextFileW(fh, &wfd));
    FindClose(fh);
    return hr;
}

static HRESULT copy_engine_move(struct file_operation *operation, struct copy_engine_operation *op, IShellItem *src_item,
        IShellItem **dest_folder)
{
    WCHAR path[MAX_PATH], item_path[MAX_PATH];
    DWORD src_attrs, dst_attrs;
    struct list *add_after;
    WCHAR *str, *ptr;
    HRESULT hr;

    *dest_folder = NULL;
    if (FAILED(hr = IShellItem_GetDisplayName(src_item, SIGDN_FILESYSPATH, &str)))
        return hr;
    wcscpy_s(item_path, ARRAY_SIZE(item_path), str);
    if (!*op->name && (ptr = StrRChrW(str, NULL, '\\')))
    {
        free(op->name);
        op->name = wcsdup(ptr + 1);
    }
    CoTaskMemFree(str);

    if (FAILED(hr = IShellItem_GetDisplayName(op->folder, SIGDN_FILESYSPATH, &str)))
        return hr;
    hr = PathCombineW(path, str, op->name) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    CoTaskMemFree(str);
    if (FAILED(hr))
        return hr;

    if ((src_attrs = GetFileAttributesW(item_path)) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(GetLastError());
    dst_attrs = GetFileAttributesW(path);
    if (IsAttribFile(src_attrs) && IsAttribDir(dst_attrs))
        return COPYENGINE_E_FILE_IS_FLD_DEST;
    if (IsAttribDir(src_attrs) && IsAttribFile(dst_attrs))
        return COPYENGINE_E_FLD_IS_FILE_DEST;
    if (dst_attrs == INVALID_FILE_ATTRIBUTES || (IsAttribFile(src_attrs) && IsAttribFile(dst_attrs)))
    {
        if (MoveFileExW(item_path, path, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
        {
            if (FAILED((hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)dest_folder))))
                return hr;
            return COPYENGINE_S_DONT_PROCESS_CHILDREN;
        }
        IShellItem_Release(*dest_folder);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    /* Merge directory to existing directory. */
    if (FAILED((hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)dest_folder))))
        return hr;

    add_after = &op->entry;
    if (FAILED((hr = copy_engine_merge_dir(operation, op, item_path, *dest_folder, &add_after))))
    {
        IShellItem_Release(*dest_folder);
        *dest_folder = NULL;
        return hr;
    }
    add_operation(operation, COPY_ENGINE_REMOVE_DIRECTORY_SILENT, src_item, NULL, NULL, NULL, 0, add_after);

    return COPYENGINE_S_NOT_HANDLED;
}

static HRESULT perform_file_operations(struct file_operation *operation)
{
    struct copy_engine_operation *op;
    HRESULT hr;

    file_operation_notify(operation, NULL, FALSE, NULL, notify_start_operations);
    set_file_operation_progress(operation, 0, 0);
    set_file_operation_progress(operation, list_count(&operation->ops), 0);
    file_operation_notify(operation, NULL, FALSE, NULL, notify_reset_timer);

    LIST_FOR_EACH_ENTRY(op, &operation->ops, struct copy_engine_operation, entry)
    {
        hr = E_FAIL;
        switch (op->opcode)
        {
            case COPY_ENGINE_REMOVE_DIRECTORY_SILENT:
            {
                WCHAR *path;
                if (FAILED(SHGetNameFromIDList(op->item_pidl, SIGDN_FILESYSPATH, &path)))
                {
                    ERR("SHGetNameFromIDList failed.\n");
                    break;
                }
                if (!RemoveDirectoryW(path))
                    WARN("Remove directory failed, err %lu.\n", GetLastError());
                CoTaskMemFree(path);
                break;
            }

            case COPY_ENGINE_MOVE:
            {
                struct notify_move_item_param p;

                p.new_item = NULL;
                if (FAILED(hr = SHCreateItemFromIDList(op->item_pidl, &IID_IShellItem, (void**)&p.item)))
                    break;
                if (FAILED((hr = file_operation_notify(operation, op, FALSE, &p, notify_pre_move_item))))
                {
                    IShellItem_Release(p.item);
                    return hr;
                }
                hr = copy_engine_move(operation, op, p.item, &p.new_item);
                p.result = hr;
                if (FAILED(hr = file_operation_notify(operation, op, TRUE, &p, notify_post_move_item)))
                {
                    if (p.new_item)
                        IShellItem_Release(p.new_item);
                    return hr;
                }
                hr = p.result;
                IShellItem_Release(p.item);
                if (p.new_item)
                    IShellItem_Release(p.new_item);
                break;
            }
        }
        set_file_operation_progress(operation, list_count(&operation->ops), operation->progress_sofar + 1);
        TRACE("op %d, hr %#lx.\n", op->opcode, hr);
        operation->aborted = FAILED(hr) || operation->aborted;
    }
    file_operation_notify(operation, NULL, FALSE, NULL, notify_finish_operations);
    return S_OK;
}

static inline struct file_operation *impl_from_IFileOperation(IFileOperation *iface)
{
    return CONTAINING_RECORD(iface, struct file_operation, IFileOperation_iface);
}

static HRESULT WINAPI file_operation_QueryInterface(IFileOperation *iface, REFIID riid, void **out)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);

    TRACE("(%p, %s, %p).\n", iface, debugstr_guid(riid), out);

    if (IsEqualIID(&IID_IFileOperation, riid) ||
        IsEqualIID(&IID_IUnknown, riid))
        *out = &operation->IFileOperation_iface;
    else
    {
        FIXME("not implemented for %s.\n", debugstr_guid(riid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI file_operation_AddRef(IFileOperation *iface)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);
    ULONG ref = InterlockedIncrement(&operation->ref);

    TRACE("(%p): ref=%lu.\n", iface, ref);

    return ref;
}

static ULONG WINAPI file_operation_Release(IFileOperation *iface)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);
    ULONG ref = InterlockedDecrement(&operation->ref);

    TRACE("(%p): ref=%lu.\n", iface, ref);

    if (!ref)
    {
        struct file_operation_sink *sink, *next_sink;

        LIST_FOR_EACH_ENTRY_SAFE(sink, next_sink, &operation->sinks, struct file_operation_sink, entry)
        {
            IFileOperationProgressSink_Release(sink->sink);
            list_remove(&sink->entry);
            free(sink);
        }
        free_file_operation_ops(operation);
        free(operation);
    }

    return ref;
}

static HRESULT WINAPI file_operation_Advise(IFileOperation *iface, IFileOperationProgressSink *sink, DWORD *cookie)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);
    struct file_operation_sink *op_sink;

    TRACE("(%p, %p, %p).\n", iface, sink, cookie);

    if (!sink)
        return E_INVALIDARG;
    if (!(op_sink = calloc(1, sizeof(*op_sink))))
        return E_OUTOFMEMORY;

    op_sink->cookie = ++operation->next_cookie;
    IFileOperationProgressSink_AddRef(sink);
    op_sink->sink = sink;
    list_add_tail(&operation->sinks, &op_sink->entry);
    return S_OK;
}

static HRESULT WINAPI file_operation_Unadvise(IFileOperation *iface, DWORD cookie)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);
    struct file_operation_sink *sink;

    TRACE("(%p, %lx).\n", iface, cookie);
    LIST_FOR_EACH_ENTRY(sink, &operation->sinks, struct file_operation_sink, entry)
    {
        if (sink->cookie == cookie)
        {
            IFileOperationProgressSink_Release(sink->sink);
            list_remove(&sink->entry);
            free(sink);
            return S_OK;
        }
    }
    return S_OK;
}

static HRESULT WINAPI file_operation_SetOperationFlags(IFileOperation *iface, DWORD flags)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);

    TRACE("(%p, %lx).\n", iface, flags);

    operation->flags = flags;
    return S_OK;
}

static HRESULT WINAPI file_operation_SetProgressMessage(IFileOperation *iface, LPCWSTR message)
{
    FIXME("(%p, %s): stub.\n", iface, debugstr_w(message));

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_SetProgressDialog(IFileOperation *iface, IOperationsProgressDialog *dialog)
{
    FIXME("(%p, %p): stub.\n", iface, dialog);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_SetProperties(IFileOperation *iface, IPropertyChangeArray *array)
{
    FIXME("(%p, %p): stub.\n", iface, array);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_SetOwnerWindow(IFileOperation *iface, HWND owner)
{
    FIXME("(%p, %p): stub.\n", iface, owner);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_ApplyPropertiesToItem(IFileOperation *iface, IShellItem *item)
{
    FIXME("(%p, %p): stub.\n", iface, item);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_ApplyPropertiesToItems(IFileOperation *iface, IUnknown *items)
{
    FIXME("(%p, %p): stub.\n", iface, items);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_RenameItem(IFileOperation *iface, IShellItem *item, LPCWSTR name,
        IFileOperationProgressSink *sink)
{
    FIXME("(%p, %p, %s, %p): stub.\n", iface, item, debugstr_w(name), sink);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_RenameItems(IFileOperation *iface, IUnknown *items, LPCWSTR name)
{
    FIXME("(%p, %p, %s): stub.\n", iface, items, debugstr_w(name));

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_MoveItem(IFileOperation *iface, IShellItem *item, IShellItem *folder,
        LPCWSTR name, IFileOperationProgressSink *sink)
{
    TRACE("(%p, %p, %p, %s, %p).\n", iface, item, folder, debugstr_w(name), sink);

    if (!folder || !item)
        return E_INVALIDARG;

    return add_operation(impl_from_IFileOperation(iface), COPY_ENGINE_MOVE, item, folder, name, sink,
            TSF_COPY_LOCALIZED_NAME | TSF_COPY_WRITE_TIME | TSF_COPY_CREATION_TIME | TSF_OVERWRITE_EXIST, NULL);
}

static HRESULT WINAPI file_operation_MoveItems(IFileOperation *iface, IUnknown *items, IShellItem *folder)
{
    FIXME("(%p, %p, %p): stub.\n", iface, items, folder);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_CopyItem(IFileOperation *iface, IShellItem *item, IShellItem *folder,
        LPCWSTR name, IFileOperationProgressSink *sink)
{
    FIXME("(%p, %p, %p, %s, %p): stub.\n", iface, item, folder, debugstr_w(name), sink);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_CopyItems(IFileOperation *iface, IUnknown *items, IShellItem *folder)
{
    FIXME("(%p, %p, %p): stub.\n", iface, items, folder);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_DeleteItem(IFileOperation *iface, IShellItem *item,
        IFileOperationProgressSink *sink)
{
    FIXME("(%p, %p, %p): stub.\n", iface, item, sink);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_DeleteItems(IFileOperation *iface, IUnknown *items)
{
    FIXME("(%p, %p): stub.\n", iface, items);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_NewItem(IFileOperation *iface, IShellItem *folder, DWORD attributes,
        LPCWSTR name, LPCWSTR template, IFileOperationProgressSink *sink)
{
    FIXME("(%p, %p, %lx, %s, %s, %p): stub.\n", iface, folder, attributes,
          debugstr_w(name), debugstr_w(template), sink);

    return E_NOTIMPL;
}

static HRESULT WINAPI file_operation_PerformOperations(IFileOperation *iface)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);
    HRESULT hr;

    TRACE("(%p).\n", iface);

    if (list_empty(&operation->ops))
        return E_UNEXPECTED;

    if (operation->flags != FOF_NO_UI)
        FIXME("Unhandled flags %#lx.\n", operation->flags);
    hr = perform_file_operations(operation);
    free_file_operation_ops(operation);
    return hr;
}

static HRESULT WINAPI file_operation_GetAnyOperationsAborted(IFileOperation *iface, BOOL *aborted)
{
    struct file_operation *operation = impl_from_IFileOperation(iface);

    TRACE("(%p, %p).\n", iface, aborted);

    if (!aborted)
        return E_POINTER;
    *aborted = operation->aborted;
    TRACE("-> aborted %d.\n", *aborted);
    return S_OK;
}

static const IFileOperationVtbl file_operation_vtbl =
{
    file_operation_QueryInterface,
    file_operation_AddRef,
    file_operation_Release,
    file_operation_Advise,
    file_operation_Unadvise,
    file_operation_SetOperationFlags,
    file_operation_SetProgressMessage,
    file_operation_SetProgressDialog,
    file_operation_SetProperties,
    file_operation_SetOwnerWindow,
    file_operation_ApplyPropertiesToItem,
    file_operation_ApplyPropertiesToItems,
    file_operation_RenameItem,
    file_operation_RenameItems,
    file_operation_MoveItem,
    file_operation_MoveItems,
    file_operation_CopyItem,
    file_operation_CopyItems,
    file_operation_DeleteItem,
    file_operation_DeleteItems,
    file_operation_NewItem,
    file_operation_PerformOperations,
    file_operation_GetAnyOperationsAborted
};

HRESULT WINAPI IFileOperation_Constructor(IUnknown *outer, REFIID riid, void **out)
{
    struct file_operation *object;
    HRESULT hr;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IFileOperation_iface.lpVtbl = &file_operation_vtbl;
    list_init(&object->sinks);
    list_init(&object->ops);
    object->ref = 1;

    hr = IFileOperation_QueryInterface(&object->IFileOperation_iface, riid, out);
    IFileOperation_Release(&object->IFileOperation_iface);

    return hr;
}
