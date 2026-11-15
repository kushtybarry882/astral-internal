// AUDIT: P1-13, P2-3, P3-7, P3-12
// UPGRADE: F-2, F-3, 1-2, 1-3, M-4, M-5, C-1
// CRASH-FIX: Cause 1 (hkCreateMove null-check), Cause 3 (cmd offset stale detection),
//            Cause 4 (Present/Present1/CreateMove ALREADY_CREATED -> skip, not retry),
//            Cause 6 (MH_Initialize/MH_StatusToString logging), STEP 0-3 (entry logs)
#include "hooks.h"
#include "config.h"
#include "game.h"
#include "gameinfo.h"
#include "memory.h"
#include "offsets.hpp"
#include "log.h"
#include "../ui/menu.h"
#include "../ui/theme.h"
#include "../features/visuals.h"
#include "../features/combat.h"
#include "../features/misc.h"
#include "../features/rage.h"
#include "../features/hooks.h"
#include "usercmd.h"

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <d3d11.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void BootLog(const char* fmt, ...);

namespace hooks {

static HookManager* g_hooks = nullptr;
static CRITICAL_SECTION g_initCS;
static Present_t g_cachedOrigPresentVtable = nullptr;
static Present1_t g_cachedOrigPresent1Vtable = nullptr;

namespace {

void RawLog(const char* msg) {
    if (msg) {
        cs2::FileLog("%s", msg);
    }
}

using GetCmdTick_t = int* (__fastcall*)(void*, int*);
using GetCmdArray_t = void* (__fastcall*)(void*, int);
using GetCmdBySeq_t = void* (__fastcall*)(void*, uint32_t);
using GetUserCmdManager_t = void* (__fastcall*)(void*);
using GetCmdTickOut_t = void (__fastcall*)(void*, int32_t*);

GetCmdTick_t g_getCmdTick = nullptr;
GetCmdArray_t g_getCmdArray = nullptr;
GetCmdBySeq_t g_getCmdBySeq = nullptr;
GetUserCmdManager_t g_getUserCmdManager = nullptr;
GetCmdTickOut_t g_getCmdTickOut = nullptr;
uintptr_t g_cmdArrayGlobal = 0;
bool g_loggedFirstCommand = false;
bool g_loggedCommandFailure = false;

// Patterns from the dump (include/patterns/patterns.json), build 2000942:
//   GetCUserCmdArray        patterns line: inline constexpr std::string_view GetCUserCmdArray        = "48 89 4C 24 08 41 56 41 57 48 83 EC 48 4C 63 FA";
//   GetCUserCmdBySequenceNumber                                = "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C 8B C0";
//   GetTickBase                                            = "8B 81 B8 06 00 00 C3 CC CC CC CC CC CC CC CC CC";
//   CreateMove                                             = "85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40 18 89";
//   pCSGOInputInstance                                     = "48 89 05 ? ? ? ? 0F 57 C0 0F 11 05";
constexpr const char* kCmdArrayPattern = "48 89 4C 24 08 41 56 41 57 48 83 EC 48 4C 63 FA";
constexpr const char* kCmdBySeqPattern = "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C 8B C0";
constexpr const char* kCmdTickPattern = "8B 81 B8 06 00 00 C3 CC CC CC CC CC CC CC CC CC";
constexpr const char* kCmdArrayGlobalPattern = "48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CF 4C 8B F8";
// CUserCmd ring manager (include/engine/cusercmd.h kGetUserCmdManager_rva).
constexpr const char* kUserCmdManagerPattern = "41 56 41 57 48 83 EC ? 48 8D 54 24 68 E8 DE E5";
// getCUserCmdTick(controller, int32_t* outTick) — patterns.json GetEntityIndex.
constexpr const char* kCmdTickOutPattern = "48 83 EC ? 4C 8B 0D ? ? ? ? 4C 8B DA";
// SerializeUserCmd — turns the live CUserCmd into the bytes sent to the server.
// include/patterns/patterns.json (build 2000942): reads [rdx+0x20]/CUserCmd has_bits
// and [rdx+0x30]/input-history size, so rdx IS the CUserCmd.
constexpr const char* kSerializeUserCmdPattern = "40 55 56 41 57 48 83 EC 60 8B 4A 20 45 33 FF 44 8B 42 30";
constexpr uintptr_t kSerializeUserCmdRVA = 0x962660; // dump-confirmed RVA
// CUserCmd ring: 150 entries * 0x98, current sequence at ring+0x5910.
constexpr uintptr_t kCmdRingSequenceOffset = 0x5910;
constexpr uintptr_t kCmdRingStride = 0x98;
constexpr int kCmdRingCount = 150;
constexpr const char* kCSGOInputPattern = "48 89 05 ? ? ? ? 0F 57 C0 0F 11 05";
constexpr const char* kCreateMovePattern = "85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40 18 89";

struct GameWindowSearch {
    DWORD pid;
    HWND best;
    LONG bestArea;
};

BOOL CALLBACK EnumGameWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<GameWindowSearch*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != search->pid || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) {
        return TRUE;
    }
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > search->bestArea) {
        search->bestArea = area;
        search->best = hwnd;
    }
    return TRUE;
}

// CRASH-FIX: the EnumWindows callback path may fault on some systems; keep it,
// but never let it take cs2.exe down. Also added a callback-free primary
// strategy (FindWindowA + PID check) that most systems will use instead.
HWND FindGameWindow() {
    DWORD targetPid = GetCurrentProcessId();

    // Preferred: exact title match, no callbacks involved.
    HWND byTitle = FindWindowA(nullptr, "Counter-Strike 2");
    if (byTitle) {
        DWORD pid = 0;
        GetWindowThreadProcessId(byTitle, &pid);
        if (pid == targetPid) {
            cs2::FileLog("[ImGui] FindGameWindow: title match hwnd=%p pid=%lu", byTitle, pid);
            return byTitle;
        }
        cs2::FileLog("[ImGui] FindGameWindow: title match wrong pid=%lu (want %lu), ignoring", pid, targetPid);
    }

    // Fallback: enumerate windows, SEH-guarded so a bad window can't kill the process.
    GameWindowSearch search = { targetPid, nullptr, 0 };
    __try {
        EnumWindows(EnumGameWindowsProc, reinterpret_cast<LPARAM>(&search));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cs2::FileLog("[ImGui] EnumWindows raised an exception — returning nullptr");
        return nullptr;
    }
    cs2::FileLog("[ImGui] FindGameWindow: enum result hwnd=%p area=%ld", search.best, search.bestArea);
    return search.best;
}

bool IsGameForeground(HWND hwnd) {
    if (!hwnd) {
        return true;
    }
    HWND foreground = GetForegroundWindow();
    return foreground == hwnd || IsChild(hwnd, foreground);
}

void ToggleMenuHotkey() {
    if (GetAsyncKeyState(VK_RSHIFT) & 1) {
        menu::g_ShowMenu = !menu::g_ShowMenu;
        cs2::FileLog("[MENU] toggled -> %d", (int)menu::g_ShowMenu);
    }
}

size_t GetModuleImageSize(HMODULE module) {
    if (!module) {
        return 0;
    }
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

} // namespace

HookManager& HookManager::Get() {
    static HookManager instance;
    return instance;
}

