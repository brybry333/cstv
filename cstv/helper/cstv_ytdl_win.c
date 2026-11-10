// cstv_ytdl.exe (Windows build only). mpv's ytdl_path has to be a program, not a script: this runs the
// bundled Python with cstv_ytdl.py from our own folder, passing on our arguments, stdin/stdout/stderr and
// the exit code. No console window; if we are killed, the job object takes Python (and its yt-dlp) along.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// one argument the way CommandLineToArgvW / the MSVC CRT reads it back
static void quote(wchar_t *out, size_t n, size_t *o, const wchar_t *a)
{
#define PUT(c) do { if (*o + 1 < n) out[(*o)++] = (c); } while (0)
    if (*a && !wcspbrk(a, L" \t\"")) { for (; *a; a++) PUT(*a); return; }
    PUT(L'"');
    for (;; a++) {
        size_t bs = 0;
        while (*a == L'\\') { a++; bs++; }
        if (!*a) { for (size_t i = 0; i < bs * 2; i++) PUT(L'\\'); break; }
        if (*a == L'"') { for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\'); PUT(L'"'); }
        else { for (size_t i = 0; i < bs; i++) PUT(L'\\'); PUT(*a); }
    }
    PUT(L'"');
#undef PUT
}

int wmain(int argc, wchar_t **argv)
{
    static wchar_t dir[32768], py[32768], script[32768], cmd[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, 32768);
    if (!n || n >= 32768) return 127;
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (sl) *sl = 0;
    _snwprintf(py, 32767, L"%ls\\python\\python.exe", dir);
    _snwprintf(script, 32767, L"%ls\\cstv_ytdl.py", dir);

    size_t o = 0;
    quote(cmd, 32768, &o, py);
    cmd[o++] = L' ';
    quote(cmd, 32768, &o, script);
    for (int i = 1; i < argc; i++) {
        if (o + 1 < 32768) cmd[o++] = L' ';
        quote(cmd, 32768, &o, argv[i]);
    }
    cmd[o < 32768 ? o : 32767] = 0;

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return 127;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
