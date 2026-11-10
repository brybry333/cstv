// cstv_os.h — what differs between Linux and Windows in cstvd: starting child programs, the control
// socket, the wakeup channel, shared memory, paths, logging. On Linux everything behaves exactly as
// before the Windows port (fork/exec, CLOEXEC pipes, unix socket, files in /tmp/cstv).
//
// Windows (MinGW-w64): child programs get only the handles we list (PROC_THREAD_ATTRIBUTE_HANDLE_LIST),
// run without a console window and sit in a job object, so killing one takes its whole tree with it.
// The game talks to us over 127.0.0.1:CSTV_TCP_PORT; frames/chat/card are named file mappings.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define SOCK_SEND_FLAGS 0
#define poll WSAPoll
#ifndef strtok_r
#define strtok_r strtok_s
#endif
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#define SOCK_SEND_FLAGS (MSG_NOSIGNAL | MSG_DONTWAIT)
#endif

// ---------------------------------------------------------------- paths
static char os_exe_dir[1024];   // where cstvd lives (UTF-8)
static char os_run_dir[1024];   // runtime files: /tmp/cstv, or %TEMP%\cstv

#ifdef _WIN32
static int os_utf8(const wchar_t *w, char *out, int n)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL) > 0;
}
static wchar_t *os_wide(const char *s)   // malloc'd UTF-16 copy
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc(n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

static void os_init_paths(void)
{
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n > 0 && n < 1024 && os_utf8(w, os_exe_dir, sizeof os_exe_dir)) {
        char *sl = strrchr(os_exe_dir, '\\');
        if (sl) *sl = 0;
    }
    wchar_t t[MAX_PATH + 1];
    char tmp[1024] = ".";
    if (GetTempPathW(MAX_PATH + 1, t)) os_utf8(t, tmp, sizeof tmp);
    size_t l = strlen(tmp);
    while (l && (tmp[l - 1] == '\\' || tmp[l - 1] == '/')) tmp[--l] = 0;
    snprintf(os_run_dir, sizeof os_run_dir, "%s\\cstv", tmp);
    wchar_t *wd = os_wide(os_run_dir);
    if (wd) { CreateDirectoryW(wd, NULL); free(wd); }
#else
    ssize_t el = readlink("/proc/self/exe", os_exe_dir, sizeof os_exe_dir - 1);
    if (el > 0) { os_exe_dir[el] = 0; char *sl = strrchr(os_exe_dir, '/'); if (sl) *sl = 0; }
    snprintf(os_run_dir, sizeof os_run_dir, "%s", CSTV_DIR);
    mkdir(os_run_dir, 0700);
#endif
}

// a file next to cstvd ("fonts/NotoSans-Bold.ttf" style, / is fine on both)
static void os_exe_path(char *out, size_t n, const char *rel)
{
#ifdef _WIN32
    snprintf(out, n, "%s\\%s", os_exe_dir, rel);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
#else
    snprintf(out, n, "%s/%s", os_exe_dir, rel);
#endif
}

static FILE *os_fopen(const char *path, const char *mode)   // UTF-8 path on both
{
#ifdef _WIN32
    wchar_t *wp = os_wide(path), *wm = os_wide(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static void os_mkdir(const char *path)   // one level; fine if it exists
{
#ifdef _WIN32
    wchar_t *wp = os_wide(path);
    if (wp) { CreateDirectoryW(wp, NULL); free(wp); }
#else
    mkdir(path, 0700);
#endif
}

static int os_file_exists(const char *path)
{
#ifdef _WIN32
    wchar_t *wp = os_wide(path);
    DWORD a = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
    free(wp);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path, X_OK) == 0;
#endif
}

// ---------------------------------------------------------------- logging
// Linux: stderr (the journal). Windows: %TEMP%\cstv\cstvd.log (cstvd has no console there); child
// programs write their stderr into the same file.
#ifdef _WIN32
static HANDLE os_log_handle = INVALID_HANDLE_VALUE;
static void os_log_open(void)
{
    char p[1100];
    snprintf(p, sizeof p, "%s\\cstvd.log", os_run_dir);
    wchar_t *wp = os_wide(p);
    if (!wp) return;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};   // inheritable: children log here too
    os_log_handle = CreateFileW(wp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp);
}
#endif

static void os_log_line(const char *line)
{
#ifdef _WIN32
    if (os_log_handle != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(os_log_handle, line, (DWORD)strlen(line), &w, NULL);
        return;
    }
#endif
    fputs(line, stderr);
}

// ---------------------------------------------------------------- child programs
typedef struct {
#ifdef _WIN32
    HANDLE proc, job;
#else
    pid_t pid;
#endif
} os_proc;