// ImGui is not thread-safe (imgui.cpp: "Dear ImGui functions are not
// thread-safe"). CS2 pumps window messages on the game thread while the
// Present hook runs on the render thread, and the Win32 backend pushes events
// into the shared ImGuiIO input queue. This recursive mutex serializes the
// WndProc handler with the frame. Recursive so WndProc can be re-entered from
// inside the handler (SetCapture/SetCursor can dispatch messages).
static std::recursive_mutex g_imguiMutex;

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool g_wndProcEntered = false;
    if (!g_wndProcEntered) {
        g_wndProcEntered = true;
        cs2::FileLog("[THREAD] WndProc entered");
    }
    {
        static std::atomic<int> s_wndMsgLog{0};
        int n = ++s_wndMsgLog;
        if (n <= 10) {
            cs2::FileLog("[WND] msg=0x%04X wParam=0x%llX menu=%d",
                         msg, static_cast<unsigned long long>(wParam),
                         (int)menu::g_ShowMenu);
        }
    }
    if (g_hooks && g_hooks->IsImGuiInitialized()) {
        bool imguiWantCapture = false;
        // Hold the frame lock while feeding the input handler. Manual
        // lock/unlock (not lock_guard) because with /EHsc an SEH fault does not
        // guarantee C++ destructors run — a skipped unlock would freeze the
        // render thread forever.
        g_imguiMutex.lock();
        __try {
            imguiWantCapture = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool g_wndExceptionLogged = false;
            if (!g_wndExceptionLogged) {
                g_wndExceptionLogged = true;
                cs2::FileLog("WndProc: exception in ImGui input handler caught");
            }
            imguiWantCapture = false;
        }
        g_imguiMutex.unlock();
        if (imguiWantCapture && menu::g_ShowMenu) {
            return 1;
        }
    }

    if (g_hooks && g_hooks->GetOrigWndProc()) {
        return CallWindowProc(g_hooks->GetOrigWndProc(), hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static bool g_runtimeValidated = false;
static std::chrono::steady_clock::time_point g_lastRefresh{}; // epoch — first call always refreshes

// H-10: Game::Refresh() throttled to ~10 Hz instead of every render frame.
static void ThrottledRefresh() {
    auto now = std::chrono::steady_clock::now();
    if (now - g_lastRefresh < std::chrono::milliseconds(100)) {
        return;
    }
    g_lastRefresh = now;
    Game::Refresh();
}

// CRASH-FIX: Cause 1 — hook bodies never call a null original; entry logged once.
HRESULT WINAPI HookManager::hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool g_presentEntered = false;
    if (!g_presentEntered) {
        g_presentEntered = true;
        cs2::FileLog("[THREAD] hkPresent entered");
    }
    if (!g_hooks) {
        if (g_cachedOrigPresentVtable) {
            return g_cachedOrigPresentVtable(swapChain, syncInterval, flags);
        }
        static bool g_loggedNullHooks = false;
        if (!g_loggedNullHooks) {
            g_loggedNullHooks = true;
            cs2::FileLog("[ASTRAL] hkPresent: g_hooks null and no cached original — returning S_OK (overlay disabled)");
        }
        return S_OK;
    }

    if (!g_runtimeValidated) {
        g_runtimeValidated = true;
        ThrottledRefresh();
        if (g_hooks && g_hooks->IsInitialized()) {
            Offsets::ValidateRuntimeOffsets(g_ClientBase);
        }
    }

    // CRASH-FIX: wrap RenderFrame so an exception inside the overlay/feature
    // pass can't kill cs2.exe on a render frame.
    __try {
        g_hooks->RenderFrame(swapChain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_renderFrameLogged = false;
        if (!s_renderFrameLogged) {
            s_renderFrameLogged = true;
            cs2::FileLog("[ASTRAL] hkPresent: RenderFrame faulted — SEH caught, continuing");
        }
    }

    if (g_cachedOrigPresentVtable) {
        __try {
            return g_cachedOrigPresentVtable(swapChain, syncInterval, flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool s_presentOrigLogged = false;
            if (!s_presentOrigLogged) {
                s_presentOrigLogged = true;
                cs2::FileLog("[ASTRAL] hkPresent: original Present faulted — returning S_OK (overlay disabled)");
            }
            return S_OK;
        }
    }
    return S_OK;
}

HRESULT WINAPI HookManager::hkPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, const void* params) {
    static bool g_present1Entered = false;
    if (!g_present1Entered) {
        g_present1Entered = true;
        cs2::FileLog("[THREAD] hkPresent1 entered");
    }
    if (!g_hooks) {
        if (g_cachedOrigPresent1Vtable) {
            return g_cachedOrigPresent1Vtable(swapChain, syncInterval, flags, params);
        }
        if (g_cachedOrigPresentVtable) {
            return g_cachedOrigPresentVtable(swapChain, syncInterval, flags);
        }
        return S_OK;
    }

    if (!g_runtimeValidated) {
        g_runtimeValidated = true;
        ThrottledRefresh();
        if (g_hooks && g_hooks->IsInitialized()) {
            Offsets::ValidateRuntimeOffsets(g_ClientBase);
        }
    }

    // CRASH-FIX: wrap RenderFrame so an exception inside the overlay/feature
    // pass can't kill cs2.exe on a render frame.
    __try {
        g_hooks->RenderFrame(swapChain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_renderFrameLogged1 = false;
        if (!s_renderFrameLogged1) {
            s_renderFrameLogged1 = true;
            cs2::FileLog("[ASTRAL] hkPresent1: RenderFrame faulted — SEH caught, continuing");
        }
    }

    if (g_cachedOrigPresent1Vtable) {
        __try {
            return g_cachedOrigPresent1Vtable(swapChain, syncInterval, flags, params);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool s_present1OrigLogged = false;
            if (!s_present1OrigLogged) {
                s_present1OrigLogged = true;
                cs2::FileLog("[ASTRAL] hkPresent1: original Present1 faulted — returning S_OK");
            }
            return S_OK;
        }
    }
    if (g_cachedOrigPresentVtable) {
        __try {
            return g_cachedOrigPresentVtable(swapChain, syncInterval, flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool s_presentOrigLogged1 = false;
            if (!s_presentOrigLogged1) {
                s_presentOrigLogged1 = true;
                cs2::FileLog("[ASTRAL] hkPresent1: original Present faulted — returning S_OK");
            }
            return S_OK;
        }
    }
    return S_OK;
}

// Resolve the per-tick CUserCmd. CS2's CreateMove signature is
// (CCSGOInput* this, int slot, bool active) — the command is not an argument.
// It lives in the 150-entry ring described in include/engine/cusercmd.h.
// Every result is validated before the feature pass is allowed to touch it.
// A command is usable only if the base-command pointer and its view-angle
// pointer are both live. This guards every resolve path below.
static bool IsPlausibleCmd(void* cmd) {
    if (!cmd || (reinterpret_cast<uintptr_t>(cmd) & 0x7) != 0) {
        return false;
    }
    // +0x08 is m_nCommandNumber (int64). Require it to look like a live command
    // number, not garbage.
    std::int64_t commandNumber = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(cmd) + 0x08, commandNumber)) {
        return false;
    }
    if (commandNumber < 0 || commandNumber > 0x7FFFFFFFLL) {
        return false;
    }
    // +0x00 vtable must exist.
    uintptr_t vtable = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(cmd) + 0x00, vtable) ||
        !memory::IsValidPtr(vtable)) {
        return false;
    }
    void* base = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(cmd) + 0x40, base) ||
        !base || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(base))) {
        return false;
    }
    void* viewAngles = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(base) + 0x40, viewAngles) ||
        !viewAngles || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(viewAngles))) {
        return false;
    }
    return true;
}

