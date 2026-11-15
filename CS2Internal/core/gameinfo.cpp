#include "gameinfo.h"
#include "game.h"
#include "memory.h"
#include "sdk.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace GameInfo {

namespace {

constexpr const char* kLevelNamePattern =
    "48 83 EC ? E8 ? ? ? ? 84 C0 74 ? 48 8D 05 ? ? ? ? 48 83 C4 ? C3 "
    "48 8B 0D ? ? ? ? 48 85 C9 74 ? 83 B9 ? ? ? ? ? 7C ? 48 8B 89 ? ? ? ? "
    "48 8D 05 ? ? ? ? 48 85 C9 48 0F 45 C1 48 83 C4 ? C3 48 8D 05 ? ? ? ? "
    "48 83 C4 ? C3";

using GetLevelName_t = const char* (__fastcall*)();

char g_playerName[64] = "Astral_User";
char g_rankName[32] = "Unranked";
char g_mapName[64] = "Unknown";

uintptr_t g_levelNameFn = 0;
std::chrono::steady_clock::time_point g_nextLevelNameScan = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_nextMapRefresh = std::chrono::steady_clock::now();

const char* CallLevelName(GetLevelName_t function) {
    if (!function) {
        return nullptr;
    }
    __try {
        return function();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void UpdateMapName() {
    auto now = std::chrono::steady_clock::now();
    if (now < g_nextMapRefresh) {
        return;
    }
    g_nextMapRefresh = now + std::chrono::milliseconds(1000);

    if (!g_levelNameFn) {
        if (now < g_nextLevelNameScan) {
            return;
        }
        g_nextLevelNameScan = now + std::chrono::milliseconds(5000);

        HMODULE engine = GetModuleHandleA("engine2.dll");
        if (!engine) {
            return;
        }
        g_levelNameFn = memory::FindPattern(engine, kLevelNamePattern);
        if (!g_levelNameFn) {
            return;
        }
    }

    const char* levelName = CallLevelName(reinterpret_cast<GetLevelName_t>(g_levelNameFn));
    if (!levelName) {
        return;
    }

    char buffer[128] = {};
    if (!memory::ReadString(reinterpret_cast<uintptr_t>(levelName), buffer, sizeof(buffer))) {
        return;
    }

    std::string name = buffer;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".bsp") == 0) {
        name.resize(name.size() - 4);
    }
    if (name.empty()) {
        return;
    }

    std::snprintf(g_mapName, sizeof(g_mapName), "%s", name.c_str());
}

const char* RankForValue(int rank) {
    static const char* ranks[19] = {
        "Unranked",
        "Silver I", "Silver II", "Silver III", "Silver IV", "Silver Elite", "Silver Elite Master",
        "Gold Nova I", "Gold Nova II", "Gold Nova III", "Gold Nova Master",
        "MG1", "MG2", "MGE", "DMG", "LE", "LEM", "SMFC", "Global Elite"
    };
    if (rank < 0 || rank >= 19) {
        return "Unranked";
    }
    return ranks[rank];
}

void UpdatePlayerInfo() {
    bool haveName = false;
    if (g_LocalPlayerController) {
        char buffer[64] = {};
        if (Game::GetControllerName(g_LocalPlayerController, buffer, sizeof(buffer))) {
            std::snprintf(g_playerName, sizeof(g_playerName), "%s", buffer);
            haveName = true;
        }
    }
    if (!haveName) {
        std::snprintf(g_playerName, sizeof(g_playerName), "%s", "Astral_User");
    }

    std::snprintf(g_rankName, sizeof(g_rankName), "%s", RankForValue(Game::GetLocalRank()));
}

} // namespace

void Refresh() {
    // Throttle name/rank/map reads to 500 ms (was every frame).
    auto now = std::chrono::steady_clock::now();
    static std::chrono::steady_clock::time_point g_lastRefresh = std::chrono::steady_clock::now();
    if (now - g_lastRefresh < std::chrono::milliseconds(500)) {
        return;
    }
    g_lastRefresh = now;

    UpdatePlayerInfo();
    UpdateMapName();
}

const char* PlayerName() {
    return g_playerName;
}

const char* RankName() {
    return g_rankName;
}

const char* MapName() {
    return g_mapName;
}

} // namespace GameInfo