static int os_proc_valid(const os_proc *p)
{
#ifdef _WIN32
    return p->proc != NULL;
#else
    return p->pid > 0;
#endif
}

enum { OS_OUT_INHERIT, OS_OUT_NULL, OS_OUT_PIPE };

#ifdef _WIN32
// one argument the way CommandLineToArgvW / the MSVC CRT reads it back
static void os_quote_arg(char *out, size_t n, size_t *o, const char *a)
{
#define PUT(c) do { if (*o + 1 < n) out[(*o)++] = (c); } while (0)
    int needs = !*a || strpbrk(a, " \t\"") != NULL;
    if (!needs) { for (; *a; a++) PUT(*a); return; }
    PUT('"');
    for (;; a++) {
        size_t bs = 0;
        while (*a == '\\') { a++; bs++; }
        if (!*a) { for (size_t i = 0; i < bs * 2; i++) PUT('\\'); break; }
        if (*a == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) PUT('\\'); PUT('"'); }
        else { for (size_t i = 0; i < bs; i++) PUT('\\'); PUT(*a); }
    }
    PUT('"');
#undef PUT
}
#endif

// Start argv[0] (Linux: looked up in PATH; Windows: a full path) with arguments argv (NULL-ended).
// in_fd: if given, the child's stdin is a pipe and *in_fd gets its write end; otherwise stdin is the null
// device. out: OS_OUT_PIPE puts the read end of the child's stdout in *out_fd; OS_OUT_NULL / OS_OUT_INHERIT.
// stderr is ours (the log). Our pipe ends never reach other children. Returns 0 on success.
static int os_spawn(const char *const argv[], int *in_fd, int out, int *out_fd, os_proc *p)
{
#ifdef _WIN32
    memset(p, 0, sizeof *p);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, nul_in = NULL, nul_out = NULL;
    int ok = 0;
    if (in_fd) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) goto done;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    }
    if (out == OS_OUT_PIPE) {
        if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) goto done;
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    } else if (out == OS_OUT_NULL) {
        nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    }
    HANDLE h_in = in_fd ? in_r : nul_in;
    HANDLE h_out = out == OS_OUT_PIPE ? out_w : out == OS_OUT_NULL ? nul_out : os_log_handle;
    HANDLE h_err = os_log_handle;
    if (!h_in || h_in == INVALID_HANDLE_VALUE) goto done;

    char cmd[8192];
    size_t o = 0;
    for (int i = 0; argv[i]; i++) {
        if (i && o + 1 < sizeof cmd) cmd[o++] = ' ';
        os_quote_arg(cmd, sizeof cmd, &o, argv[i]);
    }
    cmd[o] = 0;

    // inherit exactly these handles (each once)
    HANDLE list[3];
    int nl = 0;
    HANDLE cand[3] = {h_in, h_out, h_err};
    for (int i = 0; i < 3; i++) {
        if (!cand[i] || cand[i] == INVALID_HANDLE_VALUE) continue;
        int dup = 0;
        for (int k = 0; k < nl; k++) dup |= list[k] == cand[i];
        if (!dup) list[nl++] = cand[i];
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = malloc(asz);
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &asz)) { free(al); goto done; }
    UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nl * sizeof(HANDLE), NULL, NULL);
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = h_in;
    si.StartupInfo.hStdOutput = h_out && h_out != INVALID_HANDLE_VALUE ? h_out : NULL;
    si.StartupInfo.hStdError = h_err != INVALID_HANDLE_VALUE ? h_err : NULL;
    si.lpAttributeList = al;
    wchar_t *wcmd = os_wide(cmd), *wapp = os_wide(argv[0]);
    PROCESS_INFORMATION pi;
    BOOL made = wcmd && wapp && CreateProcessW(wapp, wcmd, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    free(wcmd);
    free(wapp);
    DeleteProcThreadAttributeList(al);
    free(al);
    if (!made) goto done;
    // a job per child: killing it kills whatever it started too (streamlink's own python, ...)
    p->job = CreateJobObjectW(NULL, NULL);
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li);
        if (!AssignProcessToJobObject(p->job, pi.hProcess)) { CloseHandle(p->job); p->job = NULL; }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->proc = pi.hProcess;
    if (in_fd) { *in_fd = _open_osfhandle((intptr_t)in_w, _O_WRONLY | _O_BINARY); in_w = NULL; }
    if (out == OS_OUT_PIPE) { *out_fd = _open_osfhandle((intptr_t)out_r, _O_RDONLY | _O_BINARY); out_r = NULL; }
    ok = 1;