// Path A (dump-verified, NO unknown-signature function call):
//   ring   = GetUserCmdManager(controller)   — embedded ring of 150 CUserCmd (0x98 each)
//   seq    = *(int32*)(ring + 0x5910)        — current command number (150 * 0x98)
//   cmd    = ring + 0x98 * (seq % 150)       — documented indexing, zero prototype risk
//   cmd    resolved by the game logic, we never call an unknown-prototype fn unless
//           the direct formula fails (then we fall back to GetCUserCmdBySequenceNumber).
static void* ResolveViaManager(void* controller) {
    if (!g_getUserCmdManager) return nullptr;
    __try {
        void* ring = g_getUserCmdManager(controller);
        if (!ring || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(ring))) {
            return nullptr;
        }
        const uintptr_t seqOffsets[2] = { kCmdRingSequenceOffset, 0x59A8 };
        for (int s = 0; s < 2; ++s) {
            int32_t seq = 0;
            if (!memory::Read(reinterpret_cast<uintptr_t>(ring) + seqOffsets[s], seq)) continue;
            if (seq <= 0 || seq > 10000000) continue;

            // Direct ring indexing — safest path, no function-call prototype risk.
            int slot = static_cast<int>(seq % kCmdRingCount);
            uintptr_t cmdAddr = reinterpret_cast<uintptr_t>(ring) +
                                static_cast<uintptr_t>(slot) * kCmdRingStride;
            void* cmd = reinterpret_cast<void*>(cmdAddr);
            if (IsPlausibleCmd(cmd)) {
                return cmd;
            }

            // Fallback: ask the game to resolve by sequence (signature is
            // controller-first per the dump name GetCUserCmdBySequenceNumber).
            if (!g_getCmdBySeq) continue;
            void* resolved = g_getCmdBySeq(controller, static_cast<uint32_t>(seq));
            if (IsPlausibleCmd(resolved)) return resolved;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Path B (community-verified chain): firstArray = *global;
// tick = getCUserCmdTick(controller); arr = getCUserCmdArray(firstArray, tick-1);
// seq = arr->m_nSequenceNumber (0x59A8 / 0x5910); cmd = bySeq(controller, seq).
static void* ResolveViaArrayChain(void* controller) {
    if (!g_cmdArrayGlobal || !g_getCmdArray || !g_getCmdBySeq || !g_getCmdTickOut) return nullptr;
    __try {
        void* firstArray = nullptr;
        if (!memory::Read(g_cmdArrayGlobal, firstArray) || !firstArray) return nullptr;
        int32_t tick = 0;
        g_getCmdTickOut(controller, &tick);
        if (tick <= 0) return nullptr;
        void* arr = g_getCmdArray(firstArray, tick - 1);
        if (IsPlausibleCmd(arr)) return arr;
        if (!arr) return nullptr;
        const uintptr_t seqOffsets[2] = { 0x59A8, kCmdRingSequenceOffset };
        for (int s = 0; s < 2; ++s) {
            int32_t seq = 0;
            if (!memory::Read(reinterpret_cast<uintptr_t>(arr) + seqOffsets[s], seq)) continue;
            if (seq <= 0 || seq > 10000000) continue;
            void* cmd = g_getCmdBySeq(controller, static_cast<uint32_t>(seq));
            if (IsPlausibleCmd(cmd)) return cmd;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Path C: GetCUserCmdArray(this, slot) where the result is the command itself.
static void* ResolveViaArrayDirect(void* pThis, int slot) {
    if (!g_getCmdArray) return nullptr;
    __try {
        void* cmd = g_getCmdArray(pThis, slot);
        if (IsPlausibleCmd(cmd)) return cmd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

static void* ResolveCurrentUserCmd(void* pThis, int slot) {
    void* controller = g_LocalPlayerController;
    const bool haveController = controller &&
        memory::IsValidPtr(reinterpret_cast<uintptr_t>(controller));

    static std::atomic<int> s_mode{-1};   // -1 probing, 0..3 chosen path
    static std::atomic<int> s_fails{0};

    int mode = s_mode.load(std::memory_order_relaxed);

    void* cmd = nullptr;
    if (mode <= 0) {
        int chosen = 0;
        if (haveController) {
            cmd = ResolveViaManager(controller);
            if (cmd) chosen = 1;
        }
        if (!cmd && haveController) {
            cmd = ResolveViaArrayChain(controller);
            if (cmd) chosen = 2;
        }
        if (!cmd) {
            cmd = ResolveViaArrayDirect(pThis, slot);
            if (cmd) chosen = 3;
        }

        if (cmd) {
            s_mode.store(chosen, std::memory_order_relaxed);
            cs2::FileLog("[CM] cmd resolve path=%d cmd=%p baseOk=1", chosen, cmd);
        }
        return cmd;
    }

    if (mode == 1 && haveController) cmd = ResolveViaManager(controller);
    else if (mode == 2 && haveController) cmd = ResolveViaArrayChain(controller);
    else if (mode == 3) cmd = ResolveViaArrayDirect(pThis, slot);

    if (!cmd) {
        if (++s_fails >= 500) {
            s_fails.store(0, std::memory_order_relaxed);
            s_mode.store(-1, std::memory_order_relaxed);
        }
        return nullptr;
    }
    s_fails.store(0, std::memory_order_relaxed);
    return cmd;
}

// Shared by the CreateMove hook (CUserCmd resolved from the ring) and the
// SerializeUserCmd hook (CUserCmd passed as an argument, before serialization).
static void RunFeaturePass(sdk::CUserCmd* userCommand) {
    if (!userCommand) {
        return;
    }

    // NOTE: the tick budget is consumed only AFTER the command passes validation
    // (right before features run) — never on a failed resolve. That way whichever
    // entry point (CreateMove or SerializeUserCmd) has a usable command applies
    // exactly once per tick, and whichever fails does not block the other.
    // Keep game state fresh at ~10 Hz.
    ThrottledRefresh();

    if (g_LocalPlayerController && memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerController))) {
        int tickBase = 0;
        if (memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerController) + sdk::m_nTickBase, tickBase)) {
            Rage::SetTickBase(tickBase);
        }
    }

    sdk::Vector cmdAngles{};
    if (!sdk::GetCmdViewAngles(userCommand, cmdAngles)) {
        // Fall back to the live view angles so movement features (bhop/strafe)
        // still run even when the command's serialized angles can't be read.
        sdk::GetViewAngles(cmdAngles);
    }
    (void)cmdAngles;

    // Deduplicate by command number so the feature pass applies exactly once
    // per command regardless of whether CreateMove or SerializeUserCmd drove
    // this tick. A time-based limiter could skip SerializeUserCmd after
    // CreateMove and silently drop the very command being serialized.
    static std::atomic<int64_t> s_lastCommandNumber{-1};
    int64_t commandNumber = 0;
    if (memory::Read(reinterpret_cast<uintptr_t>(userCommand) + 0x8, commandNumber) &&
        commandNumber > 0) {
        if (commandNumber == s_lastCommandNumber) {
            return; // already applied this command
        }
        s_lastCommandNumber = commandNumber;
    }

    {
        static std::atomic<int> s_feat{0};
        if (++s_feat <= 3) {
            cs2::FileLog("[FEAT] RunFeaturePass applied (call #%d)", s_feat.load());
        }
    }

    Combat::Run(userCommand);
    Misc::Run(userCommand);
    Misc::ApplyAutoDefuse(userCommand);
    Combat::RunTriggerbot(userCommand);

    // Rage bot and anti-aim are opt-in (Config::ragebotEnabled /
    // antiAimEnabled default false). Anti-aim runs last so it owns the
    // final command angles for the tick.
    if (Config::ragebotEnabled) {
        Rage::RunRage(userCommand);
    }
    if (Config::antiAimEnabled) {
        Rage::RunAntiAim(userCommand);
    }
}

bool __fastcall HookManager::hkCreateMove(void* pThis, int slot, bool active) {
    // CRASH-FIX: wrap the ENTIRE hook body in one SEH block.
    __try {
        static std::atomic<int> entryCount{0};
        int n = ++entryCount;
        if (n <= 10 || n % 500 == 0) {
            cs2::FileLog("[CM] ENTRY n=%d pThis=%p slot=%d active=%d", n, pThis, slot, active ? 1 : 0);
        }

        HookManager* manager = g_hooks;
        if (!manager || !manager->origCreateMove) {
            static bool g_loggedNullOriginal = false;
            if (!g_loggedNullOriginal) {
                g_loggedNullOriginal = true;
                cs2::FileLog("[CM] origCreateMove is null — returning false (engine default)");
            }
            return false;
        }

        bool result = false;
        __try {
            // P-2: run movement pre-write (auto-strafe sidemove/view) before the
            // original builds the command, so prediction and the server agree.
            Misc::PreCreateMove();

            result = manager->origCreateMove(pThis, slot, active);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool g_loggedOrigCrash = false;
            if (!g_loggedOrigCrash) {
                g_loggedOrigCrash = true;
                cs2::FileLog("[CM] origCreateMove crashed — SEH caught, returning false");
            }
            return false;
        }

        // F-2 (LOG-DIAG STEP 5): per-100-call tick log proving the hook fires.
        static std::atomic<int> g_cmTickCount{0};
        const int cmTick = ++g_cmTickCount;
        if (cmTick % 100 == 0) {
            cs2::FileLog("[CM] tick=%d slot=%d origCreateMove=%p", cmTick, slot, reinterpret_cast<void*>(manager->origCreateMove));
        }

        // Only process slot 0 — other slots are for spectators, replays, etc.
        if (slot != 0) {
            return result;
        }

        // The feature pass is rate-limited inside RunFeaturePass, so both this
        // hook and the SerializeUserCmd hook may feed it — only one applies per
        // tick. If the command is not built yet at CreateMove time, validation
        // fails here and the SerializeUserCmd hook covers the tick instead.
        void* pCmd = ResolveCurrentUserCmd(pThis, slot);
        if (!pCmd || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(pCmd))) {
            static std::atomic<int> g_cmdMissLog{0};
            const int miss = ++g_cmdMissLog;
            if (miss <= 5 || miss % 2000 == 0) {
                cs2::FileLog("[CM] cmd unresolved (n=%d) — skipping feature pass", miss);
            }
            return result;
        }

        RunFeaturePass(reinterpret_cast<sdk::CUserCmd*>(pCmd));

        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static std::atomic<int> s_cmException{0};
        if (++s_cmException <= 5) {
            cs2::FileLog("CreateMove: exception caught in hook body — returning safe result");
        }
        return false;
    }
}

// Dump RVA 0x962660 — the exact call that turns the live command into the
// bytes handed to the server. Hooking it gives us the CUserCmd for free as an
// argument, avoiding the ring guesswork entirely.
int HookManager::hkSerializeUserCmd(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    __try {
        // The pattern 40 55 56 41 57 48 83 EC 60 8B 4A 20 45 33 FF 44 8B 42 30
        // reads [rdx+0x20] (CCSGOUserCmdPB has_bits) and [rdx+0x30] (input
        // history size) — both exactly CUserCmd+0x20 / +0x30, so rdx is the
        // live CUserCmd. Probe all four register args anyway (one-time log
        // tells us, once and for all, where the command lives on this build).
        const uintptr_t candidates[4] = { a1, a2, a3, a4 };
        sdk::CUserCmd* userCommand = nullptr;
        int cmdArg = -1;
        for (int i = 0; i < 4; ++i) {
            if (IsPlausibleCmd(reinterpret_cast<void*>(candidates[i]))) {
                userCommand = reinterpret_cast<sdk::CUserCmd*>(candidates[i]);
                cmdArg = i + 1;
                break;
            }
        }

        if (userCommand) {
            static std::atomic<bool> s_loggedArg{false};
            if (!s_loggedArg.exchange(true, std::memory_order_relaxed) && cmdArg != 2) {
                cs2::FileLog("[SER] cmd resolved from arg%d this build (not rdx)", cmdArg);
            }
            RunFeaturePass(userCommand);
        } else {
            static std::atomic<int> s_miss{0};
            if (++s_miss <= 3) {
                cs2::FileLog("[SER] arg probe failed ctx=0x%llX cmd=0x%llX r8=0x%llX r9=0x%llX",
                             (unsigned long long)a1, (unsigned long long)a2,
                             (unsigned long long)a3, (unsigned long long)a4);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static std::atomic<int> s_exc{0};
        if (++s_exc <= 3) {
            cs2::FileLog("[SER] exception in hook — bypassing");
        }
    }

    HookManager* manager = g_hooks;
    if (manager && manager->origSerializeUserCmd) {
        return manager->origSerializeUserCmd(a1, a2, a3, a4);
    }
    return 0;
}

bool HookManager::EnsureRenderTarget(IDXGISwapChain* swapChain) {
    if (!device || !swapChain) {
        return false;
    }

    // PERF: GetBuffer+GetDesc+Release every frame is two COM round-trips with
    // refcount churn. If we already hold a valid RTV, skip the checks and only
    // revalidate periodically (backbuffer size could change on a resize).
    if (renderTargetView) {
        static uint32_t s_revalidateTick = 0;
        if (++s_revalidateTick < 240) { // ~4 s at 60 FPS
            return true;
        }
        s_revalidateTick = 0;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    if (!renderTargetView || desc.Width != renderTargetWidth || desc.Height != renderTargetHeight) {
        if (renderTargetView) {
            renderTargetView->Release();
            renderTargetView = nullptr;
        }

        if (FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView))) {
            backBuffer->Release();
            return false;
        }
        renderTargetWidth = desc.Width;
        renderTargetHeight = desc.Height;
    }

    backBuffer->Release();
    return true;
}

bool HookManager::InitializeImGui(IDXGISwapChain* swapChain) {
    RawLog("[ASTRAL] InitializeImGui: start");
    cs2::FileLog("InitializeImGui: start");
    if (!swapChain) {
        RawLog("[ASTRAL] InitializeImGui: swapChain null");
        cs2::FileLog("InitializeImGui: swapChain null");
        return false;
    }

    RawLog("[ASTRAL] InitializeImGui: getting D3D11 device");
    ID3D11Device* dev = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) {
        RawLog("[ASTRAL] InitializeImGui: no D3D11 device");
        cs2::FileLog("InitializeImGui: no D3D11 device");
        cs2::FileLog("ImGui init failed: no D3D11 device");
        return false;
    }
    RawLog("[ASTRAL] InitializeImGui: device obtained");

    device = dev;
    device->GetImmediateContext(&context);
    if (!context) {
        RawLog("[ASTRAL] InitializeImGui: no immediate context");
        cs2::FileLog("InitializeImGui: no immediate context");
        cs2::FileLog("ImGui init failed: no immediate context");
        device->Release();
        device = nullptr;
        return false;
    }
    RawLog("[ASTRAL] InitializeImGui: context obtained");

    RawLog("[ASTRAL] InitializeImGui: creating ImGui context");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    RawLog("[ASTRAL] InitializeImGui: context created");

    RawLog("[ASTRAL] InitializeImGui: style setup");
    ImGui::StyleColorsDark();
    theme::Apply();
    RawLog("[ASTRAL] InitializeImGui: style done");

    RawLog("[ASTRAL] InitializeImGui: loading fonts");
    theme::LoadFonts();
    RawLog("[ASTRAL] InitializeImGui: fonts done");

    RawLog("[ASTRAL] InitializeImGui: finding game window");
    hwnd = FindGameWindow();
    if (hwnd) {
        RawLog("[ASTRAL] InitializeImGui: window found via FindGameWindow");
    }
    if (!hwnd) {
        RawLog("[ASTRAL] InitializeImGui: FindGameWindow returned null, trying FindWindowA");
        hwnd = FindWindowA(nullptr, "Counter-Strike 2");
        if (hwnd) {
            RawLog("[ASTRAL] InitializeImGui: window found via FindWindowA");
        }
    }
    if (!hwnd) {
        RawLog("[ASTRAL] InitializeImGui: FindWindowA returned null, trying GetActiveWindow");
        hwnd = GetActiveWindow();
        if (hwnd) {
            RawLog("[ASTRAL] InitializeImGui: window found via GetActiveWindow");
        }
    }
    char hwndBuf[64];
    snprintf(hwndBuf, sizeof(hwndBuf), "[ASTRAL] InitializeImGui: hwnd=%p", hwnd);
    RawLog(hwndBuf);
    this->hwnd = hwnd;

    RawLog("[ASTRAL] InitializeImGui: initializing Win32 backend");
    bool win32Ok = false;
    if (hwnd) {
        RawLog("[ASTRAL] InitializeImGui: calling ImGui_ImplWin32_Init");
        if (!ImGui_ImplWin32_Init(hwnd)) {
            RawLog("[ASTRAL] InitializeImGui: Win32 init failed");
            cs2::FileLog("ImGui Win32 backend init failed");
        } else {
            RawLog("[ASTRAL] InitializeImGui: calling SetWindowLongPtrA for WndProc");
            origWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
            RawLog("[ASTRAL] InitializeImGui: WndProc hooked");
            win32Ok = true;
        }
    } else {
        RawLog("[ASTRAL] InitializeImGui: hwnd is null — ImGui will stay disabled");
    }

    RawLog("[ASTRAL] InitializeImGui: calling ImGui_ImplDX11_Init");
    if (!ImGui_ImplDX11_Init(device, context)) {
        RawLog("[ASTRAL] InitializeImGui: DX11 init failed");
        cs2::FileLog("ImGui DX11 backend init failed");
        if (win32Ok && origWndProc) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origWndProc));
            origWndProc = nullptr;
        }
        if (win32Ok) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        context->Release();
        context = nullptr;
        device->Release();
        device = nullptr;
        this->hwnd = nullptr;
        ImGui::SetCurrentContext(nullptr);
        return false;
    }
    RawLog("[ASTRAL] InitializeImGui: DX11 init OK");
    cs2::FileLog("InitializeImGui: SUCCESS - imgui initialized");

    // Create RTV from the REAL backbuffer immediately
    ID3D11Texture2D* backbuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) && backbuffer) {
        if (renderTargetView) { renderTargetView->Release(); renderTargetView = nullptr; }
        device->CreateRenderTargetView(backbuffer, nullptr, &renderTargetView);
        backbuffer->Release();
        RawLog("[ASTRAL] InitializeImGui: RTV created from real backbuffer");
    } else {
        RawLog("[ASTRAL] InitializeImGui: GetBuffer failed for RTV");
    }

    imguiInitialized = true;
    cs2::FileLog("ImGui init OK hwnd=%p device=%p context=%p", hwnd, device, context);
    return true;
}

