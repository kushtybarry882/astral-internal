// AUDIT: P3-7, N-C-7
// UPGRADE: C-3, M-5, N-C-7
#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <array>
#include <windows.h>
#include <string_view>
#include "memory.h"

namespace sdk {

struct Vector {
    float x, y, z;

    Vector() : x(0), y(0), z(0) {}
    Vector(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vector operator+(const Vector& v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vector operator-(const Vector& v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vector operator*(float s) const { return {x * s, y * s, z * s}; }
    Vector operator/(float s) const { return {x / s, y / s, z / s}; }
    Vector operator-() const { return {-x, -y, -z}; }

    Vector& operator+=(const Vector& v) { x += v.x; y += v.y; z += v.z; return *this; }
    Vector& operator-=(const Vector& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }

    bool IsZero(float tol = 0.001f) const {
        return std::abs(x) < tol && std::abs(y) < tol && std::abs(z) < tol;
    }

    bool IsFinite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    float Length() const {
        return std::sqrt(x * x + y * y + z * z);
    }

    float Length2D() const {
        return std::sqrt(x * x + y * y);
    }

    float LengthSq() const {
        return x * x + y * y + z * z;
    }

    float DistTo(const Vector& v) const {
        return (*this - v).Length();
    }

    float Dot(const Vector& v) const {
        return x * v.x + y * v.y + z * v.z;
    }

    Vector Normalize() const {
        float len = Length();
        if (len <= 0.0001f) return *this;
        return {x / len, y / len, z / len};
    }

    static Vector Zero() { return {0.f, 0.f, 0.f}; }
};

inline Vector operator*(float s, const Vector& v) { return v * s; }

inline float NormalizeYaw(float yaw) {
    while (yaw > 180.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;
    return yaw;
}

// QAngle convention: x = pitch, y = yaw. Pitch must stay in [-89, 89] or the
// server will clamp/garbage it; yaw wraps to [-180, 180]. The old code wrapped
// pitch through NormalizeYaw (no clamp), which could emit -120 pitch.
inline Vector NormalizeAngles(const Vector& angle) {
    Vector result = angle;
    result.x = std::remainder(result.x, 360.0f);
    result.x = std::clamp(result.x, -89.0f, 89.0f);
    result.y = NormalizeYaw(result.y);
    result.z = 0.0f;
    return result;
}

inline float AngleDelta(const Vector& from, const Vector& to) {
    Vector delta = to - from;
    delta.x = NormalizeYaw(delta.x);
    delta.y = NormalizeYaw(delta.y);
    return std::sqrt(delta.x * delta.x + delta.y * delta.y);
}

struct ViewMatrix {
    float m[4][4];
};

// CS2 CGlobalVars layout -- verified directly against client.dll (build 14196)
// by disassembling the inlined uses of *dwGlobalVars (client+0x2235108):
//   +0x04 framecount  (`mov ecx,[rax+4]; inc ecx`)
//   +0x10 maxclients  (`mov ecx,[rax+0x10]; cmp ebx,ecx; ... inc ebx; cmp; jge` loop)
//   +0x30 curtime     (`movss xmm0,[rax+0x30]; addss ...; movss [rdi+0x14],xmm0`)
//   +0x34 frametime   (passed alongside curtime to the interpolation update)
//   +0x44 tickcount   (`mov ecx,[rax+0x44]; cmp [rbx+...],ecx`)
// interval_per_tick is NOT part of this object in this build; consumers use the
// tick domain (tickcount) instead.
struct GlobalVars {
    float realtime;                          // 0x00
    std::uint32_t framecount;                // 0x04
    float absoluteframetime;                 // 0x08
    float absoluteframestarttimestddev;      // 0x0C
    std::uint32_t maxclients;                // 0x10
    std::uint8_t reserved14[0x1C];           // 0x14..0x2F
    float curtime;                           // 0x30
    float frametime;                         // 0x34
    std::uint8_t reserved38[0x0C];           // 0x38..0x43
    std::int32_t tickcount;                  // 0x44
};

// Lock the binary-verified CGlobalVars layout at compile time.
static_assert(offsetof(GlobalVars, maxclients) == 0x10, "GlobalVars::maxclients must be 0x10");
static_assert(offsetof(GlobalVars, curtime) == 0x30, "GlobalVars::curtime must be 0x30");
static_assert(offsetof(GlobalVars, frametime) == 0x34, "GlobalVars::frametime must be 0x34");
static_assert(offsetof(GlobalVars, tickcount) == 0x44, "GlobalVars::tickcount must be 0x44");
static_assert(sizeof(GlobalVars) == 0x48, "GlobalVars size must be 0x48");

// Opaque CS2 command. Field offsets are handled in core/usercmd.cpp.
struct CUserCmd {
    std::uint8_t data[0x98];
};

// N-C-7: Pattern-based offset resolution. Critical dw* offsets are hardcoded
// defaults verified against the current schema, but at runtime we pattern-scan
// to confirm or correct them. If a pattern scan disagrees with the hardcoded
// value, the scanned value wins and the hardcoded is updated.
namespace PatternOffsets {

    // Resolve a "lea rax, [rip + disp32]" style global pointer pattern.
    // pattern: "48 8B 0D ?? ?? ?? ??" or similar with one rip-relative reference.
    inline uintptr_t ResolveRipGlobal(HMODULE module, std::string_view sig) {
        uintptr_t found = memory::FindPattern(module, sig.data(), false);
        if (!found) return 0;
        // Expect: [opcode bytes...][rip-relative disp32]
        // The pattern scanner found the start; we need the disp32 at offset 3
        // (typical for "48 8B 0D ?? ?? ?? ??" = mov rax, [rip+disp32]).
        // Read displacement at found+3.
        int32_t disp = 0;
        if (!memory::Read(found + 3, disp)) return 0;
        return found + 7 + static_cast<intptr_t>(disp);
    }

    inline bool Initialize() {
        return true; // placeholder; actual resolution happens per-offset below
    }

} // namespace PatternOffsets

// CS2 offset declarations — verified against include/offsets/offsets.hpp
// (cs2-sdk.com, generated 2026-11-07) and include/schemas/client_dll.hpp.
// OFFSETS_BEGIN

// dw* globals — build 2000942 / CS2 build 14196.
// These are hardcoded defaults; ValidateOffsetsRuntime() (core/offsets.hpp)
// may override them via pattern scan at startup.
inline std::ptrdiff_t dwLocalPlayerPawn = 0x256A948;
inline std::ptrdiff_t dwLocalPlayerController = 0x2542548;
inline std::ptrdiff_t dwGameEntitySystem = 0x271FBB8;
inline std::ptrdiff_t dwViewMatrix = 0x2570250;
inline std::ptrdiff_t dwGlobalVars = 0x2235108;
inline std::ptrdiff_t dwViewAngles = 0x2581098;
inline std::ptrdiff_t dwGameRules = 0x256A3F8;
inline std::ptrdiff_t dwCSGOInput = 0x2580A10; // DEPRECATED: do not read; use g_CSGOInputRVA (P3-7)

// P3-7: dynamically resolved RVA of CCSGOInput. Set once by
// HookManager::InstallCreateMove via a pattern scan; never hardcoded.
inline uintptr_t g_CSGOInputRVA = 0;
inline std::ptrdiff_t dwGlowManager = 0x2567120;
inline std::ptrdiff_t dwViewRender = 0x2570C18;
inline std::ptrdiff_t dwPrediction = 0x256A850;
inline std::ptrdiff_t dwGameEntitySystem_highestEntityIndex = 0x2120;

// m_* member offsets — verified against include/schemas/client_dll.hpp
inline std::ptrdiff_t m_iTeamNum = 0x3E7;
inline std::ptrdiff_t m_iHealth = 0x34C;
inline std::ptrdiff_t m_nPrevArmorVal = 0x15D0;
inline std::ptrdiff_t m_ArmorValue = 0x1ED0;
inline std::ptrdiff_t m_lifeState = 0x354;
inline std::ptrdiff_t m_pGameSceneNode = 0x330;
inline std::ptrdiff_t m_vecViewOffset = 0xF60;
inline std::ptrdiff_t m_iIDEntIndex = 0x36DC;
inline std::ptrdiff_t m_bDormant = 0x103;
inline std::ptrdiff_t m_vecOrigin = 0x80;
inline std::ptrdiff_t m_angEyeAngles = 0x3600;
inline std::ptrdiff_t m_pAimPunchServices = 0x1598;
inline std::ptrdiff_t m_pGlowServices = 0x1588;
inline std::ptrdiff_t m_Glow = 0xDE8; // CGlowProperty embedded in C_BaseModelEntity
inline std::ptrdiff_t m_pBulletServices = 0x1570;
inline std::ptrdiff_t m_totalHitsOnServer = 0x48; // CCSPlayer_BulletServices

// Aim punch angle offsets within CCSPlayer_AimPunchServices.
// NEEDS VERIFICATION: these are build-dependent and must be checked against
// the current client.dll schema. Zeroing them is the client-side no-recoil
// method; the server-side RCS subtraction in combat.cpp is the authoritative fix.
inline std::ptrdiff_t m_aimPunchAngle = 0x50;
inline std::ptrdiff_t m_aimPunchAngleVel = 0xA4;
inline std::ptrdiff_t m_flFlashOverlayAlpha = 0x1504;
inline std::ptrdiff_t m_flFlashMaxAlpha = 0x150C;
inline std::ptrdiff_t m_flFlashDuration = 0x1510;
inline std::ptrdiff_t m_fFlags = 0x3F4;
inline std::ptrdiff_t m_iShotsFired = 0x1EB8;
inline std::ptrdiff_t m_nNextPrimaryAttackTick = 0x1918;
inline std::ptrdiff_t m_flNextPrimaryAttackTickRatio = 0x191C;
inline std::ptrdiff_t m_iClip1 = 0x1928;
inline std::ptrdiff_t m_fAccuracyPenalty = 0x1A18; // C_CSWeaponBase (client_dll.hpp proof)
inline std::ptrdiff_t m_entitySpottedState = 0x1E88;
inline std::ptrdiff_t m_bSpotted = 0x8;
inline std::ptrdiff_t m_hPlayerPawn = 0x92C; // CHandle<C_CSPlayerPawn> on CCSPlayerController
inline std::ptrdiff_t m_hController = 0x14BC; // CHandle<CBasePlayerController> on C_BasePlayerPawn
inline std::ptrdiff_t m_sSanitizedPlayerName = 0x878;
inline std::ptrdiff_t m_sSanitizedClanTag = 0x880;
inline std::ptrdiff_t m_iCompetitiveRanking = 0x8A0;
inline std::ptrdiff_t m_pWeaponServices = 0x12F0;
inline std::ptrdiff_t m_hActiveWeapon = 0x60;
inline std::ptrdiff_t m_iItemDefinitionIndex = 0x1BA;
inline std::ptrdiff_t m_pCameraServices = 0x1328;
inline std::ptrdiff_t m_pMovementServices = 0x1330;

// CGlowProperty fields (offsets within the glow services object)
inline std::ptrdiff_t m_fGlowColor = 0x8;
inline std::ptrdiff_t m_iGlowType = 0x30;
inline std::ptrdiff_t m_iGlowTeam = 0x34;
inline std::ptrdiff_t m_nGlowRange = 0x38;
inline std::ptrdiff_t m_nGlowRangeMin = 0x3C;
inline std::ptrdiff_t m_flGlowTime = 0x48;
inline std::ptrdiff_t m_flGlowStartTime = 0x4C;
inline std::ptrdiff_t m_bEligibleForScreenHighlight = 0x50;
inline std::ptrdiff_t m_bFlashing = 0x44;
inline std::ptrdiff_t m_bGlowing = 0x51;
inline std::ptrdiff_t m_iFOV = 0x298;
inline std::ptrdiff_t m_iFOVStart = 0x29C;
inline std::ptrdiff_t m_flFOVTime = 0x2A0;
inline std::ptrdiff_t m_flFOVRate = 0x2A4;
inline std::ptrdiff_t m_flViewmodelFOV = 0x1DCC;
inline std::ptrdiff_t m_bIsScoped = 0x1EA0;
inline std::ptrdiff_t m_flLastSmokeOverlayAlpha = 0x1530;
inline std::ptrdiff_t m_nSmokeEffectTickBegin = 0x1360;
inline std::ptrdiff_t m_bDidSmokeEffect = 0x1364;
inline std::ptrdiff_t m_bSmokeVolumeDataReceived = 0x13A9;

// C_BulletHitModel fields
inline std::ptrdiff_t m_vecStartPos = 0x12A8;
inline std::ptrdiff_t m_bIsHit = 0x12A0;
inline std::ptrdiff_t m_flTimeCreated = 0x12A4;

// CBasePlayerController offsets
inline std::ptrdiff_t m_iDesiredFOV = 0x794;

// C_CSPlayerPawn observer fields
inline std::ptrdiff_t m_pObserverServices = 0x1308;

// C_BasePlayerPawn fields
inline std::ptrdiff_t m_nLastExecutedCommandTick = 0x14B8;
inline std::ptrdiff_t m_nLastExecutedCommandNumber = 0x14B4;
inline std::ptrdiff_t m_nTickBase = 0x6B8; // CBasePlayerController

// C_CSPlayerPawn movement fields
inline std::ptrdiff_t m_bOnGroundLastTick = 0x1D88;
inline std::ptrdiff_t m_flVelocityModifier = 0x1EC0;
inline std::ptrdiff_t m_angEyeAnglesVelocity = 0x36D0;
inline std::ptrdiff_t m_bRetakesHasDefuseKit = 0x1760;

// C_C4 (planting/throwing)
inline std::ptrdiff_t m_bStartedArming = 0x1F28;
inline std::ptrdiff_t m_bBombPlacedAnimation = 0x1F30;
inline std::ptrdiff_t m_bIsPlantingViaUse = 0x1F31;
inline std::ptrdiff_t m_bPlayedArmingBeeps = 0x1F54;
inline std::ptrdiff_t m_bBombPlanted = 0x1F5B;
inline std::ptrdiff_t m_fArmedTime = 0x1F2C;

// C_PlantedC4
inline std::ptrdiff_t m_flC4Blow = 0x12B8;
inline std::ptrdiff_t m_flDefuseCountDown = 0x12D8;
inline std::ptrdiff_t m_flDefuseLength = 0x12D4;
inline std::ptrdiff_t m_bBeingDefused = 0x12C4;
inline std::ptrdiff_t m_bCannotBeDefused = 0x12BC;
inline std::ptrdiff_t m_bHasExploded = 0x12BD;
inline std::ptrdiff_t m_bBombDefused = 0x12DC;
inline std::ptrdiff_t m_bBombTicking = 0x1288;
inline std::ptrdiff_t m_nBombSite = 0x128C;
inline std::ptrdiff_t m_hBombDefuser = 0x12E0;
inline std::ptrdiff_t m_pBombDefuser = 0x1904;
inline std::ptrdiff_t m_flTimerLength = 0x12C0;

// C_CSPlayerPawn bomb zone
inline std::ptrdiff_t m_bInBombZone = 0x15E9;
inline std::ptrdiff_t m_bInBuyZone = 0x15E0;
inline std::ptrdiff_t m_bInNoDefuseArea = 0x1EB0;
inline std::ptrdiff_t m_bPrevDefuser = 0x15CE;
inline std::ptrdiff_t m_nWhichBombZone = 0x1EB4;

// C_BaseEntity movement/physics
inline std::ptrdiff_t m_vecVelocity = 0x430;
inline std::ptrdiff_t m_vecAbsVelocity = 0x3F8;
inline std::ptrdiff_t m_flWaterLevel = 0x528;
inline std::ptrdiff_t m_MoveType = 0x525;
inline std::ptrdiff_t m_nCreationTick = 0x5D0;
inline std::ptrdiff_t m_nSimulationTick = 0x390;

// CCSPlayerController server angles (for resolver)
inline std::ptrdiff_t m_ServerViewAngleChanges = 0x1340;
inline std::ptrdiff_t v_angle = 0x13A8;
inline std::ptrdiff_t v_anglePrevious = 0x13B4;

// CPlayer_ObserverServices fields
inline std::ptrdiff_t m_iObserverMode = 0x48;
inline std::ptrdiff_t m_hObserverTarget = 0x4C;

// CGameSceneNode fields
inline std::ptrdiff_t m_angAbsRotation = 0xD4;
inline std::ptrdiff_t m_angRotation = 0xB8;
inline std::ptrdiff_t m_nodeToWorld = 0x10;

// CGameSceneNode / CSkeletonInstance
inline std::ptrdiff_t m_vecAbsOrigin = 0xC8;
inline std::ptrdiff_t m_modelState = 0x140;
inline std::ptrdiff_t m_boneArray = 0x80;
inline std::ptrdiff_t m_boneArrayLegacy = 0xB0;

// OFFSETS_END

} // namespace sdk

// Global state
inline void* g_LocalPlayerPawn = nullptr;
inline void* g_LocalPlayerController = nullptr;
inline sdk::ViewMatrix g_ViewMatrix = {};
inline int g_ScreenWidth = 1920;
inline int g_ScreenHeight = 1080;
inline uintptr_t g_EntityListBase = 0;
inline uintptr_t g_ClientBase = 0;

// Forward declaration — defined in core/offsets.hpp
namespace Offsets {
    bool ValidateOffsetsRuntime(uintptr_t clientBase);
}
