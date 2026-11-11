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
    DWORD n = GetEnvironmentVariableA("CSTV_DISABLE", v, sizeof v);
    return n > 0 && n < sizeof v && strcmp(v, "0");
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

// ---------------------------------------------------------------- the game window's input
// Our window procedure goes in front of the game's (and inputsystem's), so a message we keep never reaches it.
static plat_event_fn g_event_fn;
static HWND g_hwnd;
static WNDPROC g_old_proc;
static bool g_capture;     // the CS:TV window is open: mouse motion is ours
static bool g_no_legacy;   // the game asked for raw mouse without WM_*BUTTON* messages
static wchar_t g_high_surrogate;
static int g_wheel_rest;
// MK_* of the buttons whose press we kept from the game. Every mouse message's wParam lists the buttons held and
// inputsystem presses whatever it lists, so these have to stay out of that list until they're released.
static WPARAM g_kept;

static int sdl_scancode(WPARAM vk, LPARAM lp)
{
    if (vk >= 'A' && vk <= 'Z') return 4 + (int)vk - 'A';
    if (vk >= '1' && vk <= '9') return 30 + (int)vk - '1';
    if (vk >= VK_F1 && vk <= VK_F12) return 58 + (int)vk - VK_F1;
    switch (vk) {
    case '0': return 39;
    case VK_RETURN: return (lp & (1 << 24)) ? 88 : 40;
    case VK_ESCAPE: return 41;
    case VK_BACK: return 42;
    case VK_TAB: return 43;
    case VK_SPACE: return 44;
    case VK_OEM_MINUS: return 45;
    case VK_OEM_PLUS: return 46;
    case VK_OEM_4: return 47;
    case VK_OEM_6: return 48;
    case VK_OEM_5: return 49;
    case VK_OEM_1: return 51;
    case VK_OEM_7: return 52;
    case VK_OEM_3: return 53;
    case VK_OEM_COMMA: return 54;
    case VK_OEM_PERIOD: return 55;
    case VK_OEM_2: return 56;
    case VK_CAPITAL: return 57;
    case VK_INSERT: return 73;
    case VK_HOME: return 74;
    case VK_PRIOR: return 75;
    case VK_DELETE: return 76;
    case VK_END: return 77;
    case VK_NEXT: return 78;
    case VK_RIGHT: return 79;
    case VK_LEFT: return 80;
    case VK_DOWN: return 81;
    case VK_UP: return 82;
    }
    return 0;
}

int plat_scancode_from_name(const char *name)
{
    if (name[0] && !name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return 4 + c - 'a';
        if (c >= 'A' && c <= 'Z') return 4 + c - 'A';
        if (c >= '1' && c <= '9') return 30 + c - '1';
        if (c == '0') return 39;
        if (c == ';') return 51;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return 57 + n;
    }
    static const struct { const char *name; int sc; } keys[] = {
        {"Return", 40}, {"ESCAPE", 41}, {"BACKSPACE", 42}, {"TAB", 43}, {"SPACE", 44}, {"CapsLock", 57}, {"Insert", 73},
        {"HOME", 74}, {"PageUp", 75}, {"Delete", 76}, {"END", 77}, {"PageDown", 78}, {"Right", 79}, {"Left", 80},
        {"Down", 81}, {"Up", 82},
    };
    for (auto &k : keys)
        if (!_stricmp(name, k.name)) return k.sc;
    return 0;
}

static int send_button(int button, bool down)
{
    plat_event e = {};
    e.type = down ? PLAT_BUTTONDOWN : PLAT_BUTTONUP;
    e.button = button;
    return g_event_fn(&e);
}

static int send_wheel(int delta)
{
    g_wheel_rest += delta;
    int notches = g_wheel_rest / WHEEL_DELTA;
    if (!notches) return g_capture ? 0 : 1;
    g_wheel_rest -= notches * WHEEL_DELTA;
    plat_event e = {};
    e.type = PLAT_WHEEL;
    e.wheel = notches;
    return g_event_fn(&e);
}

// true: the game shouldn't see this message
static bool on_raw_input(LPARAM lp)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return false;
    if (ri.header.dwType != RIM_TYPEMOUSE) return false;
    const RAWMOUSE &m = ri.data.mouse;
    bool keep = false;
    if (g_capture && !(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
        plat_event e = {};
        e.type = PLAT_MOTION;
        e.dx = m.lLastX;
        e.dy = m.lLastY;
        keep = !g_event_fn(&e);
    }
    if (g_no_legacy) {
        static const struct { USHORT down, up; int button; } map[] = {
            {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP, 1}, {RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_2_UP, 3},
            {RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_3_UP, 2}, {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4},
            {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5},
        };
        for (auto &b : map) {
            if (m.usButtonFlags & b.down) keep |= !send_button(b.button, true);
            if (m.usButtonFlags & b.up) keep |= !send_button(b.button, false);
        }
        if (m.usButtonFlags & RI_MOUSE_WHEEL) keep |= !send_wheel((SHORT)m.usButtonData);
    }
    return keep;
}