// SEH-wrapped feature pass for RenderFrame — separate function so __try/__except
// doesn't conflict with try/catch or C++ unwind objects in RenderFrame.
static void RenderFeaturePass(IDXGISwapChain* swapChain) {
    __try {
        ThrottledRefresh();
        GameInfo::Refresh();
        if (g_LocalPlayerPawn && memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
            Hooks::RunWorldHooks();
        }
        Misc::ApplyAutoAccept();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_featurePassExceptionLogged = false;
        if (!s_featurePassExceptionLogged) {
            s_featurePassExceptionLogged = true;
            cs2::FileLog("RenderFrame: exception in feature pass — caught");
        }
    }
    (void)swapChain;
}

void HookManager::RenderFrame(IDXGISwapChain* swapChain) {
    if (!g_hooks || !g_hooks->initialized) {
        return;
    }

    static bool rendering = false;
    if (rendering) {
        return;
    }
    int currentFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
    static int lastRenderedFrame = -1;
    if (lastRenderedFrame >= 0 && currentFrame == lastRenderedFrame) {
        return;
    }
    rendering = true;

    struct RenderGuard {
        bool& flag;
        RenderGuard(bool& f) : flag(f) {}
        ~RenderGuard() { flag = false; }
    } guard(rendering);

    ToggleMenuHotkey();

    if (!imguiInitialized) {
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        try {
            InitializeImGui(swapChain);
        } catch (...) {
            RawLog("[ASTRAL] RenderFrame: EXCEPTION in InitializeImGui");
        }
    }

    RenderFeaturePass(swapChain);
    {
        static bool s_featurePassLogged = false;
        if (!s_featurePassLogged) {
            s_featurePassLogged = true;
            cs2::FileLog("[FR] feature pass returned OK");
        }
    }

    // Never enter the ImGui pass without a valid context: NewFrame/GetIO would
    // assert and abort (not SEH-catchable) if initialization failed.
    if (imguiInitialized && ImGui::GetCurrentContext()) {
        RenderImGuiPass(this, swapChain);
        {
            static std::atomic<int> s_imguiPassCount{0};
            if (++s_imguiPassCount <= 3) {
                cs2::FileLog("[FR] RenderImGuiPass returned OK (call #%d)", s_imguiPassCount.load());
            }
        }
        lastRenderedFrame = currentFrame;
    }
}

