/*
 * wsl.exe, bash.exe and git.exe: Linux in Arctic, as WSL gives it on Windows
 *
 * One program under three names. It asks the host's arctic-lxss
 * (host/init/arctic-lxss.c) to run a command in a WSL distribution and
 * passes stdin, stdout, stderr and the exit code through, the way WSL's
 * wsl.exe does over its own socket:
 *
 *   wsl.exe   wsl [-d name] [-u user] [--cd dir] [-e cmd args | -- cmd | cmd],
 *             wsl -l, wsl --shutdown, wsl -t name
 *   bash.exe  bash with these arguments in the default distribution, the
 *             drives as Git Bash names them (/c, /d): Claude Code's
 *             CLAUDE_CODE_GIT_BASH_PATH
 *   git.exe   git there, with C:\x arguments as /c/x
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* the protocol, shared with host/init/arctic-lxss.c */
#define MAGIC "ARCTLXS1"
#define TOKEN_LEN 32
enum { F_STDIN, F_STDIN_EOF, F_STDOUT, F_STDERR, F_EXIT, F_ERROR, F_SIGNAL };
#define REQ_SHUTDOWN 1
#define REQ_GITBASH 2
#define REQ_TERMINATE 4

static SOCKET sock = INVALID_SOCKET;
static CRITICAL_SECTION send_lock;

struct buffer {
    char *data;
    size_t len, size;
};

static void put(struct buffer *b, const void *data, size_t len)
{
    if (b->len + len > b->size) {
        b->size = (b->len + len) * 2 + 256;
        b->data = realloc(b->data, b->size);
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void put32(struct buffer *b, unsigned int v)
{
    unsigned char p[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
    put(b, p, 4);
}

static char *utf8(const WCHAR *s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = malloc(len > 0 ? len : 1);

    if (len <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, out, len, NULL, NULL))
        out[0] = 0;
    return out;
}

static void put_string(struct buffer *b, const char *s)
{
    put32(b, (unsigned int)strlen(s));
    put(b, s, strlen(s));
}

static void message(const char *fmt, ...)
{
    char text[1024];
    DWORD written;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)strlen(text), &written, NULL);
}

static int send_all(const void *data, int len)
{
    const char *p = data;

    while (len > 0) {
        int n = send(sock, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all(void *data, int len)
{
    char *p = data;

    while (len > 0) {
        int n = recv(sock, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_frame(int type, const void *data, unsigned int len)
{
    unsigned char head[5] = {type, len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, len >> 24};
    int rc;

    EnterCriticalSection(&send_lock);
    rc = send_all(head, 5) || (len && send_all(data, (int)len)) ? -1 : 0;
    LeaveCriticalSection(&send_lock);
    return rc;
}

static HANDLE reader;
static volatile LONG exiting;

/* A console is read only once a key with a character is down: a read left
 * waiting when the program ends would take the next lines typed into the
 * console after it has gone */
static int console_has_text(HANDLE in)
{
    INPUT_RECORD rec;
    DWORD n;

    for (;;) {
        if (exiting || WaitForSingleObject(in, 200) != WAIT_OBJECT_0)
            return 0;
        if (!PeekConsoleInputW(in, &rec, 1, &n) || !n)
            continue;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.uChar.UnicodeChar)
            return 1;
        ReadConsoleInputW(in, &rec, 1, &n); /* key up, focus, mouse: nothing to pass */
    }
}

static DWORD WINAPI stdin_thread(void *arg)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char buf[16384];
    DWORD n, mode;
    BOOL console = GetConsoleMode(in, &mode);

    while (in && in != INVALID_HANDLE_VALUE && !exiting) {
        if (console && !console_has_text(in))
            continue;
        if (!ReadFile(in, buf, sizeof(buf), &n, NULL) || !n)
            break;
        if (send_frame(F_STDIN, buf, n))
            return 0;
    }
    send_frame(F_STDIN_EOF, NULL, 0);
    return 0;
}

static void finish(int code)
{
    InterlockedExchange(&exiting, 1);
    if (reader)
        CancelSynchronousIo(reader);
    ExitProcess(code);
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    unsigned char sig[4] = {type == CTRL_BREAK_EVENT ? 3 : 2, 0, 0, 0}; /* SIGQUIT, SIGINT */

    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    send_frame(F_SIGNAL, sig, 4);
    return TRUE;
}

/* C:\ProgramData\Arctic\Lxss\<name> */
static FILE *open_lxss_file(const WCHAR *name)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH];

    if (!GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH))
        wcscpy(dir, L"C:\\ProgramData");
    swprintf(path, MAX_PATH, L"%s\\Arctic\\Lxss\\%s", dir, name);
    return _wfopen(path, L"rb");
}

static int connect_service(char *token)
{
    struct sockaddr_in addr = {0};
    unsigned int port = 0;
    FILE *f = open_lxss_file(L"session");
    WSADATA wsa;

    if (!f || fscanf(f, "%u %32s", &port, token) != 2) {
        message("The Windows Subsystem for Linux is not running (no C:\\ProgramData\\Arctic\\Lxss\\session).\n");
        if (f)
            fclose(f);
        return -1;
    }
    fclose(f);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock == INVALID_SOCKET || connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        message("The Windows Subsystem for Linux is not running (127.0.0.1:%u: error %d).\n", port,
                WSAGetLastError());
        return -1;
    }
    return 0;
}