static LRESULT CALLBACK game_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_KEYDOWN: {
        plat_event e = {};
        e.type = PLAT_KEYDOWN;
        e.scancode = sdl_scancode(w, l);
        e.repeat = (l >> 30) & 1;
        e.ctrl = GetKeyState(VK_CONTROL) < 0;
        if (!g_event_fn(&e)) return 0;
        break;
    }
    case WM_CHAR: {
        plat_event e = {};
        e.type = PLAT_TEXT;
        wchar_t units[2] = {(wchar_t)w, 0};
        int count = 1;
        if (w >= 0xD800 && w < 0xDC00) { g_high_surrogate = (wchar_t)w; count = 0; }
        else if (w >= 0xDC00 && w < 0xE000) { units[0] = g_high_surrogate; units[1] = (wchar_t)w; count = 2; }
        if (count && w >= 0x20 && w != 0x7F)
            WideCharToMultiByte(CP_UTF8, 0, units, count, e.text, sizeof e.text - 1, nullptr, nullptr);
        if (!g_event_fn(&e)) return 0;
        break;
    }
    case WM_INPUT:
        if (on_raw_input(l)) return DefWindowProcW(h, msg, w, l);
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!send_button(1, true)) { g_kept |= MK_LBUTTON; return 0; }
        break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        if (!send_button(2, true)) { g_kept |= MK_MBUTTON; return 0; }
        break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        if (!send_button(3, true)) { g_kept |= MK_RBUTTON; return 0; }
        break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        if (!send_button(HIWORD(w) == XBUTTON1 ? 4 : 5, true)) { g_kept |= HIWORD(w) == XBUTTON1 ? MK_XBUTTON1 : MK_XBUTTON2; return TRUE; }
        break;
    case WM_LBUTTONUP:
        send_button(1, false);
        if (g_kept & MK_LBUTTON) { g_kept &= ~(WPARAM)MK_LBUTTON; return 0; }
        break;
    case WM_MBUTTONUP: if (g_kept & MK_MBUTTON) { g_kept &= ~(WPARAM)MK_MBUTTON; return 0; } break;
    case WM_RBUTTONUP: if (g_kept & MK_RBUTTON) { g_kept &= ~(WPARAM)MK_RBUTTON; return 0; } break;
    case WM_XBUTTONUP: {
        WPARAM b = HIWORD(w) == XBUTTON1 ? MK_XBUTTON1 : MK_XBUTTON2;
        if (g_kept & b) { g_kept &= ~b; return TRUE; }
        break;
    }
    case WM_MOUSEWHEEL: if (!send_wheel(GET_WHEEL_DELTA_WPARAM(w))) return 0; break;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) w &= ~g_kept;
    return CallWindowProcW(g_old_proc, h, msg, w, l);
}

static BOOL CALLBACK find_game_window(HWND h, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    char cls[32];
    if (pid != GetCurrentProcessId() || !GetClassNameA(h, cls, sizeof cls) || strcmp(cls, "Valve001")) return TRUE;
    *(HWND *)out = h;
    return FALSE;
}

bool plat_input_install(plat_event_fn fn)
{
    if (g_old_proc) return true;
    static ULONGLONG next_try;
    ULONGLONG now = GetTickCount64();
    if (now < next_try) return false;
    next_try = now + 1000;
    HWND h = nullptr;
    EnumWindows(find_game_window, (LPARAM)&h);
    if (!h) return false;
    g_event_fn = fn;
    g_hwnd = h;
    g_old_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)game_wndproc);
    return g_old_proc != nullptr;
}

void plat_input_window(bool open)
{
    g_capture = open;
    if (!open || !g_hwnd) return;
    // our cursor moves by raw mouse motion, which the game only asks for with m_rawinput 1
    RAWINPUTDEVICE cur[16];
    UINT n = 16;
    bool have = false;
    g_no_legacy = false;
    UINT got = GetRegisteredRawInputDevices(cur, &n, sizeof cur[0]);
    for (UINT i = 0; got != (UINT)-1 && i < got; i++)
        if (cur[i].usUsagePage == 1 && cur[i].usUsage == 2) { have = true; g_no_legacy = (cur[i].dwFlags & RIDEV_NOLEGACY) != 0; }
    if (!have) {
        RAWINPUTDEVICE d = {1, 2, 0, g_hwnd};
        RegisterRawInputDevices(&d, 1, sizeof d);
    }
}

char *plat_clipboard_get()
{
    if (!OpenClipboard(nullptr)) return nullptr;
    char *out = nullptr;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t *t = (const wchar_t *)GlobalLock(h)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, t, -1, nullptr, 0, nullptr, nullptr);
            if (n > 0 && (out = (char *)malloc((size_t)n))) WideCharToMultiByte(CP_UTF8, 0, t, -1, out, n, nullptr, nullptr);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

int plat_clipboard_set(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0 || !OpenClipboard(nullptr)) return -1;
    int ret = -1;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t))) {
        if (wchar_t *t = (wchar_t *)GlobalLock(g)) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, t, n);
            GlobalUnlock(g);
            if (SetClipboardData(CF_UNICODETEXT, g)) ret = 0; else GlobalFree(g);
        } else GlobalFree(g);
    }
    CloseClipboard();
    return ret;
}
