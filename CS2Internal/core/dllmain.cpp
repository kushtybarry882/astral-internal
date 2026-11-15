// language: C++, file: dllmain.cpp, runtime: manual-mapped DLL (image hidden from
// the PEB loader list, so the OS loader never runs), target: Windows 11 x64, MSVC.
// Flags: /ENTRY:DllMain /GS- (see CMakeLists.txt).
//
// UPGRADE: 0-1, 0-2, 0-3, 0-4, 0-thread-start
// LOG-DIAG: astral_boot.log (BootLog, CRT-free) traces the earliest stages;
// CRT initialization happens in DllMain itself (STEP 3), after the mapper's TLS stub.
// UPGRADE-LOG: every MainThread/DllMain step logs to %TEMP%\astral_boot.log / astral_debug.log.
//
// Because the manual mapper never hands control to the NT loader, the C runtime
// bootstrap that the OS would have performed is done here, explicitly:
//   * __security_init_cookie()        -> seed the GS stack-cookie
//   * _CRT_INIT(..., DLL_PROCESS_ATTACH) -> run C++ static initializers (.CRT)
// TLS callbacks (incl. the VCRUNTIME per-thread bootstrap) are dispatched by the
// loader on THIS thread before it calls DllMain, in the order the OS loader uses.
// std:: types stay available: the CRT is still linked (no /NODEFAULTLIB), only the
// implicit CRT entry (_dllmainCRTStartup) is bypassed by /ENTRY:DllMain.
#include "hooks.h"
#include "sdk.h"
#include "memory.h"
#include "evasion.h"
#include "security.h"
#include "offsets.hpp"
#include "offset_parser.hpp"
#include "config_io.h"

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <mutex>

// N-H-5: no dllexport — the export table entry is a scanner beacon.
// The pipe name is passed via the loader's entry stub parameter instead.
extern "C" const wchar_t* g_pipeNamePtr = nullptr;

// LOG-DIAG: CRT-free boot logger. Writes to %USERPROFILE%\Downloads\ASTRAL\astral_boot.log
// so the log survives crashes and is easy to find. Safe to call before _CRT_INIT runs
// (no CRT, no STL) when called with a single plain-string argument. Variadic format
// calls require the CRT (only used after _CRT_INIT in MainThread).
#include <cstdarg>
#include <cstdio>

void BootLog(const char* fmt, ...) {
    wchar_t userProfile[MAX_PATH] = {};
    wchar_t dir[MAX_PATH + 64] = {};
    if (ExpandEnvironmentStringsW(L"%USERPROFILE%", userProfile, MAX_PATH) > 0) {
        wsprintfW(dir, L"%s\\Downloads\\ASTRAL", userProfile);
        CreateDirectoryW(dir, nullptr);
    } else {
        wchar_t tmp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp) == 0) {
            wsprintfW(dir, L"C:\\");
        } else {
            wsprintfW(dir, L"%s", tmp);
        }
    }
    wchar_t path[MAX_PATH + 64] = {};
    wsprintfW(path, L"%s\\astral_boot.log", dir);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char line[1024] = {};
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    // Append timestamp + tid prefix
    char prefixed[1024] = {};
    int pn = wsprintfA(prefixed, "[%02d:%02d:%02d.%03d] tid=%lu ",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId());
    if (pn + n < (int)sizeof(prefixed)) {
        memcpy(prefixed + pn, line, static_cast<size_t>(n));
        pn += n;
    }
    memcpy(prefixed + pn, "\r\n", 2);
    pn += 2;

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, prefixed, static_cast<DWORD>(pn), &written, nullptr);
        CloseHandle(h);
    }
    // N-H-8: no OutputDebugStringA — debug viewers capture all telemetry.
}

static void BootLogPtr(const char* tag, uintptr_t value) {
    char line[256];
    wsprintfA(line, "%s=0x%p", tag ? tag : "(null)", reinterpret_cast<void*>(value));
    BootLog(line);
}


// Forward declaration (defined below; called from DoHandshake on the handshake
// worker thread spawned in DllMain).
extern "C" __declspec(dllexport) void InitializeSecureContext(const wchar_t* pipeName);

// From the statically linked CRT (libcmt). _CRT_INIT seeds the security
// cookie and runs the .CRT$XCU static initializer table. Called from DllMain
// on the thread that owns the module (STEP 3) — the manual mapper's TLS stub
// ran before DllMain, exactly as the OS loader runs TLS callbacks first.
extern "C" BOOL WINAPI _CRT_INIT(void* hModule, DWORD dwReason, void* lpReserved);
extern "C" void __cdecl __security_init_cookie(void);

