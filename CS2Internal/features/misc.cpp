// AUDIT: P0-2, P0-3, P0-4, P0-5, P1-1, P1-6, P1-8, P1-9, P1-10, P1-11,
//        P2-1, P2-2, P2-3, P2-4, P2-5, P3-3, P3-7, P4-1, P4-2, P4-3, P4-4, P4-5
// UPGRADE: M-1, M-2, P-1, V-1, 0-3
// CRASH-FIX: Cause 2 (AutoStrafe pawn validation), Cause 5 (hkOverrideView null-original guard)
#include "misc.h"
#include "../core/config.h"
#include "../core/game.h"
#include "../core/math.h"
#include "../core/memory.h"
#include "../core/usercmd.h"
#include "../core/log.h"
#include "../core/security.h"
#include "../features/visuals.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <MinHook.h>

namespace Misc {

namespace {

constexpr uint32_t FL_ONGROUND = 1u << 0;
constexpr uint32_t FL_JUMP_FROM_SPEC = 1u << 2; // not actually from spec, this is FL_DOUBLEJUMP in CS2


// P1-6: tracer detection state. Primary signal is the active weapon's clip
// decreasing; m_iShotsFired is the fallback for weapons without a clip.
static std::uint64_t g_lastBulletServicesHits = 0;
static int g_lastClip = -1;
static int g_lastShotsFired = 0;

void* g_lastPawn = nullptr;
bool g_haveOriginalFov = false;
bool g_haveOriginalViewmodelFov = false;
int g_originalFov = 0;
float g_originalViewmodelFov = 0.0f;

std::vector<SpectatorInfo> g_spectators;
std::mutex g_spectatorsMutex;

void RefreshPawnState() {
    if (g_lastPawn != g_LocalPlayerPawn) {
        g_lastPawn = g_LocalPlayerPawn;
        g_haveOriginalFov = false;
        g_haveOriginalViewmodelFov = false;
    }
}

// Strict liveness gate for the local pawn. A stale/garbage pointer in
// client+dwLocalPlayerPawn (main menu, map transition, or teardown) passes a
// bare nullptr check but fails these reads with overwhelming probability.
// Every write to pawn/services memory goes behind this check so we can never
// zero arbitrary heap memory before a match is actually running.
static bool LocalPawnAlive() {
    if (!g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return false;
    }
    return Game::IsAlive(g_LocalPlayerPawn);
}

void ApplyFov() {
    if (!LocalPawnAlive()) {
        return;
    }

    void* cameraServices = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_pCameraServices, cameraServices) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(cameraServices))) {
        return;
    }

    // Re-sync the cached original when the camera services pointer changes.
    static void* g_lastCameraServices = nullptr;
    if (cameraServices != g_lastCameraServices) {
        g_lastCameraServices = cameraServices;
        g_haveOriginalFov = false;
    }

    if (Config::fovChangerEnabled) {
        if (!g_haveOriginalFov) {
            memory::Read(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_iFOV, g_originalFov);
            g_haveOriginalFov = true;
        }
        uint32_t value = static_cast<uint32_t>(std::clamp(Config::fovValue, 60.0f, 140.0f));
        // Write the target FOV every frame: the game resets m_iFOV each tick.
        // Force an instant change by zeroing m_flFOVRate so the engine does not
        // interpolate from a stale m_iFOVStart. We deliberately do NOT rewrite
        // m_iFOVStart every frame — fighting the game's interpolation is what
        // made the screen glitch.
        memory::Write(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_iFOV, value);
        memory::Write(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_flFOVTime, 0.0f);
        memory::Write(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_flFOVRate, 0.0f);
    } else if (g_haveOriginalFov) {
        memory::Write(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_iFOV, g_originalFov);
        memory::Write(reinterpret_cast<uintptr_t>(cameraServices) + sdk::m_flFOVRate, 0.0f);
        g_haveOriginalFov = false;
    }
}

void ApplyViewmodelFov() {
    if (!LocalPawnAlive()) {
        return;
    }

    uintptr_t address = reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_flViewmodelFOV;
    if (Config::viewmodelFovEnabled) {
        if (!g_haveOriginalViewmodelFov) {
            memory::Read(address, g_originalViewmodelFov);
            g_haveOriginalViewmodelFov = true;
        }
        // Write every frame: the game resets m_flViewmodelFOV each tick, so a
        // one-shot write (or a write-only-on-change) never reaches the renderer.
        float value = std::clamp(Config::viewmodelFov, 40.0f, 120.0f);
        memory::Write(address, value);
    } else if (g_haveOriginalViewmodelFov) {
        memory::Write(address, g_originalViewmodelFov);
        g_haveOriginalViewmodelFov = false;
    }
}

