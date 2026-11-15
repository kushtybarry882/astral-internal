// AUDIT: P1-2, P1-3, P2-5, P2-6
// UPGRADE: V-1, V-4
// CRASH-FIX: Cause 2 — GetLocalEyePosition validates the pawn pointer.
#include "game.h"
#include "memory.h"
#include "math.h"
#include "usercmd.h"
#include "log.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace Game {

namespace {

constexpr float kWorldLimit = 1000000.0f;

// CS2 entity list layout (chunk-based). These are stable across CS2 builds but
// documented here so they can be patch-corrected in one place.
constexpr uintptr_t kEntityChunkListOffset = 0x10;
constexpr uintptr_t kEntityStride = 0x70;

bool IsFiniteVector(const sdk::Vector& value) {
    return value.IsFinite() && std::abs(value.x) < kWorldLimit &&
           std::abs(value.y) < kWorldLimit && std::abs(value.z) < kWorldLimit;
}

std::mutex g_gameStateMutex;    // protects g_ClientBase, g_LocalPlayerPawn, g_LocalPlayerController, g_EntityListBase
std::mutex g_viewMatrixMutex;   // protects g_ViewMatrix separately from game-state scans

// Close the original anonymous namespace, then open a new one for internal helpers.
} // namespace

// N-M-10: generation counter to invalidate entity index cache on list refresh.
// Declared here (before Refresh) so Refresh can increment it.
static uint64_t g_entityListGeneration = 0;

// N-M-2: internal unsynchronized helpers — callers must hold g_gameStateMutex.
// Prevents recursive_mutex overhead and deadlock from re-entrant locks.
namespace {
inline void* GetEntityUnlocked(int index) {
    uintptr_t entityList = g_EntityListBase;
    if (!entityList || index < 0 || index > 0x7FFF) {
        return nullptr;
    }

    uintptr_t chunk = 0;
    uintptr_t listOffset = 0x8 * (static_cast<uintptr_t>(index & 0x7FFF) >> 9) + kEntityChunkListOffset;
    if (!memory::Read(entityList + listOffset, chunk) || !memory::IsValidPtr(chunk)) {
        return nullptr;
    }

    void* entity = nullptr;
    uintptr_t entityOffset = kEntityStride * static_cast<uintptr_t>(index & 0x1FF);
    if (!memory::Read(chunk + entityOffset, entity)) {
        return nullptr;
    }
    if (!memory::IsValidPtr(reinterpret_cast<uintptr_t>(entity))) {
        return nullptr;
    }

    uint32_t entityIndex = 0;
    if (!memory::Read(chunk + entityOffset + 0x10, entityIndex)) {
        return nullptr;
    }
    if ((entityIndex & 0x7FFF) != static_cast<uint32_t>(index & 0x7FFF)) {
        return nullptr;
    }
    return entity;
}

inline void* GetPlayerPawnUnlocked(void* controller) {
    if (!controller || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(controller))) {
        return nullptr;
    }
    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(controller) + sdk::m_hPlayerPawn, handle)) {
        return nullptr;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0 || index > 0x7FFF) {
        return nullptr;
    }
    return GetEntityUnlocked(index);
}

} // namespace

// dwGlobalVars is a pointer slot in this build. Verified in client.dll: the
// accessor at RVA 0xB82140 starts with `mov rcx, [rip+disp]` targeting
// client+0x2235108 and returns that pointer (or null). Deref exactly once.
sdk::GlobalVars* GetGlobalVars() {
    return GetGlobalVars(g_ClientBase);
}

sdk::GlobalVars* GetGlobalVars(uintptr_t clientBase) {
    if (!clientBase) {
        return nullptr;
    }

    uintptr_t gvPtr = 0;
    if (!memory::Read(clientBase + sdk::dwGlobalVars, gvPtr) || !memory::IsValidPtr(gvPtr)) {
        return nullptr;
    }

    // Validate the fields we consume against the binary-verified layout.
    // curtime may legitimately be 0 while the client is still loading.
    float curtime = 0.0f;
    std::int32_t tickcount = 0;
    std::uint32_t maxclients = 0;
    if (!memory::Read(gvPtr + offsetof(sdk::GlobalVars, curtime), curtime) ||
        !std::isfinite(curtime) || curtime < 0.0f || curtime > 1000000.0f) {
        return nullptr;
    }
    if (!memory::Read(gvPtr + offsetof(sdk::GlobalVars, tickcount), tickcount) ||
        tickcount < 0) {
        return nullptr;
    }
    if (!memory::Read(gvPtr + offsetof(sdk::GlobalVars, maxclients), maxclients) ||
        maxclients == 0 || maxclients > 256) {
        return nullptr;
    }
    return reinterpret_cast<sdk::GlobalVars*>(gvPtr);
}

