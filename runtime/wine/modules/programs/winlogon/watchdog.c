/*
 * Windows Logon Application: a shell that stopped answering
 *
 * Now and then the session came up with the desktop colour, the pointer and
 * a half drawn taskbar, and stayed so: explorer.exe was alive but did not
 * answer, so AutoRestartShell, which waits for the shell to end, never came,
 * and there was no taskbar to reach anything else from. Here the taskbar
 * and the desktop of the shell are asked every few seconds; when neither has
 * answered for SHELL_HUNG_MS, the stacks of every explorer.exe thread go to
 * the log (so that the cause can be found) and the shell starts again, as
 * the user would do it from Task Manager in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include "wine/debug.h"

#include "winlogon.h"

WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

#define SHELL_HUNG_MS 30000

static const WCHAR winlogon_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

struct find_class
{
    const WCHAR *name;
    HWND         found;
};

/* FindWindow does not see the classes another process registered */
static BOOL CALLBACK find_class_proc( HWND hwnd, LPARAM param )
{
    struct find_class *find = (struct find_class *)param;
    WCHAR name[64];

    if (!GetClassNameW( hwnd, name, ARRAY_SIZE(name) ) || wcscmp( name, find->name )) return TRUE;
    find->found = hwnd;
    return FALSE;
}

static HWND find_toplevel( const WCHAR *class )
{
    struct find_class find = { class, 0 };

    EnumWindows( find_class_proc, (LPARAM)&find );
    return find.found;
}

/* a window that takes a message within a second */
static BOOL answers( HWND hwnd )
{
    DWORD_PTR result;

    return SendMessageTimeoutW( hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &result ) != 0;
}

/* every thread of the process, frame by frame, into the log */
static void log_stacks( DWORD pid )
{
    THREADENTRY32 entry = { .dwSize = sizeof(entry) };
    HANDLE process, snapshot;

    if (!(process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pid ))) return;
    SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME );
    SymInitializeW( process, NULL, TRUE );
    snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    for (BOOL more = Thread32First( snapshot, &entry ); more; more = Thread32Next( snapshot, &entry ))
    {
        STACKFRAME64 frame = { 0 };
        CONTEXT context = { .ContextFlags = CONTEXT_FULL };
        HANDLE thread;
        DWORD machine;

        if (entry.th32OwnerProcessID != pid) continue;
        if (!(thread = OpenThread( THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID ))) continue;
        SuspendThread( thread );
        if (GetThreadContext( thread, &context ))
        {
            ERR( "explorer.exe thread %04lx:\n", entry.th32ThreadID );
#ifdef __x86_64__
            machine = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = context.Rip;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrFrame.Offset = context.Rbp;
#else
            machine = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = context.Eip;
            frame.AddrStack.Offset = context.Esp;
            frame.AddrFrame.Offset = context.Ebp;
#endif
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            for (int i = 0; i < 32 && StackWalk64( machine, process, thread, &frame, &context, NULL,
                                                   SymFunctionTableAccess64, SymGetModuleBase64, NULL ); i++)
            {
                char buffer[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
                IMAGEHLP_MODULE64 module = { .SizeOfStruct = sizeof(module) };
                DWORD64 address = frame.AddrPC.Offset, offset = 0;

                if (!address) break;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                if (!SymGetModuleInfo64( process, address, &module )) module.ModuleName[0] = 0;
                if (SymFromAddr( process, address, &offset, symbol ))
                    ERR( "  %s!%s+%#I64x\n", module.ModuleName, symbol->Name, offset );
                else
                    ERR( "  %s+%#I64x\n", module.ModuleName[0] ? module.ModuleName : "?", address - module.BaseOfImage );
            }
        }
        ResumeThread( thread );
        CloseHandle( thread );
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle( snapshot );
    SymCleanup( process );
    CloseHandle( process );
}

/* the hung shell goes, with its stacks in the log, and another one starts */
static void restart_shell( HWND hung )
{
    WCHAR shell[MAX_PATH] = L"explorer.exe";
    DWORD pid = 0, size = sizeof(shell);
    HANDLE process;

    GetWindowThreadProcessId( hung, &pid );
    ERR( "the shell (process %04lx) has not answered for %u s, starting it again\n", pid, SHELL_HUNG_MS / 1000 );
    log_stacks( pid );
    if (pid && (process = OpenProcess( PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid )))
    {
        TerminateProcess( process, 1 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( process );
    }
    RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size );
    run( shell );
}

/* called with the shell check (SHELL_CHECK_MS); TRUE when it restarted the
 * shell itself */
BOOL check_shell_answers(void)
{
    static DWORD silent_since;
    HWND shell = find_toplevel( L"Shell_TrayWnd" );

    /* the taskbar, or the desktop while there is no taskbar yet */
    if (!shell) shell = GetShellWindow();
    if (!shell || answers( shell ))
    {
        if (silent_since) ERR( "the shell answers again after %lu ms\n", GetTickCount() - silent_since );
        silent_since = 0;
        return FALSE;
    }
    if (!silent_since)
    {
        silent_since = GetTickCount();
        return FALSE;
    }
    if (GetTickCount() - silent_since < SHELL_HUNG_MS) return FALSE;
    silent_since = 0;
    restart_shell( shell );
    return TRUE;
}
