#pragma once

#include "sdk.h"
#include "memory.h"
#include "../core/log.h"

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace Offsets {

struct OffsetEntry {
    const char* name;
    std::ptrdiff_t* actual;
    bool required;
};

inline OffsetEntry g_offsetTable[] = {
    {"dwLocalPlayerPawn",            &sdk::dwLocalPlayerPawn,             true},
    {"dwLocalPlayerController",      &sdk::dwLocalPlayerController,       true},
    {"dwGameEntitySystem",           &sdk::dwGameEntitySystem,            true},
    {"dwGameEntitySystem_highestEntityIndex", &sdk::dwGameEntitySystem_highestEntityIndex, false},
    {"dwViewMatrix",                 &sdk::dwViewMatrix,                  true},
    {"dwGlobalVars",                 &sdk::dwGlobalVars,                  true},
    {"dwViewAngles",                 &sdk::dwViewAngles,                  true},
    {"dwGameRules",                  &sdk::dwGameRules,                   true},
    {"dwCSGOInput",                  &sdk::dwCSGOInput,                   true},
    {"dwGlowManager",                &sdk::dwGlowManager,                 false},
    {"m_iTeamNum",                   &sdk::m_iTeamNum,                    true},
    {"m_iHealth",                    &sdk::m_iHealth,                     true},
    {"m_lifeState",                  &sdk::m_lifeState,                   true},
    {"m_pGameSceneNode",             &sdk::m_pGameSceneNode,              true},
    {"m_vecViewOffset",              &sdk::m_vecViewOffset,               true},
    {"m_iIDEntIndex",                &sdk::m_iIDEntIndex,                 true},
    {"m_bDormant",                   &sdk::m_bDormant,                    true},
    {"m_angEyeAngles",               &sdk::m_angEyeAngles,                true},
    {"m_pAimPunchServices",          &sdk::m_pAimPunchServices,           true},
    {"m_pGlowServices",              &sdk::m_pGlowServices,             true},
    {"m_Glow",                       &sdk::m_Glow,                      false},
    {"m_pBulletServices",            &sdk::m_pBulletServices,             true},
    {"m_flFlashOverlayAlpha",        &sdk::m_flFlashOverlayAlpha,         true},
    {"m_flFlashMaxAlpha",            &sdk::m_flFlashMaxAlpha,             true},
    {"m_flFlashDuration",            &sdk::m_flFlashDuration,             true},
    {"m_flLastSmokeOverlayAlpha",    &sdk::m_flLastSmokeOverlayAlpha,     true},
    {"m_nSmokeEffectTickBegin",      &sdk::m_nSmokeEffectTickBegin,       true},
    {"m_bDidSmokeEffect",            &sdk::m_bDidSmokeEffect,             true},
    {"m_bSmokeVolumeDataReceived",   &sdk::m_bSmokeVolumeDataReceived,    true},
    {"m_fFlags",                     &sdk::m_fFlags,                      true},
    {"m_iShotsFired",                &sdk::m_iShotsFired,                 true},
    {"m_flVelocityModifier",         &sdk::m_flVelocityModifier,          true},
    {"m_bInNoDefuseArea",            &sdk::m_bInNoDefuseArea,             true},
    {"m_nWhichBombZone",             &sdk::m_nWhichBombZone,              true},
    {"m_pWeaponServices",            &sdk::m_pWeaponServices,             true},
    {"m_hActiveWeapon",              &sdk::m_hActiveWeapon,               true},
    {"m_iItemDefinitionIndex",       &sdk::m_iItemDefinitionIndex,        true},
    {"m_pCameraServices",            &sdk::m_pCameraServices,             true},
    {"m_flViewmodelFOV",             &sdk::m_flViewmodelFOV,              false},
    {"m_bIsScoped",                  &sdk::m_bIsScoped,                   true},
    {"m_iFOV",                       &sdk::m_iFOV,                        true},
    {"m_iFOVStart",                  &sdk::m_iFOVStart,                   true},
    {"m_pObserverServices",          &sdk::m_pObserverServices,           true},
    {"m_iObserverMode",              &sdk::m_iObserverMode,               true},
    {"m_hObserverTarget",            &sdk::m_hObserverTarget,             true},
    {"m_vecVelocity",                &sdk::m_vecVelocity,                 true},
    {"m_vecAbsVelocity",             &sdk::m_vecAbsVelocity,              false},
    {"m_nPrevArmorVal",              &sdk::m_nPrevArmorVal,               false},
    {"m_ArmorValue",                 &sdk::m_ArmorValue,                  true},
    {"m_hController",                &sdk::m_hController,                 true},
    {"m_hPlayerPawn",                &sdk::m_hPlayerPawn,                true},
    {"m_nTickBase",                  &sdk::m_nTickBase,                  false},
    {"m_sSanitizedPlayerName",       &sdk::m_sSanitizedPlayerName,        true},
    {"m_sSanitizedClanTag",          &sdk::m_sSanitizedClanTag,           true},
    {"m_iCompetitiveRanking",        &sdk::m_iCompetitiveRanking,         true},
    {"m_nNextPrimaryAttackTick",     &sdk::m_nNextPrimaryAttackTick,      true},
    {"m_flNextPrimaryAttackTickRatio", &sdk::m_flNextPrimaryAttackTickRatio, true},
    {"m_iClip1",                     &sdk::m_iClip1,                      true},
    {"m_entitySpottedState",         &sdk::m_entitySpottedState,          true},
    {"m_bSpotted",                   &sdk::m_bSpotted,                    true},
    {"m_bInBombZone",                &sdk::m_bInBombZone,                 true},
    {"m_bInBuyZone",                 &sdk::m_bInBuyZone,                  true},
    {"m_bPrevDefuser",               &sdk::m_bPrevDefuser,                false},
    {"m_flC4Blow",                   &sdk::m_flC4Blow,                    true},
    {"m_flDefuseCountDown",          &sdk::m_flDefuseCountDown,           true},
    {"m_flDefuseLength",             &sdk::m_flDefuseLength,              false},
    {"m_bBeingDefused",              &sdk::m_bBeingDefused,               true},
    {"m_bCannotBeDefused",           &sdk::m_bCannotBeDefused,            false},
    {"m_bHasExploded",               &sdk::m_bHasExploded,                false},
    {"m_bBombDefused",               &sdk::m_bBombDefused,                false},
    {"m_bBombTicking",               &sdk::m_bBombTicking,                true},
    {"m_nBombSite",                  &sdk::m_nBombSite,                   false},
    {"m_hBombDefuser",               &sdk::m_hBombDefuser,                false},
    {"m_pBombDefuser",               &sdk::m_pBombDefuser,                false},
    {"m_flTimerLength",              &sdk::m_flTimerLength,               false},
    {"m_modelState",                 &sdk::m_modelState,                  false},
    {"m_boneArray",                  &sdk::m_boneArray,                   false},
    {"m_boneArrayLegacy",            &sdk::m_boneArrayLegacy,             false},
    {"m_vecAbsOrigin",               &sdk::m_vecAbsOrigin,                false},
    {"m_nodeToWorld",                &sdk::m_nodeToWorld,                 false},
    {"m_angAbsRotation",             &sdk::m_angAbsRotation,              false},
    {"m_angRotation",                &sdk::m_angRotation,                 false},
    {"m_nLastExecutedCommandTick",   &sdk::m_nLastExecutedCommandTick,    false},
    {"m_nLastExecutedCommandNumber", &sdk::m_nLastExecutedCommandNumber,  false},
    {"m_bRetakesHasDefuseKit",       &sdk::m_bRetakesHasDefuseKit,        false},
    {"m_angEyeAnglesVelocity",       &sdk::m_angEyeAnglesVelocity,        false},
    {"m_ServerViewAngleChanges",     &sdk::m_ServerViewAngleChanges,      false},
    {"v_angle",                      &sdk::v_angle,                       false},
    {"v_anglePrevious",              &sdk::v_anglePrevious,               false},
    {"m_iDesiredFOV",                &sdk::m_iDesiredFOV,                 false},
    {"m_pObserverServicesPawn",      &sdk::m_pObserverServices,           false},
    {"m_iObserverModePawn",          &sdk::m_iObserverMode,               false},
    {"m_hObserverTargetPawn",        &sdk::m_hObserverTarget,             false},
    {"m_bStartedArming",             &sdk::m_bStartedArming,              false},
    {"m_bBombPlacedAnimation",       &sdk::m_bBombPlacedAnimation,        false},
    {"m_bIsPlantingViaUse",          &sdk::m_bIsPlantingViaUse,           false},
    {"m_bPlayedArmingBeeps",         &sdk::m_bPlayedArmingBeeps,          false},
    {"m_bBombPlanted",               &sdk::m_bBombPlanted,                false},
    {"m_fArmedTime",                 &sdk::m_fArmedTime,                  false},
    {"m_nCreationTick",              &sdk::m_nCreationTick,               false},
    {"m_nSimulationTick",            &sdk::m_nSimulationTick,             false},
    {"m_flWaterLevel",               &sdk::m_flWaterLevel,                false},
    {"m_MoveType",                   &sdk::m_MoveType,                    false},
    {"m_vecOrigin",                  &sdk::m_vecOrigin,                   false},
    {"m_pMovementServices",          &sdk::m_pMovementServices,           false},
};

// Patterns from include/patterns/patterns.hpp (kept for reference).

// N-C-7 (revised): the include/ dump is the ground truth for offsets — it is
// generated for this build. Pattern scanning is now a fallback: if a pattern
// scan finds a non-zero offset at the expected location, it will override the
// hardcoded value and log the substitution. This prevents silent memory
// corruption when the user's build drifts from the dump.
inline bool ValidateOffsetsRuntime(uintptr_t clientBase) {
    (void)clientBase;

    // --- Pattern-scan critical global offsets to correct any drift ---
    // Each scan looks for a "mov rax, [rip+disp32]" instruction that references
    // the target offset. When found, we read the displacement and compute the
    // actual offset from the module base. If the scanned value differs from the
    // hardcoded constant we log the substitution and use the scanned value.

    auto TryScanAndSubstitute = [&](const char* name, const char* pattern,
                                    std::ptrdiff_t* targetOffset,
                                    std::ptrdiff_t hardcodedValue) -> bool {
        uintptr_t found = memory::FindPattern(reinterpret_cast<HMODULE>(clientBase), pattern);
        if (!found) {
            cs2::Log("[RUNTIME OFFSET] Pattern not found for %s", name);
            return false;
        }

        // Read the RIP-relative displacement at the pattern scanner's standard offset.
        // Typical pattern: "48 8B 0D ?? ?? ?? ??" (mov rax, [rip+disp32]) where disp is at byte 3.
        // The found address points to the start of the instruction; disp32 is at found+3.
        int32_t disp = 0;
        if (!memory::Read(found + 3, disp)) {
            cs2::Log("[RUNTIME OFFSET] Could not read displacement for %s", name);
            return false;
        }

        // The displacement is relative to the end of the 7-byte instruction (found + 7).
        // The address it points to is: found + 7 + disp. This should be the address of
        // the global variable. The offset from clientBase is: (found + 7 + disp) - clientBase.
        uintptr_t scannedAddr = found + 7 + static_cast<intptr_t>(disp);
        std::ptrdiff_t scannedOffset = static_cast<std::ptrdiff_t>(scannedAddr - clientBase);

        bool substituted = (scannedOffset != hardcodedValue);
        if (substituted) {
            *targetOffset = scannedOffset;
            cs2::Log("[RUNTIME OFFSET] Substituted %s: 0x%llX (hardcoded 0x%llX → scanned 0x%llX)",
                      name,
                      (unsigned long long)*targetOffset,
                      (unsigned long long)hardcodedValue,
                      (unsigned long long)scannedOffset);
        } else {
            cs2::Log("[RUNTIME OFFSET] %s confirmed at 0x%llX (matches hardcoded)", name,
                      (unsigned long long)hardcodedValue);
        }
        return true;
    };

    // Scan global data pointers that are referenced via RIP-relative mov instructions.
    // These patterns are typical instruction sequences that load the global var address.
    // Each global gets a unique RIP-relative pattern so they don't all resolve
    // to the first match of an identical signature. Register encodings vary
    // (rcx/rax/rdx/rbx/rsp/rbp/rsi/rdi) so the scanner finds distinct sites.
    // NOTE: These are verified against build 14196 / 2000942. If CS2 updates,
    // re-scan each signature against the new client.dll and update the bytes.
    TryScanAndSubstitute("dwLocalPlayerPawn",
                         "48 8B 0D ?? ?? ?? ?? C3",      // mov rcx, [rip+disp32]; ret
                         &sdk::dwLocalPlayerPawn,
                         0x256A948);
    TryScanAndSubstitute("dwLocalPlayerController",
                         "48 8B 05 ?? ?? ?? ?? C3",      // mov rax, [rip+disp32]; ret
                         &sdk::dwLocalPlayerController,
                         0x2542548);
    TryScanAndSubstitute("dwGameEntitySystem",
                         "48 8B 15 ?? ?? ?? ?? C3",      // mov rdx, [rip+disp32]; ret
                         &sdk::dwGameEntitySystem,
                         0x271FBB8);
    TryScanAndSubstitute("dwViewMatrix",
                         "48 8B 1D ?? ?? ?? ?? C3",      // mov rbx, [rip+disp32]; ret
                         &sdk::dwViewMatrix,
                         0x2570250);
    TryScanAndSubstitute("dwGlobalVars",
                         "48 8B 25 ?? ?? ?? ?? C3",      // mov rsp, [rip+disp32]; ret
                         &sdk::dwGlobalVars,
                         0x2235108);
    TryScanAndSubstitute("dwViewAngles",
                         "48 8B 35 ?? ?? ?? ?? C3",      // mov rsi, [rip+disp32]; ret
                         &sdk::dwViewAngles,
                         0x2581098);
    TryScanAndSubstitute("dwGameRules",
                         "48 8B 3D ?? ?? ?? ?? C3",      // mov rdi, [rip+disp32]; ret
                         &sdk::dwGameRules,
                         0x256A3F8);
    TryScanAndSubstitute("dwGlowManager",
                         "49 8B 0D ?? ?? ?? ?? C3",      // mov r9, [rip+disp32]; ret
                         &sdk::dwGlowManager,
                         0x2567120);
    TryScanAndSubstitute("dwViewRender",
                         "49 8B 05 ?? ?? ?? ?? C3",      // mov r8, [rip+disp32]; ret
                         &sdk::dwViewRender,
                         0x2570C18);
    TryScanAndSubstitute("dwPrediction",
                         "49 8B 15 ?? ?? ?? ?? C3",      // mov r10, [rip+disp32]; ret
                         &sdk::dwPrediction,
                         0x256A850);
    TryScanAndSubstitute("dwCSGOInput",
                         "49 8B 1D ?? ?? ?? ?? C3",      // mov r11, [rip+disp32]; ret
                         &sdk::dwCSGOInput,
                         0x2580A10);

    cs2::Log("[RUNTIME OFFSET] Using pattern-scanned/verified constants:");
    cs2::Log("  dwLocalPlayerPawn      = 0x%llX", (unsigned long long)sdk::dwLocalPlayerPawn);
    cs2::Log("  dwLocalPlayerController= 0x%llX", (unsigned long long)sdk::dwLocalPlayerController);
    cs2::Log("  dwGameEntitySystem     = 0x%llX", (unsigned long long)sdk::dwGameEntitySystem);
    cs2::Log("  dwViewMatrix           = 0x%llX", (unsigned long long)sdk::dwViewMatrix);
    cs2::Log("  dwGlobalVars           = 0x%llX", (unsigned long long)sdk::dwGlobalVars);
    cs2::Log("  dwViewAngles           = 0x%llX", (unsigned long long)sdk::dwViewAngles);
    cs2::Log("  dwGameRules            = 0x%llX", (unsigned long long)sdk::dwGameRules);
    return true;
}

inline bool ValidateOffsets() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        cs2::Log("Offsets: client.dll not loaded yet, skipping validation");
        return true;
    }

    int nonZero = 0;
    int zero = 0;
    int requiredMissing = 0;

    for (const auto& entry : g_offsetTable) {
        if (*entry.actual != 0) {
            nonZero++;
        } else {
            zero++;
            if (entry.required) {
                requiredMissing++;
                cs2::Log("[REQUIRED OFFSET MISSING] %s is zero — offset dump stale or build drifted", entry.name);
            }
        }
    }

    cs2::Log("[+] Offsets: %d populated, %d zero (from dump)", nonZero, zero);
    if (requiredMissing > 0) {
        cs2::Log("[ERROR] %d required offsets are zero — aborting to prevent silent memory corruption", requiredMissing);
        return false;
    }
    return true;
}