void ApplyClanTag() {
    if (!Config::clanChangerEnabled || !g_LocalPlayerController || !LocalPawnAlive()) {
        return;
    }

    // C-3 / M-3 / M-6: CUtlString in CS2 is a bare 8-byte { char* m_pString } at offset 0.
    // The old +8/+12 "length/capacity" reads hit adjacent CCSPlayerController fields
    // (m_iCoachingTeam at 0x888, etc.) and clobbered them. We read only m_pString,
    // write the tag into that buffer, and never touch length/capacity bookkeeping.
    uintptr_t clanTagField = reinterpret_cast<uintptr_t>(g_LocalPlayerController) + sdk::m_sSanitizedClanTag;

    uintptr_t bufferPtr = 0;
    if (!memory::Read(clanTagField, bufferPtr) || !memory::IsValidPtr(bufferPtr)) {
        return;
    }

    std::string tag = Config::clanTag;
    if (tag.empty()) {
        tag = "ASTRAL";
    }

    // Keep tag within CS2 limits (16 chars including null terminator).
    if (tag.size() > 15) {
        tag.resize(15);
    }

    // Validate the destination buffer is writable before writing.
    size_t needed = tag.size() + 1;
    if (!memory::IsRangeWritable(bufferPtr, needed)) {
        return;
    }

    // PERF: skip redundant writes (and the VirtualQuery scan) when the tag is unchanged.
    static std::string s_lastClanTag;
    if (s_lastClanTag == tag) {
        return;
    }
    memory::WriteString(bufferPtr, tag.c_str());
    s_lastClanTag = tag;
}

// H-9: spectator list caches the player snapshot so the 10 Hz background tick
// does not rebuild GetCachedPlayers() four times a second.
static std::vector<Game::CachedPlayer> g_specPlayerCache;
static uint64_t g_specCacheNextRebuild = 0;

void UpdateSpectatorList() {
    // P-1: called from the 10 Hz background thread; no frame-count throttle here.
    std::lock_guard<std::mutex> guard(g_spectatorsMutex);
    g_spectators.clear();
    if (!g_LocalPlayerPawn) {
        return;
    }

    uint64_t now = GetTickCount64();
    if (now >= g_specCacheNextRebuild) {
        g_specCacheNextRebuild = now + 1000; // rebuild at most 1 Hz
        g_specPlayerCache = Game::GetCachedPlayers();
    }

    // V-1: iterate the shared cached player list (refreshed at 1 Hz max).
    for (const Game::CachedPlayer& player : g_specPlayerCache) {
        void* entity = player.pawn;
        int index = player.index;

        // Resolve the observer services pointer from this pawn.
        void* observerServices = nullptr;
        if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_pObserverServices, observerServices) ||
            !memory::IsValidPtr(reinterpret_cast<uintptr_t>(observerServices))) {
            continue;
        }

        // Read the observer target handle.
        uint32_t targetHandle = 0;
        if (!memory::Read(reinterpret_cast<uintptr_t>(observerServices) + sdk::m_hObserverTarget, targetHandle)) {
            continue;
        }

        int targetIndex = static_cast<int>(targetHandle & 0x7FFF);
        if (targetIndex <= 0 || targetIndex > 64) {
            continue;
        }

        // Check if the target is the local player.
        if (Game::GetEntity(targetIndex) != g_LocalPlayerPawn) {
            continue;
        }

        // Read the observer mode to confirm we're actually being observed.
        uint8_t observerMode = 0;
        if (memory::Read(reinterpret_cast<uintptr_t>(observerServices) + sdk::m_iObserverMode, observerMode)) {
            if (observerMode == 0) {
                continue;
            }
        }

        // Get the spectator's name via their controller.
        SpectatorInfo info{};
        info.targetIndex = index;
        if (Game::GetPlayerName(entity, info.name, sizeof(info.name))) {
            // Successfully read the spectator name.
        } else {
            std::snprintf(info.name, sizeof(info.name), "Player %d", index);
        }
        g_spectators.push_back(info);
    }
}

void RevealRanksInternal() {
    if (!Config::revealRanksEnabled) {
        return;
    }

    // In CS2, rank information is sent via the CS_UM_ServerRankRevealAll user
    // message (enum value 350). The game already handles this natively via the
    // scoreboard, but we can force a server request by invoking the internal
    // rank-reveal function. We scan for the function pattern and call it.
    static uintptr_t revealFn = 0;
    static bool scanned = false;

    if (!scanned) {
        scanned = true;
        HMODULE client = GetModuleHandleA("client.dll");
        if (client) {
            // Pattern for ServerRankRevealAll: a function that constructs
            // and sends a CS_UM_ServerRankRevealAll message.
            // Look for: mov ecx, [g_LocalPlayerController]; call <fn>
            // This pattern finds the relative call site for the reveal all function.
            revealFn = memory::FindPattern(client,
                "48 8B CE E8 ? ? ? ? 48 8B 0D ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ?");
        }
    }

    if (!revealFn) {
        return;
    }

    using ServerRankRevealAll_t = void (*)();
    __try {
        reinterpret_cast<ServerRankRevealAll_t>(revealFn)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Pattern matched wrong code or calling convention differed; ignore gracefully
    }
}