void Refresh() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        std::lock_guard<std::mutex> lock(g_gameStateMutex);
        g_ClientBase = 0;
        g_LocalPlayerPawn = nullptr;
        g_LocalPlayerController = nullptr;
        g_EntityListBase = 0;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_gameStateMutex);
        g_ClientBase = reinterpret_cast<uintptr_t>(client);

        void* pawn = nullptr;
        void* controller = nullptr;
        uintptr_t entityList = 0;

        bool pawnRead = memory::Read(g_ClientBase + sdk::dwLocalPlayerPawn, pawn);
        bool ctrlRead = memory::Read(g_ClientBase + sdk::dwLocalPlayerController, controller);
        bool listRead = memory::Read(g_ClientBase + sdk::dwGameEntitySystem, entityList);

        g_LocalPlayerPawn = pawn;
        g_LocalPlayerController = controller;

        if (g_EntityListBase != entityList) {
            g_EntityListBase = entityList;
            ++g_entityListGeneration; // N-M-10: invalidate index cache on list change
        } else {
            g_EntityListBase = entityList;
        }

        // dwViewMatrix is an INLINE 4x4 matrix global in the client image — read it
        // directly, do not deref. (Reading a pointer there was the previous bug:
        // the address contains float matrix rows, not a pointer.)
        sdk::ViewMatrix viewMatrix{};
        bool vmRead = memory::Read(g_ClientBase + sdk::dwViewMatrix, viewMatrix);
        if (vmRead) {
            std::lock_guard<std::mutex> vmLock(g_viewMatrixMutex);
            g_ViewMatrix = viewMatrix;
        }
        static bool firstLog = true;
        if (firstLog) {
            firstLog = false;
            cs2::FileLog("Refresh: vmRead=%d m33=%.4f m00=%.4f m01=%.4f m02=%.4f m03=%.4f",
                         vmRead, g_ViewMatrix.m[3][3], g_ViewMatrix.m[0][0], g_ViewMatrix.m[0][1],
                         g_ViewMatrix.m[0][2], g_ViewMatrix.m[0][3]);
        }
    }
}

GameState GetStateSnapshot() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    GameState state;
    state.clientBase = g_ClientBase;
    state.localPlayerPawn = g_LocalPlayerPawn;
    state.localPlayerController = g_LocalPlayerController;
    state.entityListBase = g_EntityListBase;
    state.viewMatrix = g_ViewMatrix;
    return state;
}

void* GetEntity(int index) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    return GetEntityUnlocked(index);
}

void* GetControllerForPawn(void* pawn) {
    if (!pawn) {
        return nullptr;
    }

    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(pawn) + sdk::m_hController, handle)) {
        return nullptr;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0) {
        return nullptr;
    }

    void* controller = GetEntity(index);

    // P2-5: one-time validation log for the m_hController offset (0x14BC).
    // If this ever prints a mismatch the offset has drifted and must be
    // re-checked against the schema dump.
    static bool loggedController = false;
    if (!loggedController) {
        loggedController = true;
        uint32_t backHandle = 0;
        bool backOk = controller &&
            memory::Read(reinterpret_cast<uintptr_t>(controller) + sdk::m_hPlayerPawn, backHandle);
        cs2::FileLog("GetControllerForPawn: pawn=%p handle=0x%X idx=%d ctrl=%p back=0x%X ok=%d",
                     pawn, handle, index, controller, backHandle, backOk ? 1 : 0);
    }

    return controller;
}

void* GetPlayerPawn(void* controller) {
    if (!controller || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(controller))) {
        return nullptr;
    }
    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(controller) + sdk::m_hPlayerPawn, handle)) {
        return nullptr;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0 || index > 0x7FFF) {
        return nullptr;
    }
    return GetEntity(index);
}

