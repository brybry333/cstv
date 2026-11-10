// cstv_platform.h for Windows (cstv_client.dll). Kept apart from cstv_client.cpp: windows.h and the
// Source SDK headers don't mix. cstvd listens on 127.0.0.1:CSTV_TCP_PORT; the frame and chat buffers
// are named file mappings (cstv_proto.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cstv_proto.h"
#include "cstv_platform.h"

double plat_now()
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

typedef void *(*CreateInterfaceFn_t)(const char *name, int *ret);

void *plat_interface(const char *module, const char *iface)
{
    char name[64];
    snprintf(name, sizeof name, "%s.dll", module);
    HMODULE h = GetModuleHandleA(name);
    if (!h) return nullptr;
    auto ci = (CreateInterfaceFn_t)(void *)GetProcAddress(h, "CreateInterface");
    return ci ? ci(iface, nullptr) : nullptr;
}

// tier0's Msg (extern "C" on Windows, unlike the C++ ConMsg): printf-style, goes to the console
void *plat_conmsg()
{
    HMODULE h = GetModuleHandleA("tier0.dll");
    return h ? (void *)GetProcAddress(h, "Msg") : nullptr;
}

void *plat_swap_slot(void *obj, int slot, void *fn)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
    void *orig = InterlockedExchangePointer(&vt[slot], fn);
    DWORD tmp;
    VirtualProtect(&vt[slot], sizeof(void *), old, &tmp);
    return orig;
}

intptr_t plat_connect()
{
    static bool wsa;
    if (!wsa) {   // the game started winsock already; this only adds a reference
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return -1;
        wsa = true;
    }
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(CSTV_TCP_PORT);
    if (connect(s, (sockaddr *)&a, sizeof a)) { closesocket(s); return -1; }   // refused at once if cstvd isn't there
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);   // like MSG_DONTWAIT on Linux: never stall the game's frame
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
    return (intptr_t)s;
}

int plat_send(intptr_t s, const char *buf, int n) { return send((SOCKET)s, buf, n, 0); }

int plat_recv(intptr_t s, char *buf, int n) { return recv((SOCKET)s, buf, n, 0); }

void plat_close(intptr_t s) { closesocket((SOCKET)s); }

void *plat_map(const char *name, size_t size)
{
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return nullptr;
    void *v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(m);   // the view keeps the mapping alive
    return v;
}

void plat_unmap(void *p, size_t) { UnmapViewOfFile(p); }

bool plat_in_game()
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const char *base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    return _stricmp(base, "cstrike_win64.exe") == 0;
}

bool plat_disabled()
{
    char v[8];
    DWORD n = GetEnvironmentVariableA("RAWINPUT2_CSTV", v, sizeof v);
    return n > 0 && n < sizeof v && !strcmp(v, "0");
}

bool plat_claim()
{
    char v[8];
    if (GetEnvironmentVariableA("CSTV_CLIENT_LOADED", v, sizeof v)) return false;
    SetEnvironmentVariableA("CSTV_CLIENT_LOADED", "1");
    return true;
}

bool plat_claimed()
{
    char v[8];
    return GetEnvironmentVariableA("CSTV_CLIENT_LOADED", v, sizeof v) > 0;
}

// cstvd.exe lives in the cstv folder next to this DLL. The client asks for it when connecting has failed
// for a while (the launcher's copy quits 15 s after CS:TV was turned off, and nothing else restarts it).
// One at a time, and never while one runs: a second cstvd resets the shared frame buffer before it
// finds the port taken.
static HANDLE g_spawned;
static ULONGLONG g_last_spawn;

static bool cstvd_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;   // can't tell: don't risk a second one
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"cstvd.exe") == 0;
    CloseHandle(snap);
    return found;
}

void plat_spawn_helper()
{
    ULONGLONG now = GetTickCount64();
    if (g_last_spawn && now - g_last_spawn < 10000) return;
    if (g_spawned) {
        if (WaitForSingleObject(g_spawned, 0) == WAIT_TIMEOUT) return;   // ours is still starting up
        CloseHandle(g_spawned);
        g_spawned = nullptr;
    }
    if (cstvd_running()) return;
    g_last_spawn = now;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)&plat_spawn_helper, &self)) return;
    wchar_t dir[MAX_PATH], cwd[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 2];
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    if (swprintf(cwd, MAX_PATH, L"%ls\\cstv", dir) < 0 || swprintf(exe, MAX_PATH, L"%ls\\cstvd.exe", cwd) < 0
        || swprintf(cmd, MAX_PATH + 2, L"\"%ls\"", exe) < 0) return;
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
    SetEnvironmentVariableW(L"CSTV_QUIT_WITHOUT_GAME", L"1");   // it quits 15 s after the game has gone
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi)) return;
    CloseHandle(pi.hThread);
    g_spawned = pi.hProcess;
}

struct thread_arg { void *(*fn)(void *); };

static DWORD WINAPI thread_main(LPVOID p)
{
    thread_arg a = *(thread_arg *)p;
    delete (thread_arg *)p;
    a.fn(nullptr);
    return 0;
}

bool plat_thread(void *(*fn)(void *))
{
    thread_arg *a = new thread_arg{fn};
    HANDLE t = CreateThread(nullptr, 0, thread_main, a, 0, nullptr);
    if (!t) { delete a; return false; }
    CloseHandle(t);
    return true;
}

void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