// SEH-wrapped ImGui rendering pass — separate function so __try/__except
// doesn't conflict with try/catch blocks in RenderFrame.
void HookManager::RenderImGuiPass(HookManager* self, IDXGISwapChain* swapChain) {
    // Serialize the frame with the WndProc hook (see g_imguiMutex). Manual
    // lock/unlock because __try cannot coexist with C++ unwind objects here.
    g_imguiMutex.lock();

    bool frameBegun = false;
    bool frameRendered = false;
    __try {
        cs2::FileLog("[FR] ImGuiPass: mutex locked");

        self->EnsureRenderTarget(swapChain);
        cs2::FileLog("[FR] ImGuiPass: EnsureRenderTarget done");

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = menu::g_ShowMenu;
        cs2::FileLog("[FR] ImGuiPass: GetIO done");

        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        frameBegun = true;
        cs2::FileLog("[FR] ImGuiPass: NewFrame done");

        g_ScreenWidth = static_cast<int>(io.DisplaySize.x);
        g_ScreenHeight = static_cast<int>(io.DisplaySize.y);
        if (g_ScreenWidth <= 0) g_ScreenWidth = 1920;
        if (g_ScreenHeight <= 0) g_ScreenHeight = 1080;

        const bool matrixReady = (g_ViewMatrix.m[3][3] != 0.0f);
        if (matrixReady) {
            cs2::FileLog("[FR] ImGuiPass: matrix ready, drawing visuals");
            Visuals::Draw();
            cs2::FileLog("[FR] ImGuiPass: Visuals::Draw done");
            menu::DrawSpectatorList();
            cs2::FileLog("[FR] ImGuiPass: DrawSpectatorList done");

            if (Config::aimbotEnabled && !menu::g_ShowMenu) {
                ImDrawList* drawList = ImGui::GetBackgroundDrawList();
                if (drawList) {
                    ImVec2 displaySize = io.DisplaySize;
                    if (displaySize.x > 0 && displaySize.y > 0) {
                        ImVec2 screenCenter(displaySize.x * 0.5f, displaySize.y * 0.5f);
                        float fovRad = Config::aimFov * 0.5f * 3.14159265f / 180.0f;
                        float radius = std::tan(fovRad) * screenCenter.y;
                        drawList->AddCircle(screenCenter, radius, IM_COL32(0, 229, 184, 180), 64, 1.5f);
                    }
                }
            }
        }

        // The menu must be usable at the main menu (before the view matrix is
        // populated), so it is not gated on matrixReady.
        if (menu::g_ShowMenu) {
            cs2::FileLog("[FR] ImGuiPass: rendering menu");
            menu::Render();
            cs2::FileLog("[FR] ImGuiPass: menu::Render done");
        }

        // ALWAYS call Render after NewFrame. Skipping it leaves the ImGui frame
        // open and asserts/crashes on the next NewFrame. GetDrawData() below is
        // only valid once Render() has run.
        ImGui::Render();
        frameRendered = true;
        cs2::FileLog("[FR] ImGuiPass: ImGui::Render done");

        {
            static std::atomic<int> s_frames{0};
            int n = ++s_frames;
            if ((n % 120) == 0) {
                cs2::FileLog("[FR] heartbeat frame #%d menu=%d", n, (int)menu::g_ShowMenu);
            }
        }

        if (self->renderTargetView && self->context && self->device) {
            // Save the game's D3D11 state (render targets, depth-stencil,
            // viewports) and restore it afterwards. Binding our RTV and never
            // restoring corrupts the game's pipeline state and crashes the very
            // next frame.
            ID3D11RenderTargetView* savedRTV = nullptr;
            ID3D11DepthStencilView* savedDSV = nullptr;
            UINT savedNumViewports = 1;
            D3D11_VIEWPORT savedViewports[8] = {};
            cs2::FileLog("[FR] D3D: before OMGetRenderTargets");
            self->context->OMGetRenderTargets(1, &savedRTV, &savedDSV);
            cs2::FileLog("[FR] D3D: before RSGetViewports");
            self->context->RSGetViewports(&savedNumViewports, savedViewports);

            cs2::FileLog("[FR] D3D: before OMSetRenderTargets");
            self->context->OMSetRenderTargets(1, &self->renderTargetView, nullptr);
            cs2::FileLog("[FR] D3D: before ImGui_ImplDX11_RenderDrawData");
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            cs2::FileLog("[FR] D3D: ImGui_ImplDX11_RenderDrawData OK");

            if (savedNumViewports > 0 && savedNumViewports <= 8) {
                self->context->RSSetViewports(savedNumViewports, savedViewports);
            }
            cs2::FileLog("[FR] D3D: before OMSetRenderTargets restore");
            self->context->OMSetRenderTargets(1, &savedRTV, savedDSV);
            if (savedRTV) savedRTV->Release();
            if (savedDSV) savedDSV->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_exceptionLogged = false;
        if (!s_exceptionLogged) {
            s_exceptionLogged = true;
            cs2::FileLog("RenderImGuiPass: exception caught — returning safe result");
            BootLog("RenderImGuiPass: exception caught — returning safe result");
        }
        // If the frame was opened but never closed, close it so the next
        // NewFrame() does not assert ("Forgot to call Render() or EndFrame()").
        if (frameBegun && !frameRendered && ImGui::GetCurrentContext()) {
            __try {
                ImGui::EndFrame();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }

    g_imguiMutex.unlock();
}

bool HookManager::InstallCreateMove() {
    char buf[256];
    RawLog("[ASTRAL] InstallCreateMove: start");
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        RawLog("[ASTRAL] InstallCreateMove: client.dll missing");
        cs2::FileLog("CreateMove: client.dll missing");
        return false;
    }
    RawLog("[ASTRAL] InstallCreateMove: client.dll found");

    uintptr_t clientBase = reinterpret_cast<uintptr_t>(client);
    size_t imageSize = GetModuleImageSize(client);
    if (!imageSize) {
        RawLog("[ASTRAL] InstallCreateMove: image size unavailable");
        cs2::FileLog("CreateMove: client image size unavailable");
        return false;
    }
    RawLog("[ASTRAL] InstallCreateMove: imageSize obtained");

    uintptr_t cmdArrayPattern = memory::FindPattern(client, kCmdArrayPattern);
    uintptr_t cmdBySeqPattern = memory::FindPattern(client, kCmdBySeqPattern);
    uintptr_t cmdTickPattern = memory::FindPattern(client, kCmdTickPattern);
    uintptr_t cmdGlobalPattern = memory::FindPattern(client, kCmdArrayGlobalPattern);
    snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: patterns scanned array=%p byseq=%p tick=%p global=%p",
             reinterpret_cast<void*>(cmdArrayPattern), reinterpret_cast<void*>(cmdBySeqPattern),
             reinterpret_cast<void*>(cmdTickPattern), reinterpret_cast<void*>(cmdGlobalPattern));
    RawLog(buf);

    if (cmdArrayPattern) {
        g_getCmdArray = reinterpret_cast<GetCmdArray_t>(cmdArrayPattern);
    }
    if (cmdBySeqPattern) {
        g_getCmdBySeq = reinterpret_cast<GetCmdBySeq_t>(cmdBySeqPattern);
    }
    if (cmdTickPattern) {
        g_getCmdTick = reinterpret_cast<GetCmdTick_t>(cmdTickPattern);
    }
    if (cmdGlobalPattern) {
        g_cmdArrayGlobal = memory::ResolveRipRelative(cmdGlobalPattern, 3, 7);
    }

    // CUserCmd ring manager (include/engine/cusercmd.h kGetUserCmdManager_rva).
    uintptr_t cmdMgrPattern = memory::FindPattern(client, kUserCmdManagerPattern);
    if (cmdMgrPattern) {
        g_getUserCmdManager = reinterpret_cast<GetUserCmdManager_t>(cmdMgrPattern);
        snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: usercmd manager=%p", reinterpret_cast<void*>(cmdMgrPattern));
        RawLog(buf);
    } else {
        cs2::FileLog("CreateMove: usercmd manager pattern unresolved (ring path disabled)");
    }

    // getCUserCmdTick(controller, &tick) — used by the array-chain resolve path.
    uintptr_t cmdTickOutPattern = memory::FindPattern(client, kCmdTickOutPattern);
    if (cmdTickOutPattern) {
        g_getCmdTickOut = reinterpret_cast<GetCmdTickOut_t>(cmdTickOutPattern);
        snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: usercmd tick=%p", reinterpret_cast<void*>(cmdTickOutPattern));
        RawLog(buf);
    } else {
        cs2::FileLog("CreateMove: usercmd tick pattern unresolved (array-chain path disabled)");
    }

    if (!g_cmdArrayGlobal) {
        // Non-fatal: we only use this for diagnostics; the ring path does not need it.
        cs2::FileLog("CreateMove: command array global unresolved (continuing)");
    }

    // P3-7: the dump tells us the CCSGOInput instance RVA (0x2580A10) — use it directly.
    sdk::g_CSGOInputRVA = static_cast<uintptr_t>(sdk::dwCSGOInput);
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: CCSGOInput RVA=0x%llX (dump=0x2580A10)",
                 (unsigned long long)sdk::g_CSGOInputRVA);
        RawLog(buf);
    }

    // --- Resolve CreateMove target ---
    // CCSGOInput::CreateMove is vtable index 5. Hooking the function body at its
    // dump RVA intercepts exactly the callee the vtable dispatches to, so we
    // hook the RVA directly and use the vtable only as a cross-check.
    //
    // NOTE: CCSGOInput is an EMBEDDED object at client + kInstance_rva, so its
    // first pointer IS the vftable. The previous implementation dereferenced it
    // twice and read a data pointer as if it were the vtable.
    uintptr_t target = 0;
    {
        uintptr_t vtable = 0;
        if (memory::Read(clientBase + sdk::dwCSGOInput, vtable) && memory::IsValidPtr(vtable)) {
            snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: input object vtable=0x%llX", (unsigned long long)vtable);
            RawLog(buf);
            for (int i = 0; i < 64; ++i) {
                uintptr_t entry = 0;
                if (!memory::Read(vtable + static_cast<uintptr_t>(i) * sizeof(uintptr_t), entry)) {
                    break;
                }
                if (entry == clientBase + 0xB676C0) {
                    snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: vtable[%d] == CreateMove RVA 0xB676C0", i);
                    RawLog(buf);
                    break;
                }
            }
        } else {
            cs2::FileLog("CreateMove: vtable cross-check failed (hooking RVA anyway)");
        }

        uintptr_t dumpRVA = clientBase + 0xB676C0;
        if (dumpRVA >= clientBase && dumpRVA < clientBase + imageSize) {
            target = dumpRVA;
            snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: using dump CreateMove RVA target=0x%llX", (unsigned long long)target);
            RawLog(buf);
        }
    }

    // Fallback: pattern scan
    if (!target) {
        const char* kCreateMovePatterns[] = {
            "85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40 18 89",
            "85 D2 0F 85 ? ? ? ? 48 8B C4",
            "48 8B C4 44 88 40 18 89",
            "85 D2 75 ? 48 8B C4 44 88 40 18",
        };
        for (const char* pat : kCreateMovePatterns) {
            uintptr_t scanned = memory::FindPattern(client, pat, false);
            if (scanned && scanned >= clientBase && scanned < clientBase + imageSize) {
                target = scanned;
                snprintf(buf, sizeof(buf), "[ASTRAL] InstallCreateMove: CreateMove pattern scan resolved target=0x%llX using pattern '%s'",
                    (unsigned long long)target, pat);
                RawLog(buf);
                break;
            }
        }
    }

    if (!target) {
        RawLog("[ASTRAL] InstallCreateMove: CreateMove target NOT found — all strategies failed");
        cs2::FileLog("CreateMove: could not resolve target address");
        return false;
    }

    cs2::FileLog("[ASTRAL] CreateMove target resolved: %p (clientBase: %p)", target, client);

    LPVOID cmTarget = reinterpret_cast<LPVOID>(target);

    LPVOID cmTrampoline = nullptr;
    MH_STATUS status = MH_CreateHook(cmTarget, reinterpret_cast<LPVOID>(&hkCreateMove), &cmTrampoline);

    if (status == MH_OK) {
        origCreateMove = reinterpret_cast<CreateMove_t>(cmTrampoline);
        MH_EnableHook(cmTarget);
        createMoveTarget = cmTarget;
        cs2::FileLog("CreateMove hook OK target=%p array=%p byseq=%p tick=%p global=%p",
                 cmTarget,
                 reinterpret_cast<void*>(g_getCmdArray), reinterpret_cast<void*>(g_getCmdBySeq),
                 reinterpret_cast<void*>(g_getCmdTick), reinterpret_cast<void*>(g_cmdArrayGlobal));
        RawLog("[ASTRAL] InstallCreateMove: hook OK, returning true");
        return true;
    } else if (status == MH_ERROR_ALREADY_CREATED) {
        // CRASH-FIX: Cause 4 — same policy as the D3D hooks: never disable a
        // CreateMove detour we did not install.
        cs2::FileLog("[ASTRAL] CreateMove already hooked by someone else — leaving original alone; combat features disabled");
        cs2::FileLog("[HOOK] CreateMove already hooked (MH_ERROR_ALREADY_CREATED) — skipping");
        return false;
    }
    cs2::FileLog("CreateMove MH_CreateHook failed status=%d target=%p", (int)status, cmTarget);
    return false;
}