int GetEntityIndex(void* entity) {
    if (!entity) {
        return -1;
    }
    // Search the entity list for this pointer (m_iIDEntIndex is the crosshair
    // target, NOT the entity's own slot index).
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    uintptr_t entityList = g_EntityListBase;
    if (!entityList) {
        return -1;
    }

    // P2-6: direct-mapped cache. The resolver calls this for every player every
    // tick; a cached slot is validated with a single GetEntity() lookup before
    // being trusted, so stale entries are self-healing.
    static void* cachedEntity[64] = {};
    static int cachedIndex[64] = {};
    static uint64_t cachedGeneration = 0;
    if (cachedGeneration != g_entityListGeneration) {
        // Entity list was refreshed — invalidate all cache slots.
        for (int i = 0; i < 64; ++i) {
            cachedEntity[i] = nullptr;
            cachedIndex[i] = 0;
        }
        cachedGeneration = g_entityListGeneration;
    }
    size_t slot = (reinterpret_cast<uintptr_t>(entity) >> 4) & 63;
    int hint = cachedIndex[slot];
    if (cachedEntity[slot] == entity && hint > 0 && GetEntityUnlocked(hint) == entity) {
        return hint;
    }

    for (int i = 1; i <= 64; ++i) {
        void* ent = nullptr;
        uintptr_t chunk = 0;
        uintptr_t listOffset = 0x8 * (static_cast<uintptr_t>(i) >> 9) + kEntityChunkListOffset;
        if (!memory::Read(entityList + listOffset, chunk) || !memory::IsValidPtr(chunk)) {
            continue;
        }
        uintptr_t entityOffset = kEntityStride * static_cast<uintptr_t>(i & 0x1FF);
        if (!memory::Read(chunk + entityOffset, ent)) {
            continue;
        }
        if (ent == entity) {
            cachedEntity[slot] = entity;
            cachedIndex[slot] = i;
            return i;
        }
    }
    cachedEntity[slot] = nullptr;
    cachedIndex[slot] = 0;
    return -1;
}

bool GetEntityOrigin(void* entity, sdk::Vector& out) {
    out = {};
    if (!entity) {
        return false;
    }

    void* sceneNode = nullptr;
    if (memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_pGameSceneNode, sceneNode) &&
        memory::IsValidPtr(reinterpret_cast<uintptr_t>(sceneNode))) {
        sdk::Vector origin{};
        if (memory::Read(reinterpret_cast<uintptr_t>(sceneNode) + sdk::m_vecAbsOrigin, origin) &&
            IsFiniteVector(origin)) {
            out = origin;
            return true;
        }

        // Fallback: read m_vecOrigin from the scene node (0x80 on CGameSceneNode)
        sdk::Vector fallback{};
        if (memory::Read(reinterpret_cast<uintptr_t>(sceneNode) + sdk::m_vecOrigin, fallback) &&
            IsFiniteVector(fallback)) {
            out = fallback;
            return true;
        }
    }
    return false;
}

namespace {

bool GetBonePositionCore(void* entity, int boneIndex, const sdk::Vector* entityOrigin,
                         sdk::Vector& out) {
    out = {};
    if (!entity || boneIndex < 0 || boneIndex >= 128) {
        return false;
    }

    void* sceneNode = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_pGameSceneNode, sceneNode) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(sceneNode))) {
        return false;
    }

    uintptr_t boneArray = 0;
    uintptr_t modelState = reinterpret_cast<uintptr_t>(sceneNode) + sdk::m_modelState;
    if (!memory::Read(modelState + sdk::m_boneArray, boneArray) ||
        !memory::IsValidPtr(boneArray)) {
        return false;
    }

    sdk::Vector bone{};
    float raw[3] = {};
    if (!memory::Read(boneArray + static_cast<uintptr_t>(boneIndex) * 0x20, raw)) {
        return false;
    }
    bone.x = raw[0];
    bone.y = raw[1];
    bone.z = raw[2];

    if (!IsFiniteVector(bone)) {
        return false;
    }

    if (entityOrigin && entityOrigin->DistTo(bone) > 512.0f) {
        return false;
    }

    out = bone;
    return true;
}

} // namespace