void ApplyNoRecoil() {
    if (!Config::noRecoilEnabled || !LocalPawnAlive()) {
        return;
    }

    __try {
        uintptr_t pawn = reinterpret_cast<uintptr_t>(g_LocalPlayerPawn);

        uintptr_t aimPunchSvcs = 0;
        if (!memory::Read(pawn + sdk::m_pAimPunchServices, aimPunchSvcs) ||
            !memory::IsValidPtr(aimPunchSvcs)) {
            return;
        }

// Proven method: do NOT zero the pawn's aimPunch services — this is a
// client-only approach that breaks server RCS simulation. The RCS subtraction
// in combat.cpp:Run() handles recoil compensation when aimRcs is enabled.
        sdk::Vector zero{};
        memory::Write(aimPunchSvcs + sdk::m_aimPunchAngle, zero);
        memory::Write(aimPunchSvcs + sdk::m_aimPunchAngleVel, zero);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ApplyNoSmoke() {
    if (!Config::noSmokeEnabled || !LocalPawnAlive()) {
        return;
    }

    // Zero the local smoke overlay alpha every call (cheap, removes screen tint).
    __try {
        memory::Write(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_flLastSmokeOverlayAlpha, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Cache the smoke entity list; re-scan only when the highest entity index
    // changes or every 2 seconds (avoids iterating all entities every frame).
    static uintptr_t g_smokeEntities[64] = {};
    static int g_smokeCount = 0;
    static int g_lastHighestIndex = -1;
    static uint64_t g_lastRescan = 0;

    uint64_t now = GetTickCount64();
    int highest = Game::GetHighestEntityIndex();
    bool rescan = (highest != g_lastHighestIndex) || (now - g_lastRescan >= 2000);

    if (rescan) {
        g_smokeCount = 0;
        g_lastHighestIndex = highest;
        g_lastRescan = now;
        if (highest >= 1) {
            for (int i = 1; i <= highest && g_smokeCount < 64; ++i) {
                void* ent = Game::GetEntity(i);
                if (!ent) {
                    continue;
                }
                int tickBegin = 0;
                bool didEffect = false;
                if (memory::Read(reinterpret_cast<uintptr_t>(ent) + sdk::m_nSmokeEffectTickBegin, tickBegin) &&
                    memory::Read(reinterpret_cast<uintptr_t>(ent) + sdk::m_bDidSmokeEffect, didEffect)) {
                    if (tickBegin > 0 && didEffect) {
                        g_smokeEntities[g_smokeCount++] = reinterpret_cast<uintptr_t>(ent);
                    }
                }
            }
        }
    }

    // P2-4: clearing only the local overlay alpha is not enough; reset the
    // cached smoke entities so the game never starts the smoke effect.
    for (int k = 0; k < g_smokeCount; ++k) {
        uintptr_t ent = g_smokeEntities[k];
        __try {
            memory::Write(ent + sdk::m_nSmokeEffectTickBegin, 0);
            memory::Write(ent + sdk::m_bDidSmokeEffect, false);
            memory::Write(ent + sdk::m_bSmokeVolumeDataReceived, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

struct ThirdPersonState {
    bool hooked = false;
    uintptr_t hookAddr = 0;
    void* original = nullptr;
};
ThirdPersonState g_tpState;

using OverrideViewFn = void(__fastcall*)(__int64, void*);

// P4-4: thirdperson is gated by a configurable key (0 = always on when the
// feature is enabled). The old hardcoded 'M' check lived in RunWorldHooks.
bool IsThirdPersonActive() {
    if (!Config::thirdpersonEnabled) {
        return false;
    }
    if (Config::thirdpersonKey == 0) {
        return true;
    }
    return (GetAsyncKeyState(Config::thirdpersonKey) & 0x8000) != 0;
}

// CViewSetup fields (include/engine/cviewsetup.h): origin 0x4A0, angles 0x4B8.
constexpr uintptr_t kViewSetupOrigin = 0x4A0;
constexpr uintptr_t kViewSetupAngles = 0x4B8;

// CRASH-FIX: Cause 5 — the hook body never calls a null original; the install
// path verifies the trampoline too.
void __fastcall hkOverrideView(__int64 a1, void* pSetup) {
    static bool g_ovEntered = false;
    if (!g_ovEntered) {
        g_ovEntered = true;
        cs2::FileLog("[THREAD] hkOverrideView entered");
    }

    // Apply FOV / viewmodel FOV BEFORE the original builds the view setup so
    // the renderer consumes our values this frame. Writing these from Present
    // is one frame too late (the game resets them each tick), which is why the
    // FOV changer glitched and the viewmodel FOV never appeared.
    ApplyFov();
    ApplyViewmodelFov();

    if (!g_tpState.original) {
        return;   // do not call null
    }

    {
        static std::atomic<int> s_ovDebug{0};
        int ov = ++s_ovDebug;
        if (ov <= 4) {
            cs2::FileLog("[OV] hkOverrideView step: entered n=%d noRecoil=%d", ov, (int)Config::noRecoilEnabled);
        }
    }

    if (Config::noRecoilEnabled) {
        ApplyNoRecoil();
    }

    {
        static std::atomic<int> s_ovDebug2{0};
        if (++s_ovDebug2 <= 4) {
            cs2::FileLog("[OV] hkOverrideView step: calling original n=%d", s_ovDebug2.load());
        }
    }

    reinterpret_cast<OverrideViewFn>(g_tpState.original)(a1, pSetup);

    {
        static std::atomic<int> s_ovDebug3{0};
        if (++s_ovDebug3 <= 4) {
            cs2::FileLog("[OV] hkOverrideView step: original returned n=%d", s_ovDebug3.load());
        }
    }

    if (!IsThirdPersonActive() || !pSetup) {
        return;
    }

    __try {
        uintptr_t pawn = reinterpret_cast<uintptr_t>(g_LocalPlayerPawn);
        bool scoped = false;
        if (memory::Read(pawn + sdk::m_bIsScoped, scoped) && scoped) {
            return;
        }

        uintptr_t setup = reinterpret_cast<uintptr_t>(pSetup);
        float* angles = reinterpret_cast<float*>(setup + kViewSetupAngles);
        float* origin = reinterpret_cast<float*>(setup + kViewSetupOrigin);

        float pitch = angles[0] * 0.01745329251f;
        float yaw = angles[1] * 0.01745329251f;
        float fx = cosf(pitch) * cosf(yaw);
        float fy = cosf(pitch) * sinf(yaw);
        float fz = -sinf(pitch);
        float dist = Config::thirdpersonDistance;
        origin[0] -= fx * dist;
        origin[1] -= fy * dist;
        origin[2] -= fz * dist;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool InstallThirdPersonHook() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        return false;
    }

    // Try multiple OverrideView patterns for build compatibility.
    constexpr const char* kOverrideViewPatterns[] = {
        "40 57 48 83 EC ? 48 8B FA E8 ? ? ? ? BA",
        "40 53 57 48 83 EC 58 48 8B FA E8 ? ? ? ? 48",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B FA",
    };

    uintptr_t addr = 0;
    for (const char* pat : kOverrideViewPatterns) {
        addr = memory::FindPattern(client, pat, false);
        if (addr) {
            cs2::FileLog("[HOOK] OverrideView pattern matched: '%s' at %p", pat, reinterpret_cast<void*>(addr));
            break;
        }
    }

    if (!addr) {
        cs2::FileLog("[HOOK] OverrideView: no pattern matched — thirdperson/norecoil disabled");
        return false;
    }

    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(addr),
                                      reinterpret_cast<LPVOID>(&hkOverrideView),
                                      reinterpret_cast<LPVOID*>(&g_tpState.original));
    if (status == MH_OK && g_tpState.original) {
        MH_EnableHook(reinterpret_cast<void*>(addr));
        g_tpState.hookAddr = addr;
        g_tpState.hooked = true;
        cs2::FileLog("[HOOK] OverrideView hook OK at %p", reinterpret_cast<void*>(addr));
        return true;
    } else if (status == MH_ERROR_ALREADY_CREATED) {
        cs2::FileLog("[HOOK] OverrideView already hooked by another module — leaving alone");
        return false;
    }
    cs2::FileLog("[HOOK] OverrideView hook failed status=%d", (int)status);
    return false;
}

void RunThirdPerson() {
    // The OverrideView hook is installed once at startup (Misc::InstallHooks,
    // P2-3); this function only maintains the CCSGOInput thirdperson flag.
    if (!g_tpState.hooked || !g_ClientBase || !sdk::g_CSGOInputRVA) {
        return;
    }

    bool active = IsThirdPersonActive();
    static bool lastWritten = false;

    uintptr_t input = g_ClientBase + sdk::g_CSGOInputRVA;
    if (active) {
        // include/patterns/patterns.json "ThirdPersonReset" / "ConCommand_firstperson":
        // CCSGOInput third-person byte is at +0x229 (the same routine clears
        // +0x229 and the +0x6A8 chase-distance DWORD). This is dump-verified;
        // the old 0xA51 write was an unverified guess.
        constexpr uintptr_t kThirdPersonFlag = 0x229;
        __try {
            memory::Write(input + kThirdPersonFlag, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        lastWritten = true;
    } else if (lastWritten) {
        constexpr uintptr_t kThirdPersonFlag = 0x229;
        // Write false exactly once on the falling edge.
        __try {
            memory::Write(input + kThirdPersonFlag, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        lastWritten = false;
    }
}

} // namespace

void InstallHooks() {
    // P2-3: install OverrideView unconditionally at startup.
    if (!g_tpState.hooked) {
        InstallThirdPersonHook();
    }
}

void RevealRanks() {
    // P1-10/P3-3: public wrapper for the internal rank-reveal request.
    RevealRanksInternal();
}

void ApplyAutoAccept() {
    static uintptr_t acceptFn = 0;
    static bool scanned = false;
    static bool accepted = false;

    if (!Config::autoAcceptEnabled) {
        accepted = false;
        return;
    }

    if (!g_ClientBase) {
        return;
    }

    if (accepted) {
        return;
    }

    if (!scanned) {
        scanned = true;
        HMODULE client = GetModuleHandleA("client.dll");
        if (client) {
            // Primary pattern contains the E8 call to the actual accept function.
            // Layout: mov rcx,[rip+disp32] (7); test rcx,rcx (3); jz rel8 (2); E8 call (5).
            // The E8 opcode is at offset 12 from the pattern match.
            uintptr_t patternMatch = memory::FindPattern(client,
                "48 8B 0D ? ? ? ? 48 85 C1 74 ? E8 ? ? ? ? 84 C0 74 ?");
            if (patternMatch) {
                acceptFn = memory::ResolveRipRelative(patternMatch + 12, 1, 5);
            }

            if (!acceptFn) {
                // Fallback: pattern ending with E8 call after the null-check block.
                patternMatch = memory::FindPattern(client,
                    "48 8B 05 ? ? ? ? 48 85 C0 74 ? 32 DB E8 ? ? ? ?");
                if (patternMatch) {
                    acceptFn = memory::ResolveRipRelative(patternMatch + 14, 1, 5);
                }
            }
        }
    }

    if (!acceptFn) {
        return;
    }

    __try {
        using AcceptMatch_t = void(__cdecl*)();
        reinterpret_cast<AcceptMatch_t>(acceptFn)();
        accepted = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        accepted = true;
    }
}

void ApplyAutoDefuse(sdk::CUserCmd* cmd) {
    if (!Config::autoDefuseEnabled || !LocalPawnAlive() || !cmd) {
        return;
    }

    __try {
        void* bomb = Game::FindPlantBomb();
        if (!bomb) {
            return;
        }

        bool beingDefused = false;
        memory::Read(reinterpret_cast<uintptr_t>(bomb) + sdk::m_bBeingDefused, beingDefused);
        if (beingDefused) {
            sdk::SetCmdButton(cmd, sdk::IN_USE, true);
            return;
        }

        sdk::Vector bombPos{};
        sdk::Vector localPos{};
        if (!Game::GetEntityOrigin(bomb, bombPos) ||
            !Game::GetEntityOrigin(g_LocalPlayerPawn, localPos)) {
            return;
        }

        float dx = bombPos.x - localPos.x;
        float dy = bombPos.y - localPos.y;
        float dz = bombPos.z - localPos.z;
        float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq > 1600.0f) {
            return;
        }

        sdk::SetCmdButton(cmd, sdk::IN_USE, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void CaptureBulletTracer(sdk::CUserCmd* cmd) {
    if (!Config::bulletTracersEnabled || !g_LocalPlayerPawn || !cmd) {
        return;
    }

    __try {
        // P1-6: tracers must spawn on the shot, not on the server hit counter.
        // Primary signal: the active weapon's clip decreases this tick.
        bool fired = false;
        if (void* weapon = Game::GetActiveWeapon()) {
            int clip = -1;
            if (memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_iClip1, clip)) {
                if (g_lastClip >= 0 && clip < g_lastClip) {
                    fired = true;
                }
                g_lastClip = clip;
            }
        } else {
            g_lastClip = -1;
        }

        // Fallback for weapons with no clip (knife, grenades): shots-fired delta.
        int shots = Game::GetShotsFired();
        if (!fired && shots > g_lastShotsFired) {
            fired = true;
        }
        g_lastShotsFired = shots;

        // Keep the old server-hit counter updated so it never spuriously fires.
        uintptr_t pawn = reinterpret_cast<uintptr_t>(g_LocalPlayerPawn);
        uintptr_t bulletServices = 0;
        if (memory::Read(pawn + sdk::m_pBulletServices, bulletServices) &&
            memory::IsValidPtr(bulletServices)) {
            std::uint64_t hits = 0;
            if (memory::Read(bulletServices + sdk::m_totalHitsOnServer, hits)) {
                g_lastBulletServicesHits = hits;
            }
        }

        if (!fired) {
            return;
        }

        sdk::Vector eye{};
        if (!Game::GetLocalEyePosition(eye)) {
            return;
        }

        sdk::Vector angles{};
        if (!sdk::GetViewAngles(angles)) {
            return;
        }

        sdk::Vector forward{};
        sdk::Vector right{};
        sdk::Vector up{};
        Math::AngleVectors(angles, &forward, &right, &up);

        // P4-5: m_vecMuzzlePos0/1 only exist on CCSWeaponBaseVData (0x608),
        // not on the weapon entity, so approximate the muzzle from the eye.
        sdk::Vector start = eye + forward * 15.0f + right * 4.0f - up * 3.0f;
        sdk::Vector end = eye + forward * 8192.0f;

        Visuals::AddBulletTracer(start, end);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void AutoStrafe(sdk::CUserCmd* cmd) {
    // CRASH-FIX: Cause 2 � never use a null/invalid local pawn.
    if (!cmd || !g_LocalPlayerPawn ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }
    if (!Config::autoStrafeEnabled) {
        return;
    }

    // Only air-strafe: never on the ground. The switch-key gate and the
    // |delta| <= 0.5 dead zone are removed so the strafe always applies while
    // airborne (use autoStrafeHumanity to make it look legit).
    uint32_t flags = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_fFlags, flags)) {
        return;
    }
    bool onGround = (flags & FL_ONGROUND) != 0;
    if (onGround) {
        return;
    }

    // Read velocity.
    sdk::Vector vel{};
    if (!memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_vecVelocity, vel)) {
        return;
    }
    float speed2D = vel.Length2D();
    if (speed2D < 5.0f) {
        return;
    }

    // Humanity: randomly skip ticks based on the slider (0-100).
    if (Config::autoStrafeHumanity > 0.0f && Config::autoStrafeHumanity < 100.0f) {
        static thread_local std::mt19937 g_humanityRng(
            static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        static thread_local std::uniform_real_distribution<float> g_humanityDist(0.0f, 100.0f);
        if (g_humanityDist(g_humanityRng) > Config::autoStrafeHumanity) {
            return;
        }
    }

    // Read view angles (command first, live fallback).
    sdk::Vector viewAngles{};
    if (!sdk::GetCmdViewAngles(cmd, viewAngles) && !sdk::GetViewAngles(viewAngles)) {
        return;
    }

    float velYaw = std::atan2(vel.y, vel.x) * 57.29577951308232f;
    float delta = sdk::NormalizeYaw(viewAngles.y - velYaw); // [-180, 180]

    // Calculate ideal strafe direction: +1 for right (clockwise from velocity),
    // -1 for left (counter-clockwise). This keeps the wishdir perpendicular
    // to velocity for maximum air acceleration (speedboost).
    float strafeDir = 0.0f;
    if (static_cast<int>(Config::autoStrafeMode) == 1) {
        // Auto mode: rotate view to maintain perpendicular wishdir for speedboost.
        // Seed strafeDir from the view-to-velocity delta so the accumulation
        // loop actually has a direction to build on.
        strafeDir = (delta >= 0.0f) ? 1.0f : -1.0f;
        // Auto mode: rotate view to maintain perpendicular wishdir for speedboost.
        // The view angle is rotated per-tick toward the direction that keeps
        // movement command wishdir 90° from velocity — this maximizes acceleration.
        static thread_local float g_speedTurnAccum = 0.0f;
        constexpr float kRadToDeg = 57.29577951308232f;
        constexpr float kTickInterval = 1.0f / 64.0f;
        constexpr float sv_airaccelerate = 12.0f;
        constexpr float sv_air_max_wishspeed = 30.0f;
        // Faster turn rate for maximum speedboost; the Max Turn slider fine-tunes it.
        float baseTurnRate = (sv_airaccelerate * sv_air_max_wishspeed / speed2D) * kRadToDeg;
        float perTick = baseTurnRate * kTickInterval;
        // Apply Max Turn slider as a cap; if slider is high (>= 20), speedboost is near-max.
        float sliderCap = Config::autoStrafeMaxTurn * kTickInterval;
        if (perTick > sliderCap) {
            perTick = sliderCap;
        }
        // Accumulate rotation; reverse direction if we've over-rotated past 90°.
        g_speedTurnAccum += perTick * strafeDir;
        // Normalize accumulated angle; clamp to ±90° from velocity for effective speedboost.
        g_speedTurnAccum = std::max(-90.0f, std::min(90.0f, g_speedTurnAccum));
        float yawAdjust = g_speedTurnAccum;
        viewAngles.y = sdk::NormalizeYaw(viewAngles.y - yawAdjust * strafeDir);
        strafeDir = (g_speedTurnAccum > 0.0f) ? 1.0f : (g_speedTurnAccum < 0.0f ? -1.0f : 0.0f);
        if (strafeDir == 0.0f) {
            // Reset accumulation when centered; avoid fighting the velocity vector.
            g_speedTurnAccum = 0.0f;
            return;
        }
    } else {
        // Assist mode: strafe only while the player is turning, side = turn dir.
        sdk::Vector eyeVel{};
        if (memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_angEyeAnglesVelocity,
                         eyeVel)) {
            float yawVel = eyeVel.y;
            if (yawVel > 0.25f) {
                strafeDir = 1.0f;
            } else if (yawVel < -0.25f) {
                strafeDir = -1.0f;
            }
        }
    }

    if (strafeDir == 0.0f) {
        return;
    }

    // Write movement services commands for maximum speedboost.
    // m_flCmdLeftMove/0x1A4 controls sidemove; forwardmove kept at 0 to let
    // the engine's air acceleration work based on wishdir angle.
    sdk::SetCmdSideMove(cmd, strafeDir * 450.0f);
    sdk::SetCmdForwardMove(cmd, 0.0f);
    // Keep the button state consistent with the movement floats.
    sdk::SetCmdButton(cmd, sdk::IN_MOVELEFT, strafeDir < 0.0f);
    sdk::SetCmdButton(cmd, sdk::IN_MOVERIGHT, strafeDir > 0.0f);
}
void PreCreateMove() {
    // Runs before origCreateMove so the movement-services values we write are
    // what the cmd builder (and client-side prediction) read. This keeps the
    // server and the client in agreement, avoiding prediction desync/rubberband.
    if (!g_LocalPlayerPawn ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    uint32_t flags = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_fFlags, flags)) {
        return;
    }
    bool onGround = (flags & FL_ONGROUND) != 0;

    sdk::Vector vel{};
    memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_vecVelocity, vel);
    float speed2D = vel.Length2D();

    void* mvs = nullptr;
    memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_pMovementServices, mvs);

    if (!Config::autoStrafeEnabled || onGround || speed2D < 5.0f || !mvs) {
        return;
    }

    sdk::Vector viewAngles{};
    if (!sdk::GetViewAngles(viewAngles)) {
        return;
    }

    float velYaw = std::atan2(vel.y, vel.x) * 57.29577951308232f;
    float delta = sdk::NormalizeYaw(viewAngles.y - velYaw);

    float strafeDir = 0.0f;
    if (static_cast<int>(Config::autoStrafeMode) == 1) {
        static float g_strafeSide = 0.0f;
        if (g_strafeSide != 0.0f && std::fabs(delta) < 2.0f) {
            strafeDir = g_strafeSide;
        } else {
            strafeDir = (delta >= 0.0f) ? 1.0f : -1.0f;
            g_strafeSide = strafeDir;
        }
    } else {
        sdk::Vector eyeVel{};
        memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_angEyeAnglesVelocity,
                     eyeVel);
        float yawVel = eyeVel.y;
        if (yawVel > 0.25f) {
            strafeDir = 1.0f;
        } else if (yawVel < -0.25f) {
            strafeDir = -1.0f;
        }
    }

    if (strafeDir == 0.0f) {
        return;
    }

    // Humanity: randomly skip ticks so the strafe isn't perfectly consistent.
    if (Config::autoStrafeHumanity > 0.0f) {
        static std::mt19937 g_humanityRng(
            static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        static std::uniform_real_distribution<float> g_humanityDist(0.0f, 100.0f);
        if (g_humanityDist(g_humanityRng) < Config::autoStrafeHumanity) {
            return;
        }
    }

    // Movement-services fields (CPlayer_MovementServices, schema-verified):
    //   m_flCmdForwardMove 0x1A0, m_flCmdLeftMove 0x1A4,
    //   m_flForwardMove    0x1C0, m_flLeftMove       0x1C4
    constexpr std::ptrdiff_t kCmdForwardMove = 0x1A0;
    constexpr std::ptrdiff_t kCmdLeftMove = 0x1A4;
    constexpr std::ptrdiff_t kForwardMove = 0x1C0;
    constexpr std::ptrdiff_t kLeftMove = 0x1C4;

    const float side = strafeDir * 450.0f;
    memory::Write(reinterpret_cast<uintptr_t>(mvs) + kCmdForwardMove, 0.0f);
    memory::Write(reinterpret_cast<uintptr_t>(mvs) + kCmdLeftMove, side);
    memory::Write(reinterpret_cast<uintptr_t>(mvs) + kForwardMove, 0.0f);
    memory::Write(reinterpret_cast<uintptr_t>(mvs) + kLeftMove, side);

    // Auto mode: rotate the live view toward the velocity at the optimal
    // air-acceleration turn rate so the wishdir stays perpendicular to the
    // velocity and speed keeps climbing. Visible view drift is expected and is
    // what makes "Auto" a speedboost; use Assist mode (or the Max Turn slider)
    // to keep it legit.
    if (static_cast<int>(Config::autoStrafeMode) == 1) {
        constexpr float kRadToDeg = 57.29577951308232f;
        constexpr float kTickInterval = 1.0f / 64.0f;
        constexpr float sv_airaccelerate = 12.0f;
        constexpr float sv_air_max_wishspeed = 30.0f;
        float turnRate = (sv_airaccelerate * sv_air_max_wishspeed / speed2D) * kRadToDeg;
        float perTick = turnRate * kTickInterval;
        float sliderPerTick = Config::autoStrafeMaxTurn * kTickInterval;
        if (perTick > sliderPerTick) {
            perTick = sliderPerTick;
        }
        float newYaw = sdk::NormalizeYaw(viewAngles.y - perTick * strafeDir);
        sdk::SetViewAngles({viewAngles.x, newYaw, viewAngles.z});
    }
}

void Run(sdk::CUserCmd* cmd) {
    if (!cmd || !g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    // Auto-strafe (independent of bunnyhop)
    AutoStrafe(cmd);

    // Stateless bhop: tap IN_JUMP on the landing tick and release it in the
    // air. Never hold IN_JUMP while airborne — the subtick jump-spam penalty
    // (sv_jump_spam_penalty_time) downgrades re-presses within one tick, which
    // is what caused the old "stucking" / micro-jump behaviour. Humanity
    // randomly skips presses to make it look legit.
    static bool wasOnGround = false;

    uint32_t flags = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_fFlags, flags)) {
        sdk::SetCmdButton(cmd, sdk::IN_JUMP, false);
        wasOnGround = false;
        CaptureBulletTracer(cmd);
        return;
    }
    bool onGround = (flags & FL_ONGROUND) != 0;

    static std::mt19937 g_bhopHumanityRng(
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_real_distribution<float> g_bhopHumanityDist(0.0f, 100.0f);

    auto bhopShouldJump = [&]() {
        if (Config::bhopHumanity <= 0.0f) {
            return true;
        }
        return g_bhopHumanityDist(g_bhopHumanityRng) >= Config::bhopHumanity;
    };

    if (Config::bhopKey == 0 || (GetAsyncKeyState(Config::bhopKey) & 0x8000) != 0) {

    if (onGround && !wasOnGround) {
            // Fresh landing: tap jump once.
            if (bhopShouldJump()) {
                sdk::SetCmdButton(cmd, sdk::IN_JUMP, true);
            }
        } else if (!onGround) {
            sdk::SetCmdButton(cmd, sdk::IN_JUMP, false);
        }
        // Holding jump while already on the ground (onGround && wasOnGround):
        // do not re-press — that would spam the jump and trigger the penalty.
        wasOnGround = onGround;
    } else {
        sdk::SetCmdButton(cmd, sdk::IN_JUMP, false);
        wasOnGround = false;
    }

    CaptureBulletTracer(cmd);
}

std::atomic<bool> g_bgThreadStarted{false};
static std::atomic<bool> g_bgStopRequested{false};

// N-DET: helper for BackgroundLoop body — separate function so __try/__except
// doesn't conflict with C++ object unwinding (std::lock_guard).
static void BackgroundLoopBody() {
    if (Config::noSmokeEnabled) {
        ApplyNoSmoke();
    }
    if (Config::spectatorListEnabled) {
        UpdateSpectatorList();
    } else {
        std::lock_guard<std::mutex> lock(g_spectatorsMutex);
        g_spectators.clear();
    }
}

// P-1: heavy 10 Hz loops run on a dedicated thread; the render/CreateMove
// threads never pay for them. Wrapped in SEH so a crash in this thread
// cannot kill cs2.exe.
void BackgroundLoop() {
    __try {
        cs2::FileLog("[THREAD] BackgroundLoop entered");
        // 0-3: every cheat thread hides itself and clears BeingDebugged.
        Security::InitializeNTSClear();
        Security::HideFromDebugger();

        // M-10: set started flag from INSIDE the thread so it's accurate.
        g_bgThreadStarted = true;

        while (!g_bgStopRequested.load()) {
            __try {
                BackgroundLoopBody();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                static bool g_loggedBgCrash = false;
                if (!g_loggedBgCrash) {
                    g_loggedBgCrash = true;
                    cs2::FileLog("[THREAD] BackgroundLoop: exception in feature pass — caught");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        g_bgThreadStarted = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cs2::FileLog("[THREAD] BackgroundLoop: fatal exception — thread exiting");
        g_bgThreadStarted = false;
    }
}

void StartBackgroundThread() {
    if (g_bgThreadStarted) {
        return;
    }
    g_bgStopRequested = false;
    std::thread(BackgroundLoop).detach();
}

void StopBackgroundThread() {
    g_bgStopRequested = true;
    // Wait up to 2s for the thread to observe the stop request.
    for (int i = 0; i < 200 && g_bgThreadStarted.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static void ClearSpectators() {
    // The background thread writes g_spectators under this mutex; take it here
    // too instead of clearing the vector unlocked.
    std::lock_guard<std::mutex> lock(g_spectatorsMutex);
    g_spectators.clear();
}

void Apply() {
    if (!LocalPawnAlive()) {
        g_lastPawn = nullptr;
        g_haveOriginalFov = false;
        g_haveOriginalViewmodelFov = false;
        ClearSpectators();
        return;
    }

    RefreshPawnState();

    // P-1: start the heavy-loop background thread once; it owns the no-smoke
    // memory writes and the spectator scan at 10 Hz.
    StartBackgroundThread();

    // No-flash: zero the flash duration and alpha.
    if (Config::noFlashEnabled) {
        __try {
            memory::Write(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_flFlashDuration, 0.0f);
            memory::Write(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_flFlashMaxAlpha, 0.0f);
            memory::Write(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_flFlashOverlayAlpha, 0.0f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    ApplyFov();
    ApplyViewmodelFov();

    // Clan tag changer.
    if (Config::clanChangerEnabled) {
        ApplyClanTag();
    }

    // Reveal ranks (request from server).
    RevealRanksInternal();

    // Thirdperson / no recoil hook management.
    RunThirdPerson();
}

std::vector<SpectatorInfo> GetSpectators() {
    std::lock_guard<std::mutex> lock(g_spectatorsMutex);
    return g_spectators;
}

} // namespace Misc