// SerializeUserCmd receives the live CUserCmd as an argument and turns it into
// the bytes sent to the server. Hooking it gives us the command directly, with
// zero ring-resolution guesswork. Feature writes happen BEFORE the original
// serializes, so the server consumes our angles/buttons. Independent of the
// CreateMove hook — if this install fails the ring path still works.
bool HookManager::InstallSerializeUserCmd() {
    RawLog("[ASTRAL] InstallSerializeUserCmd: start");
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        RawLog("[ASTRAL] InstallSerializeUserCmd: client.dll missing");
        return false;
    }

    char buf[256];
    uintptr_t clientBase = reinterpret_cast<uintptr_t>(client);
    size_t imageSize = GetModuleImageSize(client);

    uintptr_t target = 0;
    uintptr_t pattern = memory::FindPattern(client, kSerializeUserCmdPattern);
    if (pattern && pattern >= clientBase && (pattern - clientBase) < imageSize) {
        target = pattern;
        snprintf(buf, sizeof(buf), "[ASTRAL] InstallSerializeUserCmd: pattern resolved target=0x%llX",
                 (unsigned long long)target);
        RawLog(buf);
    } else {
        uintptr_t rva = clientBase + kSerializeUserCmdRVA;
        if (rva >= clientBase && (rva - clientBase) < imageSize) {
            target = rva;
            cs2::FileLog("[SER] pattern scan failed — using dump RVA 0x962660 target=%p",
                         reinterpret_cast<void*>(target));
        }
    }

    if (!target) {
        cs2::FileLog("[SER] target unresolved — SerializeUserCmd hook not installed");
        return false;
    }

    MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target),
                                     reinterpret_cast<LPVOID>(&hkSerializeUserCmd),
                                     reinterpret_cast<LPVOID*>(&origSerializeUserCmd));
    if (status != MH_OK) {
        cs2::FileLog("[SER] MH_CreateHook failed status=%d target=%p", (int)status,
                     reinterpret_cast<void*>(target));
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<LPVOID>(target)) != MH_OK) {
        cs2::FileLog("[SER] MH_EnableHook failed target=%p", reinterpret_cast<void*>(target));
        return false;
    }

    serializeTarget = reinterpret_cast<void*>(target);
    cs2::FileLog("[SER] SerializeUserCmd hook OK target=%p — feature pass uses live cmd", reinterpret_cast<void*>(target));
    RawLog("[ASTRAL] InstallSerializeUserCmd: hook OK, returning true");
    return true;
}