static void DoHandshake() {
    BootLog("[ASTRAL] DoHandshake: started");
    if (g_pipeNamePtr) {
        BootLog("[ASTRAL] DoHandshake: g_pipeNamePtr non-null");
        BootLog("[ASTRAL] DoHandshake: calling HideCurrentThread");
        evasion::HideCurrentThread();
        BootLog("[ASTRAL] DoHandshake: HideCurrentThread done");
        BootLog("[ASTRAL] DoHandshake: calling InitializeSecureContext");
        InitializeSecureContext(g_pipeNamePtr);
        BootLog("[ASTRAL] DoHandshake: InitializeSecureContext done");
    } else {
        BootLog("[ASTRAL] DoHandshake: g_pipeNamePtr is null");
    }

    // 0-4: erase the exported pipe-name pointer once the handshake is done.
    uintptr_t ptrAddr = reinterpret_cast<uintptr_t>(&g_pipeNamePtr);
    memory::Write<uintptr_t>(ptrAddr, 0);
    g_pipeNamePtr = nullptr;

    BootLog("[ASTRAL] DoHandshake: returning");
}

static std::atomic<bool> g_crtReady{false};

// The CRT is fully initialized by DllMain before any thread is spawned, so this
// wait always returns immediately — kept only as a defensive gate.
static DWORD WINAPI HandshakeThread(LPVOID lpParam) {
    (void)lpParam;
    BootLog("[THREAD] HandshakeThread entered");
    BootLogPtr("[THREAD] hModule", reinterpret_cast<uintptr_t>(lpParam));
    for (int tries = 0; !g_crtReady.load() && tries < 6000; ++tries) {
        Sleep(10);
    }
    Security::InitializeNTSClear();
    Security::HideFromDebugger();
    DoHandshake();
    BootLog("[THREAD] HandshakeThread done");
    return 0;
}

// Forward declarations + stub page for the thread-start trampoline.
// 0-thread-start (check: "Thread start address does not point inside the mapped
// image"). A tiny stub page is allocated OUTSIDE the image; the two DllMain
// threads' Win32 start addresses resolve to this page instead of into the DLL.
static DWORD WINAPI HandshakeThread(LPVOID lpParam);
namespace cs2 {
static DWORD WINAPI MainThread(LPVOID lpParam);
}

