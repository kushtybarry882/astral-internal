#pragma once

#include "sdk.h"
#include "memory.h"
#include "../core/log.h"

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace Offsets {

inline std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

inline void ApplyOffset(const std::string& name, std::ptrdiff_t offset) {
    #define APPLY(nameStr, ptr) \
        if (name == nameStr) { *ptr = offset; cs2::FileLog("[OFFSETS] %s = 0x%llX", nameStr, (unsigned long long)offset); }

    APPLY("dwLocalPlayerPawn", &sdk::dwLocalPlayerPawn);
    APPLY("dwLocalPlayerController", &sdk::dwLocalPlayerController);
    APPLY("dwGameEntitySystem", &sdk::dwGameEntitySystem);
    APPLY("dwGameEntitySystem_highestEntityIndex", &sdk::dwGameEntitySystem_highestEntityIndex);
    APPLY("dwViewMatrix", &sdk::dwViewMatrix);
    APPLY("dwGlobalVars", &sdk::dwGlobalVars);
    APPLY("dwViewAngles", &sdk::dwViewAngles);
    APPLY("dwGameRules", &sdk::dwGameRules);
    APPLY("dwCSGOInput", &sdk::dwCSGOInput);
    APPLY("dwGlowManager", &sdk::dwGlowManager);
    APPLY("dwViewRender", &sdk::dwViewRender);
    APPLY("dwPrediction", &sdk::dwPrediction);

    APPLY("m_iTeamNum", &sdk::m_iTeamNum);
    APPLY("m_iHealth", &sdk::m_iHealth);
    APPLY("m_lifeState", &sdk::m_lifeState);
    APPLY("m_pGameSceneNode", &sdk::m_pGameSceneNode);
    APPLY("m_vecViewOffset", &sdk::m_vecViewOffset);
    APPLY("m_iIDEntIndex", &sdk::m_iIDEntIndex);
    APPLY("m_bDormant", &sdk::m_bDormant);
    APPLY("m_angEyeAngles", &sdk::m_angEyeAngles);
    APPLY("m_pAimPunchServices", &sdk::m_pAimPunchServices);
    APPLY("m_pGlowServices", &sdk::m_pGlowServices);
    APPLY("m_Glow", &sdk::m_Glow);
    APPLY("m_pBulletServices", &sdk::m_pBulletServices);
    APPLY("m_flFlashOverlayAlpha", &sdk::m_flFlashOverlayAlpha);
    APPLY("m_flFlashMaxAlpha", &sdk::m_flFlashMaxAlpha);
    APPLY("m_flFlashDuration", &sdk::m_flFlashDuration);
    APPLY("m_flLastSmokeOverlayAlpha", &sdk::m_flLastSmokeOverlayAlpha);
    APPLY("m_nSmokeEffectTickBegin", &sdk::m_nSmokeEffectTickBegin);
    APPLY("m_bDidSmokeEffect", &sdk::m_bDidSmokeEffect);
    APPLY("m_bSmokeVolumeDataReceived", &sdk::m_bSmokeVolumeDataReceived);
    APPLY("m_fFlags", &sdk::m_fFlags);
    APPLY("m_iShotsFired", &sdk::m_iShotsFired);
    APPLY("m_flVelocityModifier", &sdk::m_flVelocityModifier);
    APPLY("m_bInNoDefuseArea", &sdk::m_bInNoDefuseArea);
    APPLY("m_nWhichBombZone", &sdk::m_nWhichBombZone);
    APPLY("m_pWeaponServices", &sdk::m_pWeaponServices);
    APPLY("m_hActiveWeapon", &sdk::m_hActiveWeapon);
    APPLY("m_iItemDefinitionIndex", &sdk::m_iItemDefinitionIndex);
    APPLY("m_pCameraServices", &sdk::m_pCameraServices);
    APPLY("m_flViewmodelFOV", &sdk::m_flViewmodelFOV);
    APPLY("m_bIsScoped", &sdk::m_bIsScoped);
    APPLY("m_iFOV", &sdk::m_iFOV);
    APPLY("m_iFOVStart", &sdk::m_iFOVStart);
    APPLY("m_pObserverServices", &sdk::m_pObserverServices);
    APPLY("m_iObserverMode", &sdk::m_iObserverMode);
    APPLY("m_hObserverTarget", &sdk::m_hObserverTarget);
    APPLY("m_vecVelocity", &sdk::m_vecVelocity);
    APPLY("m_vecAbsVelocity", &sdk::m_vecAbsVelocity);
    APPLY("m_nPrevArmorVal", &sdk::m_nPrevArmorVal);
    APPLY("m_ArmorValue", &sdk::m_ArmorValue);
    APPLY("m_hController", &sdk::m_hController);
    APPLY("m_hPlayerPawn", &sdk::m_hPlayerPawn);
    APPLY("m_nTickBase", &sdk::m_nTickBase);
    APPLY("m_sSanitizedPlayerName", &sdk::m_sSanitizedPlayerName);
    APPLY("m_sSanitizedClanTag", &sdk::m_sSanitizedClanTag);
    APPLY("m_iCompetitiveRanking", &sdk::m_iCompetitiveRanking);
    APPLY("m_nNextPrimaryAttackTick", &sdk::m_nNextPrimaryAttackTick);
    APPLY("m_flNextPrimaryAttackTickRatio", &sdk::m_flNextPrimaryAttackTickRatio);
    APPLY("m_iClip1", &sdk::m_iClip1);
    APPLY("m_entitySpottedState", &sdk::m_entitySpottedState);
    APPLY("m_bSpotted", &sdk::m_bSpotted);
    APPLY("m_bInBombZone", &sdk::m_bInBombZone);
    APPLY("m_bInBuyZone", &sdk::m_bInBuyZone);
    APPLY("m_bPrevDefuser", &sdk::m_bPrevDefuser);
    APPLY("m_flC4Blow", &sdk::m_flC4Blow);
    APPLY("m_flDefuseCountDown", &sdk::m_flDefuseCountDown);
    APPLY("m_flDefuseLength", &sdk::m_flDefuseLength);
    APPLY("m_bBeingDefused", &sdk::m_bBeingDefused);
    APPLY("m_bCannotBeDefused", &sdk::m_bCannotBeDefused);
    APPLY("m_bHasExploded", &sdk::m_bHasExploded);
    APPLY("m_bBombDefused", &sdk::m_bBombDefused);
    APPLY("m_bBombTicking", &sdk::m_bBombTicking);
    APPLY("m_nBombSite", &sdk::m_nBombSite);
    APPLY("m_hBombDefuser", &sdk::m_hBombDefuser);
    APPLY("m_pBombDefuser", &sdk::m_pBombDefuser);
    APPLY("m_flTimerLength", &sdk::m_flTimerLength);
    APPLY("m_modelState", &sdk::m_modelState);
    APPLY("m_boneArray", &sdk::m_boneArray);
    APPLY("m_boneArrayLegacy", &sdk::m_boneArrayLegacy);
    APPLY("m_vecAbsOrigin", &sdk::m_vecAbsOrigin);
    APPLY("m_nodeToWorld", &sdk::m_nodeToWorld);
    APPLY("m_angAbsRotation", &sdk::m_angAbsRotation);
    APPLY("m_angRotation", &sdk::m_angRotation);
    APPLY("m_nLastExecutedCommandTick", &sdk::m_nLastExecutedCommandTick);
    APPLY("m_nLastExecutedCommandNumber", &sdk::m_nLastExecutedCommandNumber);
    APPLY("m_bRetakesHasDefuseKit", &sdk::m_bRetakesHasDefuseKit);
    APPLY("m_angEyeAnglesVelocity", &sdk::m_angEyeAnglesVelocity);
    APPLY("m_ServerViewAngleChanges", &sdk::m_ServerViewAngleChanges);
    APPLY("v_angle", &sdk::v_angle);
    APPLY("v_anglePrevious", &sdk::v_anglePrevious);
    APPLY("m_iDesiredFOV", &sdk::m_iDesiredFOV);
    APPLY("m_pMovementServices", &sdk::m_pMovementServices);
    APPLY("m_vecOrigin", &sdk::m_vecOrigin);
    APPLY("m_flWaterLevel", &sdk::m_flWaterLevel);
    APPLY("m_MoveType", &sdk::m_MoveType);
    APPLY("m_nCreationTick", &sdk::m_nCreationTick);
    APPLY("m_nSimulationTick", &sdk::m_nSimulationTick);

    #undef APPLY
}

// Parses offsets from the gamehacks folder dump (offsets.txt / schema format).
// Falls back to hardcoded defaults in sdk.h when the dump is absent or stale.
// Reads lines of the form:
//   dwLocalPlayerPawn = 0x256A948
//   m_iHealth = 0x34C
inline void ParseFromGamehacksFolder() {
    namespace fs = std::filesystem;

    // Candidate locations for the offsets dump.
    std::vector<std::filesystem::path> searchPaths = {
        std::filesystem::current_path() / "offsets.txt",
        std::filesystem::current_path() / "gamehacks" / "offsets.txt",
        std::filesystem::current_path() / "dump" / "offsets.txt",
    };

    // Also check %LOCALAPPDATA%\ASTRAL\offsets.txt
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        searchPaths.push_back(std::filesystem::path(localAppData) / "ASTRAL" / "offsets.txt");
    }

    std::filesystem::path offsetsPath;
    for (const auto& p : searchPaths) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            offsetsPath = p;
            break;
        }
    }

    if (offsetsPath.empty()) {
        cs2::FileLog("[OFFSETS] No offsets dump found — using hardcoded defaults from sdk.h");
        return;
    }

    std::ifstream file(offsetsPath);
    if (!file.is_open()) {
        cs2::FileLog("[OFFSETS] Failed to open %s — using hardcoded defaults", offsetsPath.string().c_str());
        return;
    }

    int parsed = 0;
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);

        if (line.empty() || line[0] == '#' || line[0] == '/') {
            continue;
        }

        size_t sep = line.find('=');
        if (sep == std::string::npos) {
            continue;
        }

        std::string name = Trim(line.substr(0, sep));
        std::string value = Trim(line.substr(sep + 1));

        // Strip "0x" prefix if present.
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
            value = value.substr(2);
        }

        char* endPtr = nullptr;
        unsigned long long off = std::strtoull(value.c_str(), &endPtr, 16);
        if (endPtr == value.c_str()) {
            continue; // not a hex number
        }

        ApplyOffset(name, static_cast<std::ptrdiff_t>(off));
        ++parsed;
    }

    cs2::FileLog("[OFFSETS] Parsed %d offsets from %s", parsed, offsetsPath.string().c_str());
}

} // namespace Offsets