bool GetBonePosition(void* entity, int boneIndex, sdk::Vector& out) {
    sdk::Vector origin{};
    const sdk::Vector* originPtr = GetEntityOrigin(entity, origin) ? &origin : nullptr;
    return GetBonePositionCore(entity, boneIndex, originPtr, out);
}

bool GetBonePosition(void* entity, int boneIndex, const sdk::Vector& entityOrigin,
                     sdk::Vector& out) {
    return GetBonePositionCore(entity, boneIndex, &entityOrigin, out);
}

bool GetLocalEyePosition(sdk::Vector& out) {
    out = {};
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    // CRASH-FIX: Cause 2 — validate the pawn pointer before any read.
    void* localPawn = g_LocalPlayerPawn;
    if (!localPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(localPawn))) {
        return false;
    }

    sdk::Vector origin{};
    if (!GetEntityOrigin(localPawn, origin)) {
        return false;
    }

    sdk::Vector viewOffset{};
    if (memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_vecViewOffset, viewOffset) &&
        IsFiniteVector(viewOffset) && !viewOffset.IsZero()) {
        out = origin + viewOffset;
    } else {
        out = {origin.x, origin.y, origin.z + 64.0f};
    }
    return true;
}

bool GetAimPunch(sdk::Vector& out) {
    out = {};
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* localPawn = g_LocalPlayerPawn;
    if (!localPawn) {
        return false;
    }

    void* services = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_pAimPunchServices, services) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(services))) {
        return false;
    }

    sdk::Vector punch{};
    if (!memory::Read(reinterpret_cast<uintptr_t>(services) + 0xA4, punch) || !IsFiniteVector(punch)) {
        return false;
    }
    out = punch;
    return true;
}

bool IsVisible(void* entity) {
    if (!entity || !g_LocalPlayerPawn) {
        return false;
    }

    // Check if the entity is dormant (not in PVS)
    bool dormant = false;
    if (memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_bDormant, dormant) && dormant) {
        return false;
    }

    // P1-3: a real BVH ray trace (g_bvh / trace_ray) is not exposed by this
    // SDK dump, so use the networked EntitySpottedState_t::m_bSpotted flag as
    // the visibility signal. It is set by the local player's spotting pass and
    // is strictly better than the old 60-degree FOV cone. Log once so the
    // limitation is visible in the debug log.
    bool spotted = false;
    if (memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_entitySpottedState + sdk::m_bSpotted,
                     spotted)) {
        static bool warnedSpottedFallback = false;
        if (!warnedSpottedFallback) {
            warnedSpottedFallback = true;
            cs2::FileLog("IsVisible: no BVH trace available, using m_bSpotted fallback");
        }
        return spotted;
    }

    return false;
}

bool IsAlive(void* entity) {
    if (!entity) {
        return false;
    }

    int health = 0;
    std::uint8_t lifeState = 1;
    if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_iHealth, health) ||
        !memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_lifeState, lifeState)) {
        return false;
    }
    return health > 0 && health <= 1000 && lifeState == 0;
}

std::uint8_t GetTeam(void* entity) {
    if (!entity) {
        return 0;
    }
    std::uint8_t team = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_iTeamNum, team)) {
        return 0;
    }
    return team;
}

int GetHealth(void* entity) {
    if (!entity) {
        return 0;
    }
    int health = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_iHealth, health)) {
        return 0;
    }
    return health;
}

bool IsValidPlayerUnlocked(void* entity, bool teamCheck) {
    if (!entity) return false;
    void* localPawn = g_LocalPlayerPawn;
    if (entity == localPawn) return false;

    int health = 0;
    std::uint8_t lifeState = 1;
    if (!memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_iHealth, health) ||
        !memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_lifeState, lifeState)) {
        return false;
    }
    if (health <= 0 || health > 1000 || lifeState != 0) return false;

    if (teamCheck) {
        std::uint8_t localTeam = 0;
        std::uint8_t entityTeam = 0;
        memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_iTeamNum, localTeam);
        memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_iTeamNum, entityTeam);
        if (localTeam != 0 && entityTeam == localTeam) return false;
    }
    return true;
}

bool IsValidPlayer(void* entity, bool teamCheck) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    return IsValidPlayerUnlocked(entity, teamCheck);
}