done:
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    if (nul_in && nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (nul_out && nul_out != INVALID_HANDLE_VALUE) CloseHandle(nul_out);
    return ok ? 0 : -1;
#else
    int ip[2] = {-1, -1}, op[2] = {-1, -1};
    p->pid = 0;
    // O_CLOEXEC: other children (chat renderer, yt-dlp lookups) must not inherit these, or e.g.
    // streamlink never sees its pipe close and can't exit when we stop
    if (in_fd && pipe2(ip, O_CLOEXEC)) return -1;
    if (out == OS_OUT_PIPE && pipe2(op, O_CLOEXEC)) { if (in_fd) { close(ip[0]); close(ip[1]); } return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (in_fd) dup2(ip[0], 0);
        else if (dn >= 0) dup2(dn, 0);
        if (out == OS_OUT_PIPE) dup2(op[1], 1);
        else if (out == OS_OUT_NULL && dn >= 0) dup2(dn, 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (in_fd) close(ip[0]);
    if (out == OS_OUT_PIPE) close(op[1]);
    if (pid < 0) {
        if (in_fd) close(ip[1]);
        if (out == OS_OUT_PIPE) close(op[0]);
        return -1;
    }
    if (in_fd) *in_fd = ip[1];
    if (out == OS_OUT_PIPE) *out_fd = op[0];
    p->pid = pid;
    return 0;
#endif
}

static void os_proc_forget(os_proc *p)
{
#ifdef _WIN32
    if (p->proc) CloseHandle(p->proc);
    if (p->job) CloseHandle(p->job);   // KILL_ON_JOB_CLOSE: anything still running in it ends
    p->proc = p->job = NULL;
#else
    p->pid = 0;
#endif
}

// still running? (reaps it when it has ended)
static int os_proc_running(os_proc *p)
{
    if (!os_proc_valid(p)) return 0;
#ifdef _WIN32
    if (WaitForSingleObject(p->proc, 0) == WAIT_TIMEOUT) return 1;
#else
    if (waitpid(p->pid, NULL, WNOHANG) == 0) return 1;
#endif
    os_proc_forget(p);
    return 0;
}

// waits for it to end; its exit status (-1 if it didn't exit normally)
static int os_proc_wait(os_proc *p)
{
    if (!os_proc_valid(p)) return -1;
    int rc = -1;
#ifdef _WIN32
    DWORD code;
    WaitForSingleObject(p->proc, INFINITE);
    if (GetExitCodeProcess(p->proc, &code)) rc = (int)code;
#else
    int st;
    if (waitpid(p->pid, &st, 0) == p->pid && WIFEXITED(st)) rc = WEXITSTATUS(st);
#endif
    os_proc_forget(p);
    return rc;
}

// Ask it to stop, give it grace_ms, then kill it. Never waits longer than that: an unbounded wait on
// streamlink once froze the whole helper. Windows has no polite stop for windowless programs, so it
// (and everything it started) ends at once.
static void os_proc_kill(os_proc *p, int grace_ms)
{
    if (!os_proc_valid(p)) return;
#ifdef _WIN32
    (void)grace_ms;
    if (p->job) TerminateJobObject(p->job, 1);
    else TerminateProcess(p->proc, 1);
    WaitForSingleObject(p->proc, 2000);
#else
    kill(p->pid, SIGTERM);
    for (int i = 0; i < grace_ms / 20; i++) {
        if (waitpid(p->pid, NULL, WNOHANG) == p->pid) { os_proc_forget(p); return; }
        usleep(20000);
    }
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
#endif
    os_proc_forget(p);
}

// ---------------------------------------------------------------- sockets
static void os_net_init(void)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
#endif
}

#ifdef _WIN32
static void os_sock_setup(sock_t s)   // non-blocking sends (Linux uses MSG_DONTWAIT), not inherited
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
}
#endif

// The listening control socket. Linux: the unix socket systemd handed over (cstvd.socket), or our own
// at CSTV_SOCK_PATH. Windows: 127.0.0.1:CSTV_TCP_PORT.
static sock_t os_ctl_listen(void (*log)(const char *, ...))
{
#ifdef _WIN32
    SOCKET ls = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (ls == INVALID_SOCKET) return SOCK_BAD;
    BOOL excl = TRUE;   // nobody else may sit on the same port
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(CSTV_TCP_PORT);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 2)) {
        log("socket 127.0.0.1:%d: error %d (another cstvd running?)", CSTV_TCP_PORT, WSAGetLastError());
        closesocket(ls);
        return SOCK_BAD;
    }
    return ls;