bool HookManager::Initialize() {
    BootLog("[HOOK] Initialize: enter");
    InitializeCriticalSection(&g_initCS);
    BootLog("[HOOK] Initialize: critsec init");
    EnterCriticalSection(&g_initCS);
    BootLog("[HOOK] Initialize: critsec entered");
    g_hooks = this;

    // CRASH-FIX: Cause 6 — log MH_Initialize and log every hook status; a
    // non-OK MH_Initialize aborts before any hook is installed.
    BootLog("[HOOK] Initialize: calling MH_Initialize");
    MH_STATUS mhStatus = MH_Initialize();
    BootLog("[HOOK] MH_Initialize status=%d (%s)", (int)mhStatus, MH_StatusToString(mhStatus));
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        BootLog("[HOOK] MH_Initialize FAILED status=%d (%s) — aborting", (int)mhStatus, MH_StatusToString(mhStatus));
        LeaveCriticalSection(&g_initCS);
        return false;
    }

    HMODULE client = GetModuleHandleA("client.dll");
    BootLog("[HOOK] Initialize: client.dll handle=%p", (void*)client);
    if (!client) {
        BootLog("[HOOK] Initialize: client.dll missing");
        LeaveCriticalSection(&g_initCS);
        return false;
    }
    BootLog("[HOOK] Initialize: client.dll found");

    BootLog("[HOOK] Initialize: calling Game::Refresh");
    Game::Refresh();
    BootLog("[HOOK] Initialize: Game::Refresh done");

    {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "astral_dummy_wnd";
        RegisterClassExA(&wc);
        HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP,
                                         0, 0, 1, 1, nullptr, nullptr,
                                         wc.hInstance, nullptr);
        cs2::FileLog("Dummy window created hwnd=%p", dummyHwnd);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width = 1;
        sd.BufferDesc.Height = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = dummyHwnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        IDXGISwapChain* swap = nullptr;

        BootLog("[HOOK] Initialize: calling D3D11CreateDeviceAndSwapChain");
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &featureLevel, 1, D3D11_SDK_VERSION, &sd, &swap,
            &dev, nullptr, &ctx
        );
        BootLog("[HOOK] Initialize: D3D11CreateDeviceAndSwapChain hr=%08x swap=%p dev=%p", hr, (void*)swap, (void*)dev);

        if (FAILED(hr) || !swap) {
            BootLog("[HOOK] Initialize: D3D11CreateDeviceAndSwapChain failed");
            if (swap) swap->Release();
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            if (dummyHwnd) DestroyWindow(dummyHwnd);
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            LeaveCriticalSection(&g_initCS);
            return false;
        }

        BootLog("[HOOK] Initialize: reading swap chain vtable");
        void** vtable = *reinterpret_cast<void***>(swap);
        BootLog("[HOOK] Initialize: vtable=%p vtable[8]=%p vtable[22]=%p",
                 vtable, vtable ? vtable[8] : nullptr, vtable ? vtable[22] : nullptr);

        BootLog("[HOOK] Initialize: calling MH_CreateHook Present");
        Present_t presentAddress = reinterpret_cast<Present_t>(vtable[8]);
        Present_t trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<void*>(presentAddress),
            reinterpret_cast<LPVOID>(&hkPresent),
            reinterpret_cast<LPVOID*>(&trampoline));
        BootLog("[HOOK] Initialize: MH_CreateHook Present status=%d trampoline=%p", (int)status, (void*)trampoline);

        bool presentOk = false;
        if (status == MH_OK) {
            origPresent = trampoline;
            BootLog("[HOOK] Initialize: calling MH_EnableHook Present");
            MH_EnableHook(reinterpret_cast<void*>(presentAddress));
            presentTarget = reinterpret_cast<void*>(presentAddress);
            g_cachedOrigPresentVtable = trampoline;
            BootLog("[HOOK] Initialize: PresentHook enabled origPresent=%p", (void*)trampoline);
            presentOk = true;
        } else if (status == MH_ERROR_ALREADY_CREATED) {
            BootLog("[HOOK] Initialize: PresentHook already hooked — skipping");
            presentOk = false;
        } else {
            BootLog("[HOOK] Initialize: PresentHook failed status=%d", (int)status);
        }
        BootLog("[HOOK] Initialize: PresentHook enabled=%d", (int)presentOk);

        if (vtable[22]) {
            BootLog("[HOOK] Initialize: calling MH_CreateHook Present1");
            Present1_t present1Address = reinterpret_cast<Present1_t>(vtable[22]);
            Present1_t trampoline1 = nullptr;
            status = MH_CreateHook(
                reinterpret_cast<void*>(present1Address), reinterpret_cast<LPVOID>(&hkPresent1),
                reinterpret_cast<LPVOID*>(&trampoline1));
            BootLog("[HOOK] Initialize: MH_CreateHook Present1 status=%d trampoline=%p", (int)status, (void*)trampoline1);
            if (status == MH_OK) {
                origPresent1 = trampoline1;
                MH_EnableHook(reinterpret_cast<void*>(present1Address));
                present1Target = reinterpret_cast<void*>(present1Address);
                g_cachedOrigPresent1Vtable = trampoline1;
            } else if (status == MH_ERROR_ALREADY_CREATED) {
                BootLog("[HOOK] Initialize: Present1Hook already hooked — skipping");
            }
        }

        if (!presentOk) {
            BootLog("[HOOK] Initialize: CRITICAL Present hook failed — no rendering");
        }

        if (swap) swap->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        if (dummyHwnd) DestroyWindow(dummyHwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
    }

    BootLog("[HOOK] Initialize: calling InstallCreateMove");
    bool createMoveOk = InstallCreateMove();
    BootLog("[HOOK] Initialize: InstallCreateMove returned %d", (int)createMoveOk);
    if (!createMoveOk) {
        BootLog("[HOOK] Initialize: CreateMove hook failed — combat disabled");
    }

    BootLog("[HOOK] Initialize: calling InstallSerializeUserCmd");
    bool serializeOk = InstallSerializeUserCmd();
    BootLog("[HOOK] Initialize: InstallSerializeUserCmd returned %d", (int)serializeOk);

    BootLog("[HOOK] Initialize: calling Misc::InstallHooks");
    Misc::InstallHooks();
    BootLog("[HOOK] Initialize: Misc::InstallHooks done");

    initialized = true;
    BootLog("[HOOK] Initialize: complete, returning true");
    LeaveCriticalSection(&g_initCS);
    return true;
}