int GetShotsFired() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* localPawn = g_LocalPlayerPawn;
    if (!localPawn) {
        return 0;
    }
    int shots = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_iShotsFired, shots)) {
        return 0;
    }
    return shots;
}

int GetActiveWeaponDefIndex() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* localPawn = g_LocalPlayerPawn;
    if (!localPawn) {
        return 0;
    }

    void* weaponServices = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_pWeaponServices, weaponServices) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(weaponServices))) {
        return 0;
    }

    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weaponServices) + sdk::m_hActiveWeapon, handle)) {
        return 0;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0) {
        return 0;
    }

    void* weapon = GetEntityUnlocked(index);
    if (!weapon) {
        return 0;
    }

    uint16_t definitionIndex = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_iItemDefinitionIndex, definitionIndex)) {
        return 0;
    }
    return static_cast<int>(definitionIndex);
}

void* GetActiveWeapon() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* localPawn = g_LocalPlayerPawn;
    if (!localPawn) {
        return nullptr;
    }

    void* weaponServices = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(localPawn) + sdk::m_pWeaponServices, weaponServices) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(weaponServices))) {
        return nullptr;
    }

    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weaponServices) + sdk::m_hActiveWeapon, handle)) {
        return nullptr;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0) {
        return nullptr;
    }

    return GetEntityUnlocked(index);
}

// ASSUMES g_gameStateMutex IS ALREADY HELD. Never call the public wrapper from
// inside a locked function: std::mutex is non-recursive and would deadlock the
// game thread (that was the in-game freeze/crash).
static void* GetActiveWeaponForPawnUnlocked(void* pawn) {
    if (!pawn) {
        return nullptr;
    }

    void* weaponServices = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(pawn) + sdk::m_pWeaponServices, weaponServices) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(weaponServices))) {
        return nullptr;
    }

    uint32_t handle = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weaponServices) + sdk::m_hActiveWeapon, handle)) {
        return nullptr;
    }
    int index = static_cast<int>(handle & 0x7FFF);
    if (index <= 0) {
        return nullptr;
    }

    return GetEntityUnlocked(index);
}

void* GetActiveWeaponForPawn(void* pawn) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    return GetActiveWeaponForPawnUnlocked(pawn);
}

int GetWeaponClip1(void* pawn) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* weapon = GetActiveWeaponForPawnUnlocked(pawn);
    if (!weapon) {
        return -1;
    }
    int clip = -1;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_iClip1, clip)) {
        return -1;
    }
    return clip;
}

int GetWeaponDefIndex(void* pawn) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* weapon = GetActiveWeaponForPawnUnlocked(pawn);
    if (!weapon) {
        return 0;
    }
    uint16_t defIndex = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_iItemDefinitionIndex, defIndex)) {
        return 0;
    }
    return static_cast<int>(defIndex);
}

const char* GetWeaponName(int defIndex) {
    // Display-only table for the most common weapons. Unknown ids render as
    // "Weapon #N" in the ESP.
    switch (defIndex) {
        case 1:  return "Desert Eagle";
        case 2:  return "Dual Berettas";
        case 3:  return "Five-SeveN";
        case 4:  return "Glock-18";
        case 7:  return "AK-47";
        case 8:  return "AUG";
        case 9:  return "AWP";
        case 10: return "FAMAS";
        case 11: return "G3SG1";
        case 13: return "Galil AR";
        case 16: return "M4A4";
        case 17: return "MAC-10";
        case 18: return "M249";
        case 19: return "P90";
        case 22: return "MP5-SD";
        case 23: return "MP5-Navy";
        case 24: return "P250";
        case 25: return "UMP-45";
        case 27: return "MAG-7";
        case 28: return "Negev";
        case 29: return "Sawed-Off";
        case 30: return "Tec-9";
        case 32: return "XM1014";
        case 33: return "SG553";
        case 34: return "MP9";
        case 38: return "MP7";
        case 39: return "MP9";
        case 40: return "P2000";
        case 41: return "MP5-SD";
        case 46: return "MP7";
        case 59: return "USP";
        case 60: return "M4A1-S";
        case 61: return "USP-S";
        case 63: return "CZ75";
        case 64: return "P2000";
        default:  return "Unknown";
    }
}