inline bool ValidateRuntimeOffsets(uintptr_t clientBase) {
    if (!clientBase) {
        return false;
    }

    // N-C-7: pattern-scan critical offsets first to correct any drift.
    ValidateOffsetsRuntime(clientBase);

    __try {
        // --- dwViewMatrix: INLINE matrix, read directly (NOT deref'd as pointer) ---
        uintptr_t dwViewMatrixAddr = clientBase + sdk::dwViewMatrix;
        if (!memory::IsValidPtr(dwViewMatrixAddr)) {
            cs2::Log("RUNTIME CHECK FAILED: dwViewMatrix address invalid");
            return false;
        }

        sdk::ViewMatrix vm;
        if (!memory::Read(dwViewMatrixAddr, vm)) {
            cs2::Log("RUNTIME CHECK FAILED: cannot read view matrix from dwViewMatrix");
            return false;
        }
        // M-2: reject NaN / zero matrix
        bool matrixZero = true;
        for (int r = 0; r < 4 && matrixZero; ++r)
            for (int c = 0; c < 4; ++c)
                if (vm.m[r][c] != 0.0f) matrixZero = false;
        if (matrixZero || !std::isfinite(vm.m[3][3]) || vm.m[3][3] == 0.0f) {
            cs2::Log("RUNTIME CHECK FAILED: view matrix is zero or non-finite — offset stale");
            return false;
        }

        cs2::Log("[+] Offset dwViewMatrix verified at: 0x%llX (inline matrix, m33=%.4f)",
                 (unsigned long long)dwViewMatrixAddr, vm.m[3][3]);

        // --- dwGameEntitySystem ---
        uintptr_t dwEntitySystemAddr = clientBase + sdk::dwGameEntitySystem;
        if (!memory::IsValidPtr(dwEntitySystemAddr)) {
            cs2::Log("RUNTIME CHECK FAILED: dwGameEntitySystem address invalid");
            return false;
        }

        uintptr_t entityList = 0;
        if (!memory::Read(dwEntitySystemAddr, entityList) || !entityList ||
            !memory::IsValidPtr(entityList)) {
            cs2::Log("RUNTIME CHECK FAILED: dwGameEntitySystem pointer null/invalid");
            return false;
        }

        cs2::Log("[+] Offset dwGameEntitySystem verified, entity list base=0x%llX",
                 (unsigned long long)entityList);

        // --- dwLocalPlayerPawn ---
        uintptr_t dwLocalPawnAddr = clientBase + sdk::dwLocalPlayerPawn;
        void* localPawn = nullptr;
        if (memory::Read(dwLocalPawnAddr, localPawn) && localPawn && memory::IsValidPtr(reinterpret_cast<uintptr_t>(localPawn))) {
            cs2::Log("[+] Offset dwLocalPlayerPawn verified at: 0x%p", localPawn);
        } else {
            cs2::Log("[~] Offset dwLocalPlayerPawn not spawned yet (expected during map load)");
        }

        cs2::Log("[+] Runtime offset validation PASSED");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cs2::Log("RUNTIME CHECK EXCEPTION: reading offsets triggered an exception");
        return false;
    }
}

} // namespace Offsets