void HookManager::Shutdown() {
    if (serializeTarget) {
        MH_DisableHook(serializeTarget);
        MH_RemoveHook(serializeTarget);
        serializeTarget = nullptr;
        origSerializeUserCmd = nullptr;
    }

    if (createMoveTarget) {
        MH_DisableHook(createMoveTarget);
        MH_RemoveHook(createMoveTarget);
        createMoveTarget = nullptr;
        origCreateMove = nullptr;
    }

    if (presentTarget) {
        MH_DisableHook(presentTarget);
        MH_RemoveHook(presentTarget);
        presentTarget = nullptr;
    }
    if (present1Target) {
        MH_DisableHook(present1Target);
        MH_RemoveHook(present1Target);
        present1Target = nullptr;
    }

    origPresent = nullptr;
    origPresent1 = nullptr;
    g_cachedOrigPresentVtable = nullptr;
    g_cachedOrigPresent1Vtable = nullptr;

    if (origWndProc && hwnd) {
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origWndProc));
        origWndProc = nullptr;
    }

    hwnd = nullptr;

    if (imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }

    if (renderTargetView) {
        renderTargetView->Release();
        renderTargetView = nullptr;
    }
    if (context) {
        context->Release();
        context = nullptr;
    }
    if (device) {
        device->Release();
        device = nullptr;
    }

    MH_Uninitialize();
    initialized = false;
}

} // namespace hooks