float GetFlashDuration(void* pawn) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    if (!pawn) {
        return 0.0f;
    }
    float flash = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(pawn) + sdk::m_flFlashDuration, flash)) {
        return 0.0f;
    }
    return flash;
}

bool IsPawnScoped(void* pawn) {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    if (!pawn) {
        return false;
    }
    bool scoped = false;
    if (!memory::Read(reinterpret_cast<uintptr_t>(pawn) + sdk::m_bIsScoped, scoped)) {
        return false;
    }
    return scoped;
}

std::vector<CachedPlayer> GetCachedPlayers() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    static std::vector<CachedPlayer> cache;
    static uint64_t nextRebuild = 0;

    uint64_t now = GetTickCount64();
    if (now < nextRebuild) {
        return cache;
    }
    nextRebuild = now + 1000;

    cache.clear();
    for (int index = 1; index <= 64; ++index) {
        void* controller = GetEntityUnlocked(index);
        if (!controller) {
            continue;
        }
        void* pawn = GetPlayerPawnUnlocked(controller);
        if (!pawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(pawn))) {
            continue;
        }
        if (!IsValidPlayerUnlocked(pawn, false)) {
            continue;
        }
        cache.push_back({pawn, controller, index});
    }
    return cache;
}

bool CanFire() {
    void* weapon = GetActiveWeapon();
    if (!weapon) {
        return false;
    }

    int clip = 0;
    if (memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_iClip1, clip) && clip <= 0) {
        return false;
    }

    auto* globalVars = GetGlobalVars();
    if (!globalVars) {
        return false;
    }

    int nextTick = 0;
    float ratio = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_nNextPrimaryAttackTick, nextTick) ||
        !memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_flNextPrimaryAttackTickRatio, ratio)) {
        return false;
    }

    // Tick-domain comparison (matches the engine): m_nNextPrimaryAttackTick is
    // compared with CGlobalVars::tickcount (+0x44, verified in client.dll); the
    // sub-tick ratio only matters when both ticks are equal. We do not have the
    // current sub-tick fraction, so on an equal tick we require the ratio to
    // have fully elapsed. This never fires early (at worst one tick late).
    const std::int32_t currentTick = globalVars->tickcount;
    if (currentTick > nextTick) {
        return true;
    }
    if (currentTick == nextTick) {
        return ratio <= 0.0f;
    }
    return false;
}

bool GetControllerName(void* controller, char* out, std::size_t capacity) {
    if (!out || capacity == 0) {
        return false;
    }
    out[0] = '\0';
    if (!controller) {
        return false;
    }

    uintptr_t namePtr = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(controller) + sdk::m_sSanitizedPlayerName, namePtr) ||
        !memory::IsValidPtr(namePtr)) {
        return false;
    }
    return memory::ReadString(namePtr, out, capacity);
}

bool GetPlayerName(void* pawn, char* out, std::size_t capacity) {
    void* controller = GetControllerForPawn(pawn);
    return GetControllerName(controller, out, capacity);
}

int GetLocalRank() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    void* localController = g_LocalPlayerController;
    if (!localController) {
        return 0;
    }
    int rank = 0;
    if (!memory::Read(reinterpret_cast<uintptr_t>(localController) + sdk::m_iCompetitiveRanking, rank)) {
        return 0;
    }
    if (rank < 0 || rank > 18) {
        return 0;
    }
    return rank;
}