/* Windows' own variables mean nothing to Linux; the rest goes along */
static int pass_variable(const WCHAR *entry)
{
    static const WCHAR *const windows_only[] = {
        L"PATH", L"PATHEXT", L"COMSPEC", L"SYSTEMROOT", L"SYSTEMDRIVE", L"WINDIR", L"TEMP", L"TMP", L"HOME",
        L"HOMEDRIVE", L"HOMEPATH", L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA", L"PROGRAMDATA", L"PROGRAMFILES",
        L"PROGRAMW6432", L"COMMONPROGRAMFILES", L"COMMONPROGRAMW6432", L"ALLUSERSPROFILE", L"PUBLIC", L"OS",
        L"NUMBER_OF_PROCESSORS", L"USERNAME", L"USER", L"LOGNAME", L"USERDOMAIN", L"COMPUTERNAME", L"LOGONSERVER",
        L"SESSIONNAME", L"PSMODULEPATH", L"SHELL", L"PWD", L"OLDPWD", L"SHLVL", L"PROMPT", L"DRIVERDATA",
        L"ONEDRIVE", L"CLAUDE_CODE_GIT_BASH_PATH", NULL};
    const WCHAR *eq = wcschr(entry, L'=');
    WCHAR name[256];
    size_t len;

    if (!eq || eq == entry || (len = eq - entry) >= ARRAY_SIZE(name))
        return 0;
    for (size_t i = 0; i < len; i++) /* a name the shell can hold */
        if (!(iswalnum(entry[i]) || entry[i] == L'_') || entry[i] > 127)
            return 0;
    memcpy(name, entry, len * sizeof(WCHAR));
    name[len] = 0;
    if (!_wcsnicmp(name, L"PROCESSOR_", 10) || !_wcsnicmp(name, L"WINE", 4) || !_wcsnicmp(name, L"=", 1))
        return 0;
    for (int i = 0; windows_only[i]; i++)
        if (!_wcsicmp(name, windows_only[i]))
            return 0;
    return 1;
}

/* C:\x or C:/x as Git Bash writes it: /c/x */
static char *git_bash_path(const WCHAR *arg)
{
    char *s = utf8(arg);

    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' &&
        (s[2] == '\\' || s[2] == '/' || !s[2])) {
        char *out = malloc(strlen(s) + 2);
        sprintf(out, "/%c%s", s[0] | 0x20, s + 2);
        for (char *p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
        free(s);
        return out;
    }
    return s;
}

static void usage(void)
{
    message("Usage: wsl.exe [Argument] [Options...] [CommandLine]\n\n"
            "    -d, --distribution <name>   run in that distribution\n"
            "    -u, --user <name>           run as that user\n"
            "    --cd <dir>                  start in that folder (Windows or Linux path, ~ for home)\n"
            "    -e, --exec <cmd> [args]     run the command without the shell\n"
            "    -- <command line>           run the rest through the shell\n"
            "    -l, --list                  list the distributions\n"
            "    -t, --terminate <name>      stop a distribution\n"
            "    --shutdown                  stop them all\n");
}

