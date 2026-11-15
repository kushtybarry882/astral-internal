#pragma once

// LOG-DIAG: log path resolved via GetTempPathW.
// Logs live in %USERPROFILE%\Downloads\ASTRAL so they're easy to find after
// a crash. Falls back to %TEMP% then C:\ if the profile dir can't be read.
// Uses raw Win32 file I/O (no std::ofstream / std::filesystem) so it cannot
// crash on static stream initialization inside a manually-mapped DLL.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

namespace cs2 {

namespace detail {

    inline void GetLogDirW(wchar_t* out, DWORD outSize) {
        wchar_t userProfile[MAX_PATH] = {};
        if (ExpandEnvironmentStringsW(L"%USERPROFILE%", userProfile, MAX_PATH) > 0) {
            wsprintfW(out, L"%s\\Downloads\\ASTRAL", userProfile);
        } else {
            wchar_t tmp[MAX_PATH] = {};
            if (GetTempPathW(MAX_PATH, tmp) > 0) {
                wsprintfW(out, L"%s", tmp);
            } else {
                wsprintfW(out, L"C:\\");
            }
        }
    }

    inline HANDLE& LogFileHandle() {
        static HANDLE h = INVALID_HANDLE_VALUE;
        static bool tried = false;
        if (!tried) {
            tried = true;
            wchar_t dir[MAX_PATH + 64] = {};
            GetLogDirW(dir, sizeof(dir) / sizeof(dir[0]));
            CreateDirectoryW(dir, nullptr);

            wchar_t path[MAX_PATH + 64] = {};
            wsprintfW(path, L"%s\\astral_debug.log", dir);
            h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        return h;
    }

    inline CRITICAL_SECTION& LogCS() {
        static CRITICAL_SECTION cs;
        static bool init = false;
        if (!init) {
            InitializeCriticalSection(&cs);
            init = true;
        }
        return cs;
    }

} // namespace detail

inline void Log(const char* fmt, ...) {
    if (!fmt) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    (void)len;
}

inline void FileLog(const char* fmt, ...) {
    if (!fmt) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buffer)) n = (int)sizeof(buffer) - 1;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char prefixed[1024] = {};
    int pn = wsprintfA(prefixed, "[%02d:%02d:%02d.%03d] tid=%lu ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId());
    if (pn + n < (int)sizeof(prefixed)) {
        memcpy(prefixed + pn, buffer, static_cast<size_t>(n));
        pn += n;
    }
    memcpy(prefixed + pn, "\r\n", 2);
    pn += 2;

    EnterCriticalSection(&detail::LogCS());
    HANDLE h = detail::LogFileHandle();
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, prefixed, static_cast<DWORD>(pn), &written, nullptr);
        FlushFileBuffers(h);
    }
    LeaveCriticalSection(&detail::LogCS());
}

} // namespace cs2