#else
    // Socket activation (cstvd.socket): systemd holds the listening socket and starts us when the game
    // connects; it hands the socket over as fd 3 (LISTEN_FDS / LISTEN_PID). Otherwise we make our own.
    const char *lpid = getenv("LISTEN_PID"), *lfds = getenv("LISTEN_FDS");
    if (lpid && lfds && atol(lpid) == (long)getpid() && atoi(lfds) >= 1) {
        fcntl(3, F_SETFD, FD_CLOEXEC);
        unsetenv("LISTEN_PID");
        unsetenv("LISTEN_FDS");
        unsetenv("LISTEN_FDNAMES");
        log("started by the game connecting (socket activation)");
        return 3;
    }
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", CSTV_SOCK_PATH);
    unlink(CSTV_SOCK_PATH);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 2)) { log("socket: %s", strerror(errno)); return SOCK_BAD; }
    chmod(CSTV_SOCK_PATH, 0600);
    return ls;
#endif
}

static sock_t os_accept(sock_t ls)
{
#ifdef _WIN32
    SOCKET c = accept(ls, NULL, NULL);
    if (c != INVALID_SOCKET) os_sock_setup(c);
    return c;
#else
    return accept4(ls, NULL, NULL, SOCK_CLOEXEC);
#endif
}

// ---------------------------------------------------------------- wakeup channel (mpv's thread -> main loop)
// Linux: a pipe. Windows: WSAPoll only takes sockets, so a UDP socket that sends to itself.
static sock_t os_wake_rd = SOCK_BAD;
#ifndef _WIN32
static int os_wake_wr = -1;
#endif

static int os_wake_init(void)
{
#ifdef _WIN32
    SOCKET u = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (u == INVALID_SOCKET) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int al = sizeof a;
    if (bind(u, (struct sockaddr *)&a, sizeof a) || getsockname(u, (struct sockaddr *)&a, &al) ||
        connect(u, (struct sockaddr *)&a, sizeof a)) { closesocket(u); return -1; }
    u_long nb = 1;
    ioctlsocket(u, FIONBIO, &nb);
    os_wake_rd = u;
    return 0;
#else
    int p[2];
    if (pipe2(p, O_NONBLOCK | O_CLOEXEC)) return -1;
    os_wake_rd = p[0];
    os_wake_wr = p[1];
    return 0;
#endif
}

static void os_wake(void)
{
    char c = 1;
#ifdef _WIN32
    send(os_wake_rd, &c, 1, 0);
#else
    if (write(os_wake_wr, &c, 1) < 0) {}
#endif
}

static void os_wake_drain(void)
{
    char tmp[64];
#ifdef _WIN32
    while (recv(os_wake_rd, tmp, sizeof tmp, 0) > 0) {}
#else
    while (read(os_wake_rd, tmp, sizeof tmp) > 0) {}
#endif
}

// ---------------------------------------------------------------- shared memory
// The frames buffer: created (or reused) by us. Linux: a file in /tmp/cstv (one of another size belongs
// to an older build: unlinked, so a game still on that build keeps its copy instead of misreading ours).
// Windows: a named mapping whose name carries the layout version.
static void *os_shm_create(const char *path_or_name, size_t size)
{
#ifdef _WIN32
    wchar_t *wn = os_wide(path_or_name);
    HANDLE m = wn ? CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, wn) : NULL;
    free(wn);
    if (!m) return NULL;
    void *v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    // The handle stays open as long as we run: a named object loses its NAME when its last handle closes,
    // even while a view keeps the memory alive -- the game could then no longer open it (found 2026-10-17:
    // audio played, the screen kept its placeholder).
    if (!v) CloseHandle(m);
    return v;
#else
    struct stat st;
    if (stat(path_or_name, &st) == 0 && st.st_size != (off_t)size) unlink(path_or_name);
    int fd = open(path_or_name, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size)) { close(fd); return NULL; }
    void *v = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return v == MAP_FAILED ? NULL : v;
#endif
}

// Someone else's buffer (the overlay's info card), read-only; NULL until it exists with at least `size`.
static const void *os_shm_open_ro(const char *path_or_name, size_t size)
{
#ifdef _WIN32
    wchar_t *wn = os_wide(path_or_name);
    HANDLE m = wn ? OpenFileMappingW(FILE_MAP_READ, FALSE, wn) : NULL;
    free(wn);
    if (!m) return NULL;
    void *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, size);
    CloseHandle(m);
    return v;
#else
    int fd = open(path_or_name, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    void *v = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)size) v = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return v == MAP_FAILED ? NULL : v;
#endif
}

static void os_shm_close(const void *v, size_t size)
{
#ifdef _WIN32
    (void)size;
    UnmapViewOfFile(v);
#else
    munmap((void *)v, size);
#endif
}