static BYTE* g_stub = nullptr;
static BYTE* GetThreadStub() {
    if (g_stub) {
        return g_stub;
    }
    BYTE* buf = reinterpret_cast<BYTE*>(VirtualAlloc(nullptr, 0x100,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) {
        return nullptr;
    }
    auto emitStub = [&](int off, uintptr_t target) {
        buf[off + 0] = 0x48; buf[off + 1] = 0xB8;          // mov rax, target
        memcpy(buf + off + 2, &target, sizeof(target));
        buf[off + 10] = 0xFF; buf[off + 11] = 0xE0;        // jmp rax
    };
    emitStub(0x00, reinterpret_cast<uintptr_t>(&cs2::MainThread));
    emitStub(0x20, reinterpret_cast<uintptr_t>(&HandshakeThread));
    DWORD oldProtect = 0;
    VirtualProtect(buf, 0x100, PAGE_EXECUTE_READ, &oldProtect);
    g_stub = buf;
    return buf;
}

static std::atomic<bool> g_shutdownRequested{false};

// Table-driven CRC-32 (IEEE). The previous implementation ran an inner 8-step
// bit loop PER BYTE and did it twice every 5 s across the whole .text section
// — a multi-hundred-millisecond stall per scan. The table version is roughly
// 10x faster and still byte-accurate.
static uint32_t ComputeTextChecksum(const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static std::once_flag s_tableOnce;
    std::call_once(s_tableOnce, []() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
    });
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

namespace cs2 {

static DWORD WINAPI MainThread(LPVOID lpParam) {
    __try {
        BootLog("[MAIN] MainThread entered");
        BootLogPtr("[MAIN] lpParam", reinterpret_cast<uintptr_t>(lpParam));
        BootLog("[MAIN] selfModule entered");

        Security::InitializeNTSClear();
        Security::HideFromDebugger();

        HMODULE selfModule = reinterpret_cast<HMODULE>(lpParam);
        if (selfModule) {
            BootLog("[MAIN] calling InitializeIntegrityCheck");
            // These read the PE headers, so they run BEFORE the wipe.
            Security::InitializeIntegrityCheck(selfModule);
            BootLog("[MAIN] InitializeIntegrityCheck done");

            BootLog("[MAIN] calling ApplyZeroRWX");
            Security::ApplyZeroRWX(selfModule);
            BootLog("[MAIN] ApplyZeroRWX done");

            // 0-1: remove the image from the PEB loader lists (no-op if the mapper
            // never linked it, which is the usual case — unlink is a safety net).
            BootLog("[MAIN] calling UnlinkFromPEB");
            evasion::UnlinkFromPEB(selfModule);
            BootLog("[MAIN] UnlinkFromPEB done");

            // 0-2: zero SizeOfHeaders so memory scanners find no MZ/PE signature at
            // the mapped base. Runs on this thread, in-process.
            BootLog("[MAIN] calling WipePeHeaders");
            evasion::WipePeHeaders(selfModule);
            BootLog("[MAIN] WipePeHeaders done");
        } else {
            BootLog("[MAIN] WARNING: lpParam is null — integrity check/header wipe skipped");
        }

        BootLog("[MAIN] calling ParseFromGamehacksFolder");
        Offsets::ParseFromGamehacksFolder();
        BootLog("[MAIN] offsets parsed, waiting for client.dll");

        while (!GetModuleHandleA("client.dll")) {
            Sleep(100);
        }
        BootLog("[MAIN] client.dll found");
        while (!GetModuleHandleA("tier0.dll")) {
            Sleep(100);
        }
        BootLog("[MAIN] tier0.dll found");
        Sleep(500);

        BootLog("[MAIN] validating offsets before hook init");
        {
            HMODULE clientMod = GetModuleHandleA("client.dll");
            if (clientMod) {
                bool offsetsRuntimeValid = Offsets::ValidateOffsetsRuntime(reinterpret_cast<uintptr_t>(clientMod));
                BootLog("[MAIN] ValidateOffsetsRuntime done");
                (void)offsetsRuntimeValid;
            }
            bool offsetsValid = Offsets::ValidateOffsets();
            BootLog("[MAIN] ValidateOffsets done");
            (void)offsetsValid;

            BootLog("[MAIN] calling HookManager::Initialize");
            hooks::HookManager& hm = hooks::HookManager::Get();
            if (hm.Initialize()) {
                BootLog("[MAIN] HookManager::Initialize OK");
            } else {
                BootLog("[MAIN] HookManager::Initialize FAILED");
            }

            BootLog("[MAIN] loading default config");
            ConfigIO::AutoLoadDefault();
            BootLog("[MAIN] default config loaded; entering CRC monitor loop");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_mainThreadExceptionLogged = false;
        if (!s_mainThreadExceptionLogged) {
            s_mainThreadExceptionLogged = true;
            BootLog("[MAIN] unhandled exception caught — hooks/config disabled");
        }
    }

    {
        HMODULE hMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(&cs2::MainThread), &hMod);
        if (hMod) {
            PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
            PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
                reinterpret_cast<uintptr_t>(hMod) + dos->e_lfanew);
            uintptr_t textBase = reinterpret_cast<uintptr_t>(hMod) +
                nt->OptionalHeader.BaseOfCode;
            DWORD textSize = nt->OptionalHeader.SizeOfCode;

            uint32_t initialCrc = 0;
            __try {
                initialCrc = ComputeTextChecksum(reinterpret_cast<const uint8_t*>(textBase), textSize);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                BootLog("[MAIN] CRC: failed to compute initial CRC");
                initialCrc = 0;
            }

            while (!g_shutdownRequested) {
                Sleep(5000);
                if (g_shutdownRequested) {
                    break;
                }
                if (initialCrc == 0) {
                    continue;
                }
                uint32_t currentCrc = 0;
                __try {
                    currentCrc = ComputeTextChecksum(reinterpret_cast<const uint8_t*>(textBase), textSize);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    BootLog("[MAIN] CRC: exception reading .text section");
                    continue;
                }
                // Verify DLL integrity (section CRCs, not just .text).
                if (!Security::VerifyIntegrity()) {
                    BootLog("[MAIN] Integrity check failed — DLL may be tampered");
                    hooks::HookManager::Get().Shutdown();
                    g_shutdownRequested = true;
                }

                if (currentCrc != initialCrc) {
                    BootLog("[MAIN] CRC mismatch detected - disabling hooks instead of killing process");
                    hooks::HookManager::Get().Shutdown();
                    g_shutdownRequested = true;
                }
                if (g_shutdownRequested) {
                    break;
                }
            }
        }
    }

    while (!g_shutdownRequested) {
        Sleep(1000);
    }
    hooks::HookManager::Get().Shutdown();
    return 0;
}

} // namespace cs2