bool WorldToScreen(const sdk::Vector& world, sdk::Vector& screen) {
    sdk::ViewMatrix viewMatrix{};
    {
        std::lock_guard<std::mutex> vmLock(g_viewMatrixMutex);
        viewMatrix = g_ViewMatrix;
    }
    int screenWidth = g_ScreenWidth;
    int screenHeight = g_ScreenHeight;
    if (!(viewMatrix.m[3][3] != 0.0f)) {
        static int failCount = 0;
        if (++failCount < 5 || failCount % 100 == 0) {
            cs2::FileLog("WorldToScreen: viewMatrix.m[3][3] == 0 (failCount=%d)", failCount);
        }
        return false;
    }

    float w = viewMatrix.m[3][0] * world.x + viewMatrix.m[3][1] * world.y +
              viewMatrix.m[3][2] * world.z + viewMatrix.m[3][3];
    if (w < 0.01f) {
        return false;
    }

    float x = viewMatrix.m[0][0] * world.x + viewMatrix.m[0][1] * world.y +
              viewMatrix.m[0][2] * world.z + viewMatrix.m[0][3];
    float y = viewMatrix.m[1][0] * world.x + viewMatrix.m[1][1] * world.y +
              viewMatrix.m[1][2] * world.z + viewMatrix.m[1][3];
    float invW = 1.0f / w;

    screen.x = (static_cast<float>(screenWidth) * 0.5f) + (x * invW) * (static_cast<float>(screenWidth) * 0.5f);
    screen.y = (static_cast<float>(screenHeight) * 0.5f) - (y * invW) * (static_cast<float>(screenHeight) * 0.5f);
    screen.z = w;
    return true;
}

void* FindPlantBomb() {
    static void* cachedBomb = nullptr;
    static uint64_t lastScan = 0;
    uint64_t now = GetTickCount64();

    if (cachedBomb && now - lastScan < 500) {
        return cachedBomb;
    }
    cachedBomb = nullptr;
    lastScan = now;

    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    uintptr_t entityList = g_EntityListBase;
    if (!entityList) {
        return nullptr;
    }

    for (int i = 1; i <= 2048; ++i) {
        uintptr_t chunk = 0;
        uintptr_t listOffset = 0x8 * (static_cast<uintptr_t>(i) >> 9) + kEntityChunkListOffset;
        if (!memory::Read(entityList + listOffset, chunk) || !memory::IsValidPtr(chunk)) {
            continue;
        }

        void* entity = nullptr;
        uintptr_t entityOffset = kEntityStride * static_cast<uintptr_t>(i & 0x1FF);
        if (!memory::Read(chunk + entityOffset, entity) ||
            !memory::IsValidPtr(reinterpret_cast<uintptr_t>(entity))) {
            continue;
        }

        uintptr_t pawn = reinterpret_cast<uintptr_t>(entity);
        float c4Blow = 0.0f;
        if (memory::Read(pawn + sdk::m_flC4Blow, c4Blow) && c4Blow > 0.0f) {
            bool ticking = false;
            if (memory::Read(pawn + sdk::m_bBombTicking, ticking) && ticking) {
                cachedBomb = entity;
                return entity;
            }
        }
    }
    return nullptr;
}

bool IsBombPlanted() {
    return FindPlantBomb() != nullptr;
}

float GetBombDefuseTimeRemaining() {
    void* bomb = FindPlantBomb();
    if (!bomb) {
        return 0.0f;
    }

    auto* gv = GetGlobalVars();
    if (!gv) {
        return 0.0f;
    }

    float defuseEndTime = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(bomb) + sdk::m_flDefuseCountDown, defuseEndTime)) {
        return 0.0f;
    }

    return defuseEndTime - gv->curtime;
}

bool IsDefusing() {
    if (!g_LocalPlayerPawn) {
        return false;
    }

    void* bomb = FindPlantBomb();
    if (!bomb) {
        return false;
    }

    void* defuser = nullptr;
    if (!memory::Read(reinterpret_cast<uintptr_t>(bomb) + sdk::m_pBombDefuser, defuser) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(defuser))) {
        return false;
    }

    return defuser == g_LocalPlayerPawn;
}

void* GetDefuserEntity() {
    void* bomb = FindPlantBomb();
    if (!bomb) {
        return nullptr;
    }

    void* defuser = nullptr;
    if (memory::Read(reinterpret_cast<uintptr_t>(bomb) + sdk::m_pBombDefuser, defuser) &&
        memory::IsValidPtr(reinterpret_cast<uintptr_t>(defuser))) {
        return defuser;
    }

    return nullptr;
}

int GetHighestEntityIndex() {
    std::lock_guard<std::mutex> lock(g_gameStateMutex);
    if (!g_EntityListBase) {
        return 64;
    }
    int highest = 0;
    if (!memory::Read(g_EntityListBase + sdk::dwGameEntitySystem_highestEntityIndex, highest)) {
        return 64;
    }
    if (highest < 1 || highest > 0x7FFF) {
        return 64;
    }
    return highest;
}

} // namespace Game