static int list_distros(void)
{
    char line[1024];
    FILE *f = open_lxss_file(L"distros");
    int first = 1;

    if (!f) {
        message("No distribution in C:\\ProgramData\\Arctic\\Lxss\\distros.\n");
        return 1;
    }
    printf("Windows Subsystem for Linux Distributions:\n");
    while (fgets(line, sizeof(line), f)) {
        char *gap = line + strcspn(line, " \t\r\n");
        if (!*gap || line[0] == '#')
            continue;
        *gap = 0;
        printf("%s%s\n", line, first ? " (Default)" : "");
        first = 0;
    }
    fclose(f);
    return 0;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    WCHAR self[MAX_PATH], *base, cwd_w[MAX_PATH * 2];
    char token[TOKEN_LEN + 1] = "", *distro = "", *user = "", *cwd;
    char **args = calloc(argc + 4, sizeof(char *));
    unsigned int flags = 0, nargs = 0;
    struct buffer req = {0};
    int i = 1;

    GetModuleFileNameW(NULL, self, MAX_PATH);
    base = wcsrchr(self, L'\\') ? wcsrchr(self, L'\\') + 1 : self;
    GetCurrentDirectoryW(ARRAY_SIZE(cwd_w), cwd_w);
    cwd = utf8(cwd_w);

    if (!_wcsicmp(base, L"bash.exe") || !_wcsicmp(base, L"sh.exe")) {
        flags |= REQ_GITBASH;
        args[nargs++] = "bash";
        for (; i < argc; i++)
            args[nargs++] = utf8(argv[i]);
    } else if (!_wcsicmp(base, L"git.exe")) {
        flags |= REQ_GITBASH;
        args[nargs++] = "git";
        for (; i < argc; i++)
            args[nargs++] = git_bash_path(argv[i]);
    } else {
        int exec = 0;
        for (; i < argc; i++) {
            const WCHAR *a = argv[i];
            if ((!wcscmp(a, L"-d") || !wcscmp(a, L"--distribution")) && i + 1 < argc)
                distro = utf8(argv[++i]);
            else if ((!wcscmp(a, L"-u") || !wcscmp(a, L"--user")) && i + 1 < argc)
                user = utf8(argv[++i]);
            else if (!wcscmp(a, L"--cd") && i + 1 < argc)
                cwd = utf8(argv[++i]);
            else if (!wcscmp(a, L"-l") || !wcscmp(a, L"--list"))
                return list_distros();
            else if (!wcscmp(a, L"-v") || !wcscmp(a, L"--verbose"))
                continue;
            else if (!wcscmp(a, L"--shutdown"))
                flags |= REQ_SHUTDOWN;
            else if ((!wcscmp(a, L"-t") || !wcscmp(a, L"--terminate")) && i + 1 < argc) {
                flags |= REQ_TERMINATE;
                distro = utf8(argv[++i]);
            } else if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help")) {
                usage();
                return 0;
            } else if (!wcscmp(a, L"-e") || !wcscmp(a, L"--exec")) {
                exec = 1;
                i++;
                break;
            } else if (!wcscmp(a, L"--")) {
                i++;
                break;
            } else
                break;
        }
        if (exec)
            for (; i < argc; i++)
                args[nargs++] = utf8(argv[i]);
        else if (i < argc) {
            /* the rest is a command line for the user's shell, as WSL runs it */
            struct buffer line = {0};
            for (; i < argc; i++) {
                char *a = utf8(argv[i]);
                if (line.len)
                    put(&line, " ", 1);
                put(&line, a, strlen(a));
            }
            put(&line, "", 1);
            args[nargs++] = "/bin/sh";
            args[nargs++] = "-c";
            args[nargs++] = line.data;
        }
    }

    if (connect_service(token))
        return 1;
    InitializeCriticalSection(&send_lock);

    WCHAR *block = GetEnvironmentStringsW();
    unsigned int nenv = 0;
    struct buffer env = {0};
    for (WCHAR *e = block; *e; e += wcslen(e) + 1)
        if (pass_variable(e)) {
            put_string(&env, utf8(e));
            nenv++;
        }

    put(&req, MAGIC, 8);
    put(&req, token, TOKEN_LEN);
    put32(&req, flags);
    put32(&req, nargs);
    put32(&req, nenv);
    put_string(&req, distro);
    put_string(&req, user);
    put_string(&req, cwd);
    for (unsigned int k = 0; k < nargs; k++)
        put_string(&req, args[k]);
    if (env.len)
        put(&req, env.data, env.len);
    if (send_all(req.data, (int)req.len)) {
        message("wsl: the request did not go through\n");
        return 1;
    }

    if (!(flags & (REQ_SHUTDOWN | REQ_TERMINATE))) {
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        reader = CreateThread(NULL, 0, stdin_thread, NULL, 0, NULL);
    }

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    static char data[65536 + 1];
    for (;;) {
        unsigned char head[5];
        unsigned int len;
        DWORD written;

        if (recv_all(head, 5))
            break;
        len = head[1] | head[2] << 8 | head[3] << 16 | (unsigned int)head[4] << 24;
        if (len > 65536 || (len && recv_all(data, (int)len)))
            break;
        switch (head[0]) {
        case F_STDOUT:
        case F_STDERR: {
            HANDLE h = head[0] == F_STDOUT ? out : err;
            for (unsigned int off = 0; off < len && WriteFile(h, data + off, len - off, &written, NULL) && written;)
                off += written;
            break;
        }
        case F_ERROR:
            data[len] = 0;
            message("%s\n", data);
            finish(1);
        case F_EXIT:
            finish(len >= 4 ? (data[0] & 0xff) | (data[1] & 0xff) << 8 : 1);
        }
    }
    message("wsl: the connection to the Linux subsystem was lost\n");
    finish(1);
    return 1;
}