extern "C" __declspec(dllexport) void InitializeSecureContext(const wchar_t* pipeName) {
    if (!pipeName) {
        return;
    }

    for (int retry = 0; retry < 30; ++retry) {
        HANDLE pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            BootLog("[ASTRAL] InitializeSecureContext: pipe connected");

            uint8_t token[32] = {0};
            DWORD bytesRead = 0;
            if (!ReadFile(pipe, token, 32, &bytesRead, nullptr) || bytesRead != 32) {
                BootLog("[ASTRAL] InitializeSecureContext: token read failed");
                CloseHandle(pipe);
                return;
            }
            BootLog("[ASTRAL] InitializeSecureContext: token read OK");

            DWORD processId = 0;
            if (!ReadFile(pipe, &processId, sizeof(processId), &bytesRead, nullptr) ||
                bytesRead != sizeof(processId)) {
                BootLog("[ASTRAL] InitializeSecureContext: PID read failed");
                CloseHandle(pipe);
                return;
            }
            BootLog("[ASTRAL] InitializeSecureContext: PID read OK");

            if (processId != GetCurrentProcessId()) {
                BootLog("[ASTRAL] InitializeSecureContext: PID mismatch");
                CloseHandle(pipe);
                return;
            }

            for (int i = 0; i < 16; ++i) {
                uint8_t tmp = token[i];
                token[i] = token[31 - i];
                token[31 - i] = tmp;
            }

            DWORD bytesWritten = 0;
            if (!WriteFile(pipe, token, 32, &bytesWritten, nullptr) ||
                bytesWritten != 32) {
                BootLog("[ASTRAL] InitializeSecureContext: token write failed");
                CloseHandle(pipe);
                return;
            }
            BootLog("[ASTRAL] InitializeSecureContext: handshake complete");

            CloseHandle(pipe);
            return;
        }
        Sleep(500);
    }
    BootLog("[ASTRAL] InitializeSecureContext: timed out waiting for pipe (handshake disabled?)");
}

// Entry point. /ENTRY:DllMain (see CMakeLists.txt) makes this the image's
// AddressOfEntryPoint instead of _dllmainCRTStartup.
//
// LOG-DIAG / STEP 3: the CRT is initialized HERE, on the same thread that owns
// the module (the TLS stub ran first, just like the OS loader would run TLS
// callbacks before DllMain). This kills the deadlock where a spawned thread
// deadlocks inside _CRT_INIT while DllMain still holds the loader lock.
// Pre-CRT logging uses BootLog (raw CreateFileW/WriteFile) — never std::ofstream.
extern "C" BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        BootLog("[DLLMAIN] entered");
        BootLogPtr("[DLLMAIN] hModule", reinterpret_cast<uintptr_t>(hModule));

        __security_init_cookie();
        BootLog("[DLLMAIN] cookie seeded");

        _CRT_INIT(hModule, DLL_PROCESS_ATTACH, nullptr);
        g_crtReady.store(true);
        BootLog("[DLLMAIN] _CRT_INIT returned");

        DisableThreadLibraryCalls(hModule);
        BootLog("[DLLMAIN] DisableThreadLibraryCalls ok");

        // 0-thread-start + STEP 3 ordering: spawn MainThread FIRST, then
        // HandshakeThread (the main thread owns the module setup; handshake
        // negotiation runs in parallel).
        BYTE* stub = GetThreadStub();
        HANDLE hMain = stub ?
            CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub + 0x00), hModule, 0, nullptr) :
            CreateThread(nullptr, 0, cs2::MainThread, hModule, 0, nullptr);
        BootLog("[DLLMAIN] MainThread spawned");

        HANDLE hHandshake = stub ?
            CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub + 0x20), hModule, 0, nullptr) :
            CreateThread(nullptr, 0, HandshakeThread, hModule, 0, nullptr);
        BootLog("[DLLMAIN] HandshakeThread spawned");

        if (hHandshake) CloseHandle(hHandshake);
        if (hMain) CloseHandle(hMain);

        BootLog("[DLLMAIN] spawned OK, DllMain returning");
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_shutdownRequested = true;
    }

    return TRUE;
}
