// AUDIT: P0-1, P0-6, P0-8, P1-2, P1-6, P1-12, P3-9, P3-10, P4-5
// UPGRADE: V-1, V-2, V-3, V-4, V-5, P-3, 0-3
#include "visuals.h"
#include "../core/config.h"
#include "../core/game.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../core/security.h"
#include "../core/usercmd.h"

#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace Visuals {

namespace {

constexpr ImU32 kMint = IM_COL32(32, 201, 151, 255);
constexpr ImU32 kText = IM_COL32(233, 236, 239, 255);
constexpr ImU32 kMuted = IM_COL32(108, 117, 125, 255);
constexpr ImU32 kBlack = IM_COL32(8, 8, 10, 215);

ImU32 ToColor(const float (&color)[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], color[3]));
}

ImU32 ToColorOpaque(const float (&color)[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], 1.0f));
}

} // namespace
struct BulletTracer {
    sdk::Vector start;
    sdk::Vector end;
    float timeCreated;
    float duration;
};

std::deque<BulletTracer> g_tracers;

// V-5: per-player glow cache. We still need to reach the glow object every
// refresh to keep the effect alive, but we skip all VirtualProtect writes when
// the chosen color/alpha/range for that player has not changed.
static uintptr_t g_glowPawn[65] = {};
static uint32_t g_glowHash[65] = {};

uint32_t GlowConfigHash(const float (&color)[4], int range) {
    uint32_t h = 2166136261u; // FNV-1-ish
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
    auto mixF = [&](float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, sizeof(u));
        mix(u);
    };
    for (float c : color) {
        mixF(c);
    }
    mix(static_cast<uint32_t>(range));
    return h;
}

// V-5/V-3 helper: writes the glow object. Kept separate from ApplyGlow()
// because __try cannot wrap code with unwindable objects.
void WriteGlowObject(uintptr_t glowObj, const float (&color)[4], int glowRange) {
    float alpha = color[3];
    float glowTime = 0.5f;
    auto* globalVars = Game::GetGlobalVars();

    __try {
        memory::Write(glowObj + sdk::m_fGlowColor, sdk::Vector{ color[0], color[1], color[2] });
        memory::Write(glowObj + sdk::m_fGlowColor + 0xC, alpha);

        // m_nGlowRange (0x38) - range from config
        memory::Write(glowObj + sdk::m_nGlowRange, glowRange);

        // m_bGlowing (0x51) - enable glow
        memory::Write(glowObj + sdk::m_bGlowing, true);

        // m_flGlowTime (0x48) - pulsing glow
        memory::Write(glowObj + sdk::m_flGlowTime, glowTime);

        // m_flGlowStartTime (0x4C) - start time for pulsing
        if (globalVars) {
            memory::Write(glowObj + sdk::m_flGlowStartTime, globalVars->curtime);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// V-3: chams is delivered through the engine glow path (m_Glow) --- the same
// path ApplyGlow already used, but now with chams colors/brightness. A true
// material override would require hooking DrawModelExecute, which is not
// available in this dump (see UPGRADE_NOTES.md).
void ApplyGlow() {
    if (!Config::glowEnabled && !Config::chamsEnabled) {
        return;
    }

    Game::GameState state = Game::GetStateSnapshot();
    if (!state.clientBase || !g_LocalPlayerPawn) {
        return;
    }

    std::uint8_t localTeam = Game::GetTeam(g_LocalPlayerPawn);

    // V-1: iterate the shared 1 Hz cache instead of the 64-entity scan.
    for (const Game::CachedPlayer& cp : Game::GetCachedPlayers()) {
        void* entity = cp.pawn;
        int cacheIndex = Game::GetEntityIndex(entity);
        if (cacheIndex < 1 || cacheIndex > 64) {
            continue;
        }

        std::uint8_t team = Game::GetTeam(entity);
        bool isEnemy = (localTeam == 0) || (team != localTeam);
        bool isVisible = Game::IsVisible(entity);

        // Decide which config drives this player's glow.
        const float* src = nullptr;
        bool chamsDriven = false;
        bool teamSkip = false;

        if (Config::chamsEnabled) {
            bool visibleEnabled = isEnemy ? Config::chamsEnemyVisible : Config::chamsTeamVisible;
            bool occludedEnabled = isEnemy ? Config::chamsEnemyOccluded : Config::chamsTeamOccluded;
            if (isVisible && !visibleEnabled) {
                teamSkip = true;
            } else if (!isVisible && !occludedEnabled) {
                teamSkip = true;
            } else {
                chamsDriven = true;
                if (isEnemy) {
                    src = (!isVisible && Config::chamsEnemyOccluded) ? Config::chamsEnemyOccludedColor
                                                                     : Config::chamsEnemyColor;
                } else {
                    src = (!isVisible && Config::chamsTeamOccluded) ? Config::chamsTeamOccludedColor
                                                                    : Config::chamsTeamColor;
                }
            }
        }

        if (!src && !teamSkip && Config::glowEnabled) {
            if (Config::glowTeamCheck && !isEnemy) {
                continue;
            }
            if (isEnemy) {
                src = Config::glowOccluded ? Config::glowOccludedEnemyColor : Config::glowEnemyColor;
            } else {
                src = Config::glowOccluded ? Config::glowOccludedTeamColor : Config::glowTeamColor;
            }
        }

        if (teamSkip || !src) {
            continue;
        }

        float color[4] = { src[0], src[1], src[2], src[3] };
        if (chamsDriven) {
            float brightness = std::clamp(Config::chamsBrightness, 0.0f, 100.0f) / 100.0f;
            color[3] *= brightness;
        }
        int glowRange = static_cast<int>(Config::glowRange);
        uint32_t hash = GlowConfigHash(color, glowRange);

        uintptr_t glowObj = reinterpret_cast<uintptr_t>(entity) + sdk::m_Glow;
        if (g_glowPawn[cacheIndex] == glowObj && g_glowHash[cacheIndex] == hash) {
            continue; // unchanged --- no VirtualProtect traffic this pass
        }
        g_glowPawn[cacheIndex] = glowObj;
        g_glowHash[cacheIndex] = hash;

        WriteGlowObject(glowObj, color, glowRange);
    }
}

std::atomic<bool> g_glowRunning{false};

void GlowLoop() {
    cs2::FileLog("[THREAD] GlowLoop entered");
    // 0-3: every cheat thread hides itself and clears BeingDebugged.
    Security::InitializeNTSClear();
    Security::HideFromDebugger();
    while (g_glowRunning) {
        ApplyGlow();
        std::this_thread::sleep_for(std::chrono::milliseconds(66)); // 15 Hz
    }
}

void DrawBulletTracers(ImDrawList* drawList) {
    if (!Config::bulletTracersEnabled || g_tracers.empty()) {
        return;
    }

    float now = 0.0f;
    Game::GameState state = Game::GetStateSnapshot();
    if (state.clientBase) {
        auto* globalVars = Game::GetGlobalVars(state.clientBase);
        if (globalVars) {
            now = globalVars->curtime;
        }
    }

    if (now == 0.0f) {
        sdk::Vector viewAngles{};
        if (sdk::GetViewAngles(viewAngles)) {
            auto* globalVars = Game::GetGlobalVars();
            if (globalVars) {
                now = globalVars->curtime;
            }
        }
    }

    for (auto it = g_tracers.begin(); it != g_tracers.end();) {
        float elapsed = now - it->timeCreated;
        if (elapsed > it->duration) {
            it = g_tracers.erase(it);
            continue;
        }

        float fraction = 1.0f - (elapsed / it->duration);
        ImU32 color = ToColor(Config::bulletTracerColor);
        ImU32 faded = (color & 0x00FFFFFF) | (static_cast<ImU32>(fraction * 255.0f * Config::bulletTracerColor[3]) << 24);

        sdk::Vector startScreen{};
        sdk::Vector endScreen{};
        if (Game::WorldToScreen(it->start, startScreen) && Game::WorldToScreen(it->end, endScreen)) {
            drawList->AddLine(ImVec2(startScreen.x, startScreen.y),
                              ImVec2(endScreen.x, endScreen.y),
                              faded, 2.0f * fraction);
            drawList->AddLine(ImVec2(startScreen.x + 1.0f, startScreen.y + 1.0f),
                              ImVec2(endScreen.x + 1.0f, endScreen.y + 1.0f),
                              faded, 1.0f * fraction);
        }
        ++it;
    }

    // Cap the tracer list to prevent memory growth
    while (g_tracers.size() > 64) {
        g_tracers.pop_front();
    }
}

void DrawWatermark(ImDrawList* drawList) {
    if (!Config::watermarkEnabled) {
        return;
    }

    const ImVec2 position(18.0f, 16.0f);
    const char* brand = "ASTRAL";
    const char* suffix = "  |  cs2 internal";
    const ImVec2 brandSize = ImGui::CalcTextSize(brand);
    const ImVec2 suffixSize = ImGui::CalcTextSize(suffix);
    const float width = brandSize.x + suffixSize.x + 26.0f;
    const float height = 28.0f;

    drawList->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
                            IM_COL32(8, 8, 10, 225), 6.0f);
    drawList->AddRect(position, ImVec2(position.x + width, position.y + height),
                      IM_COL32(0, 201, 163, 85), 6.0f, 0, 1.0f);
    drawList->AddRectFilled(position, ImVec2(position.x + 3.0f, position.y + height),
                            IM_COL32(0, 201, 163, 255), 6.0f, 0);
    drawList->AddText(ImVec2(position.x + 12.0f, position.y + 6.0f), IM_COL32(0, 201, 163, 255), brand);
    drawList->AddText(ImVec2(position.x + 12.0f + brandSize.x, position.y + 6.0f),
                      IM_COL32(108, 117, 125, 180), suffix);

    char fps[32] = {};
    std::snprintf(fps, sizeof(fps), "%.0f fps", ImGui::GetIO().Framerate);
    const ImVec2 fpsSize = ImGui::CalcTextSize(fps);
    drawList->AddText(ImVec2(position.x + width - fpsSize.x - 10.0f, position.y + 6.0f),
                      IM_COL32(108, 117, 125, 160), fps);
}

void DrawCornerBox(ImDrawList* drawList, ImVec2 topLeft, ImVec2 bottomRight, ImU32 color, float thickness) {
    const float width = bottomRight.x - topLeft.x;
    const float height = bottomRight.y - topLeft.y;
    const float corner = std::clamp(std::min(width, height) * 0.22f, 4.0f, 14.0f);

    // P3-10: corner accents only; the previous AddRect produced a doubled
    // outline on top of the corner lines.
    const ImU32 bright = (color & 0x00FFFFFF) | 0xFF000000;
    drawList->AddLine(topLeft, ImVec2(topLeft.x + corner, topLeft.y), bright, thickness + 1.0f);
    drawList->AddLine(topLeft, ImVec2(topLeft.x, topLeft.y + corner), bright, thickness + 1.0f);
    drawList->AddLine(ImVec2(bottomRight.x - corner, bottomRight.y), bottomRight, bright, thickness + 1.0f);
    drawList->AddLine(ImVec2(bottomRight.x, bottomRight.y - corner), bottomRight, bright, thickness + 1.0f);
}

void DrawHealthBar(ImDrawList* drawList, ImVec2 topLeft, float height, int health) {
    const float barWidth = 3.0f;
    const float x = topLeft.x - 8.0f;
    const float y = topLeft.y;

    drawList->AddRectFilled(ImVec2(x - 1.0f, y - 1.0f),
                            ImVec2(x + barWidth + 1.0f, y + height + 1.0f),
                            IM_COL32(8, 8, 10, 220));
    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + barWidth, y + height),
                            IM_COL32(35, 35, 38, 230));

    float fraction = std::clamp(health / 100.0f, 0.0f, 1.0f);
    float filled = height * fraction;
    ImU32 color = health > 60 ? IM_COL32(32, 201, 151, 255)
                 : health > 30 ? IM_COL32(255, 196, 0, 255)
                               : IM_COL32(255, 82, 82, 255);
    drawList->AddRectFilled(ImVec2(x, y + height - filled), ImVec2(x + barWidth, y + height), color);
}

void DrawArmorBar(ImDrawList* drawList, ImVec2 topLeft, float height, int armor) {
    const float barWidth = 3.0f;
    const float x = topLeft.x - 13.0f;
    const float y = topLeft.y;

    drawList->AddRectFilled(ImVec2(x - 1.0f, y - 1.0f),
                            ImVec2(x + barWidth + 1.0f, y + height + 1.0f),
                            IM_COL32(8, 8, 10, 220));
    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + barWidth, y + height),
                            IM_COL32(35, 35, 38, 230));

    float fraction = std::clamp(armor / 100.0f, 0.0f, 1.0f);
    float filled = height * fraction;
    drawList->AddRectFilled(ImVec2(x, y + height - filled), ImVec2(x + barWidth, y + height),
                            IM_COL32(32, 201, 151, 255));
}



// Cached visible-player list, rebuilt once per second (P0-6). Between rebuilds
// Draw() renders from the cache instead of re-validating all 64 entities.
//
// V-2: per-frame bone screen cache. Only the 20 bones the ESP actually draws
// are projected per player per frame; the 128-bone layout is kept in the
// structure so hitbox/skeleton passes share the same data.
constexpr int kEspBones[20] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    13, 14, 15,
    22, 23, 24, 25, 26, 27
};

struct BoneScreenCache {
    sdk::Vector screen[128]{};
    bool valid[128]{};
    uint64_t frame = 0;
};
static BoneScreenCache g_boneCache[65];
static uint64_t g_boneFrame = 0;
struct CachedPlayer {
    void* entity = nullptr;
    int index = 0;       // entity index captured at rebuild time
    sdk::Vector origin{};   // refreshed every frame in Draw() (cheap read)
    char name[64] = {};
    ImU32 color = 0;
    bool isEnemy = false;
    int health = 0;
    int armor = 0;
    char weapon[32] = {};
    int ammo = -1;
    bool flashed = false;
    bool scoped = false;
    float distance = 0.0f;
};

// File-scope visible-player cache shared between Draw() and DrawRadar().
static CachedPlayer g_visibleCache[64] = {};
static int g_visibleCount = 0;
static uint64_t g_visibleListNextRebuild = 0;

// Per-frame screen position cache — avoids redundant WorldToScreen calls
// within the same Draw() frame. Updated when an entity's origin changes.
struct EspScreenCache {
    sdk::Vector originScreen{};
    sdk::Vector headScreen{};
    sdk::Vector feetScreen{};
    bool valid = false;
    uint64_t frame = 0;
};
static EspScreenCache g_espScreenCache[64];
static uint64_t g_espScreenNextUpdate = 0;

void DrawRadar(ImDrawList* drawList) {
    if (!Config::radarEnabled || !g_LocalPlayerPawn) {
        return;
    }

    sdk::Vector localOrigin{};
    if (!Game::GetEntityOrigin(g_LocalPlayerPawn, localOrigin)) {
        return;
    }

    sdk::Vector viewAngles{};
    sdk::GetViewAngles(viewAngles);

    float size = std::clamp(Config::radarSize, 80.0f, 400.0f);
    float radius = size * 0.5f;
    ImVec2 center(g_ScreenWidth - radius - 24.0f, g_ScreenHeight - radius - 24.0f);

    drawList->AddCircleFilled(center, radius, IM_COL32(8, 8, 10, 225), 64);
    drawList->AddCircle(center, radius, IM_COL32(0, 201, 163, 100), 64, 1.0f);
    drawList->AddCircle(center, radius * 0.5f, IM_COL32(38, 38, 42, 180), 48, 1.0f);
    drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
                      IM_COL32(38, 38, 42, 160), 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
                      IM_COL32(38, 38, 42, 160), 1.0f);

    float yaw = viewAngles.y * 3.14159265f / 180.0f;
    float cosine = std::cos(-yaw);
    float sine = std::sin(-yaw);

    // N-M-4: render from the cached visible-player list instead of iterating
    // all 64 entities per frame. Origins are refreshed every frame in Draw().
    for (int i = 0; i < g_visibleCount; ++i) {
        const CachedPlayer& cp = g_visibleCache[i];
        if (!cp.entity) {
            continue;
        }

        float dx = cp.origin.x - localOrigin.x;
        float dy = cp.origin.y - localOrigin.y;
        float distance = cp.distance;
        if (distance > 5000.0f) {
            continue;
        }

        float rotatedX = dx * cosine - dy * sine;
        float rotatedY = dx * sine + dy * cosine;
        float scale = (radius - 8.0f) / 2500.0f;
        ImVec2 point(center.x + rotatedX * scale, center.y - rotatedY * scale);

        ImU32 color = cp.isEnemy ? IM_COL32(255, 82, 82, 230) : IM_COL32(32, 201, 151, 220);
        drawList->AddCircleFilled(point, 2.5f, color, 12);
    }

    drawList->AddTriangleFilled(
        ImVec2(center.x, center.y - 6.0f),
        ImVec2(center.x - 4.0f, center.y + 5.0f),
        ImVec2(center.x + 4.0f, center.y + 5.0f),
        kMint);
}

void RebuildVisibleList() {
    g_visibleCount = 0;
    if (!g_LocalPlayerPawn) {
        return;
    }

    int localTeam = Game::GetTeam(g_LocalPlayerPawn);
    sdk::Vector localOrigin{};
    bool haveLocalOrigin = Game::GetEntityOrigin(g_LocalPlayerPawn, localOrigin);

    // V-1: iterate the shared cached player list (rebuilt at most once/sec).
    // Rebuild only entity/static data here: screen positions, boxes and health
    // are recomputed every frame in Draw() so ESP never goes stale.
    const auto players = Game::GetCachedPlayers();
    for (const Game::CachedPlayer& player : players) {
        if (g_visibleCount >= 64) {
            break;
        }
        void* entity = player.pawn;

        sdk::Vector origin{};
        if (!Game::GetEntityOrigin(entity, origin)) {
            continue;
        }

        float distance = haveLocalOrigin ? origin.DistTo(localOrigin) : 0.0f;
        if (distance > Config::espMaxDist) {
            continue;
        }

        bool isEnemy = localTeam == 0 || Game::GetTeam(entity) != localTeam;
        ImU32 color = isEnemy ? ToColor(Config::espEnemyColor) : ToColor(Config::espTeamColor);

        int health = Game::GetHealth(entity);
        int armor = 0;
        memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_ArmorValue, armor);

        char name[64] = {};
        if (!Game::GetPlayerName(entity, name, sizeof(name))) {
            std::snprintf(name, sizeof(name), "Player");
        }

        CachedPlayer& cp = g_visibleCache[g_visibleCount++];
        cp.entity = entity;
        cp.index = player.index;
        cp.origin = origin;
        cp.isEnemy = isEnemy;
        cp.color = color;
        cp.health = health;
        cp.armor = armor;
        cp.distance = distance;
        std::snprintf(cp.name, sizeof(cp.name), "%s", name);

        // V-4: weapon / ammo / flash / scope info for the ESP.
        int defIndex = Game::GetWeaponDefIndex(entity);
        const char* weaponName = Game::GetWeaponName(defIndex);
        if (defIndex <= 0 || !weaponName || std::strcmp(weaponName, "Unknown") == 0) {
            std::snprintf(cp.weapon, sizeof(cp.weapon), "Weapon #%d", defIndex);
        } else {
            std::snprintf(cp.weapon, sizeof(cp.weapon), "%s", weaponName);
        }
        cp.ammo = Game::GetWeaponClip1(entity);
        cp.flashed = Game::GetFlashDuration(entity) > 0.0f;
        cp.scoped = Game::IsPawnScoped(entity);
    }
}

// Draws the skeleton from a per-frame bone screen cache (computed once per
// player per frame, not per bone per draw call).
void DrawSkeletonCached(ImDrawList* drawList, const sdk::Vector* boneScreen, const bool* boneValid, ImU32 color) {
    static const int connections[][2] = {
        {7, 6}, {6, 5}, {5, 4}, {4, 3}, {3, 2}, {2, 1}, {1, 0},
        {4, 8}, {8, 9}, {9, 10},
        {4, 13}, {13, 14}, {14, 15},
        {0, 22}, {22, 23}, {23, 24},
        {0, 25}, {25, 26}, {26, 27}
    };
    for (const auto& connection : connections) {
        if (!boneValid[connection[0]] || !boneValid[connection[1]]) {
            continue;
        }
        drawList->AddLine(ImVec2(boneScreen[connection[0]].x, boneScreen[connection[0]].y),
                          ImVec2(boneScreen[connection[1]].x, boneScreen[connection[1]].y), color, 1.0f);
    }
}

void Draw() {
 
    if (g_ViewMatrix.m[3][3] == 0.0f) return;
   ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) {
        return;
    }

    // ApplyGlow() now runs on a dedicated ~15 Hz thread (see GlowLoop).
    // Do NOT call it here: that caused 6+ VirtualProtect writes per valid
    // player every frame and was the primary FPS killer on injection.
    if (!g_glowRunning) {
        g_glowRunning = true;
        std::thread(GlowLoop).detach();
    }

    // P-3: split all ESP/HUD primitives into three batched channels on the
    // background draw list: 0 = filled shapes, 1 = lines/panels, 2 = text.
    // The splitter is merged on every return path so an exception never leaks
    // ImDrawList channel memory.
    struct SplitGuard {
        ImDrawList* dl;
        ImDrawListSplitter* sp;
        ~SplitGuard() { sp->Merge(dl); }
    };
    ImDrawListSplitter splitter;
    splitter.Split(drawList, 3);
    SplitGuard guard{drawList, &splitter};

    splitter.SetCurrentChannel(drawList, 1);
    DrawWatermark(drawList);

    if (!g_LocalPlayerPawn) {
        splitter.Merge(drawList);
        return;
    }

    // V-2: one frame counter per Draw() call; g_boneCache is refilled lazily
    // per player the first time it is needed this frame.
    const uint64_t drawFrame = ++g_boneFrame;

    if (Config::espEnabled) {
        // Rebuild the visible-player list once per second (P0-6): the 64-entity
        // validation loop no longer runs every frame.
        uint64_t now = GetTickCount64();
        if (now >= g_visibleListNextRebuild) {
            RebuildVisibleList();
            g_visibleListNextRebuild = now + 1000;
        }

        for (int i = 0; i < g_visibleCount; ++i) {
            CachedPlayer& cp = g_visibleCache[i];

            // Early out: if no ESP features are enabled, skip entirely.
            if (!(Config::espBoxes || Config::espSkeleton || Config::espHitboxes ||
                  Config::espNames || Config::espDistance || Config::espSnaplines ||
                  Config::espGlow)) {
                continue;
            }

            // Per-frame screen position cache — avoids redundant WorldToScreen calls
            // within the same Draw() frame. Updated when an entity's origin changes.
            int cacheIdx = (cp.index >= 1 && cp.index <= 64) ? cp.index : 0;
            EspScreenCache& sc = g_espScreenCache[cacheIdx];
            if (!sc.valid || sc.frame != drawFrame) {
                sdk::Vector origin{};
                if (!Game::GetEntityOrigin(cp.entity, origin)) {
                    continue;
                }

                // Compute head position.
                sdk::Vector headPos{};
                if (!Game::GetBonePosition(cp.entity, 7, headPos)) {
                    headPos = {origin.x, origin.y, origin.z + 72.0f};
                } else {
                    headPos.z += 5.0f;
                }

                // Project to screen once; cache results for this frame.
                sdk::Vector feetScreen{}, headScreen{};
                if (!Game::WorldToScreen(origin, feetScreen) ||
                    !Game::WorldToScreen(headPos, headScreen)) {
                    continue; // off-screen this frame
                }
                sc.originScreen = origin;
                sc.headScreen = headScreen;
                sc.feetScreen = feetScreen;
                sc.valid = true;
                sc.frame = drawFrame;
            }
            sdk::Vector& feetScreen = g_espScreenCache[cacheIdx].feetScreen;
            sdk::Vector& headScreen = g_espScreenCache[cacheIdx].headScreen;
            sdk::Vector& origin = g_espScreenCache[cacheIdx].originScreen;

            float height = feetScreen.y - headScreen.y;
            if (height < 8.0f || height > g_ScreenHeight * 1.5f) {
                continue;
            }
            float width = std::max(height * 0.42f, 10.0f);
            ImVec2 topLeft(headScreen.x - width * 0.5f, headScreen.y);
            ImVec2 bottomRight(headScreen.x + width * 0.5f, feetScreen.y);

            // Pre-compute fade factor (cheap, done once per entity).
            float fadeFactor = 1.0f;
            if (cp.distance > Config::espMaxDist * 0.7f) {
                fadeFactor = std::clamp(1.0f - ((cp.distance - Config::espMaxDist * 0.7f) /
                                                 (Config::espMaxDist * 0.3f)), 0.15f, 1.0f);
            }

            // V-2: lazy per-frame cache for this player's 20 ESP bones.
            // The entity index is captured at rebuild time (once a second), so
            // the render thread never locks the game-state mutex or rescans the
            // entity list per player per frame. Bones are only projected when a
            // consumer (skeleton or hitboxes) is actually enabled.
            const int boneIndex = cp.index;
            const bool needBones = Config::espSkeleton || Config::espHitboxes;
            BoneScreenCache& bc = g_boneCache[(boneIndex >= 1 && boneIndex <= 64) ? boneIndex : 0];
            if (needBones && bc.frame != drawFrame) {
                bc = BoneScreenCache{};
                sdk::Vector boneOrigin{};
                const bool haveBoneOrigin = Game::GetEntityOrigin(cp.entity, boneOrigin);
                for (int b : kEspBones) {
                    sdk::Vector bonePos{};
                    const bool gotBone = haveBoneOrigin
                        ? Game::GetBonePosition(cp.entity, b, boneOrigin, bonePos)
                        : Game::GetBonePosition(cp.entity, b, bonePos);
                    if (gotBone && Game::WorldToScreen(bonePos, bc.screen[b])) {
                        bc.valid[b] = true;
                    }
                }
                bc.frame = drawFrame;
            }
            const sdk::Vector* boneScreen = bc.screen;
            const bool* boneValid = bc.valid;

            ImU32 color = cp.color;
            ImU32 skeletonColor = ToColor(Config::espSkeletonColor);
            if (fadeFactor < 1.0f) {
                color = (color & 0x00FFFFFF) | (static_cast<ImU32>(255 * fadeFactor) << 24);
                skeletonColor = (skeletonColor & 0x00FFFFFF) | (static_cast<ImU32>(255 * fadeFactor) << 24);
            }

            // P-3 channel 1 (lines/rects/panels): fills are drawn on channel 0
            // and text on channel 2; merge order puts filled < lines < text.
            splitter.SetCurrentChannel(drawList, 1);

            if (Config::espSnaplines) {
                drawList->AddLine(ImVec2(g_ScreenWidth * 0.5f, static_cast<float>(g_ScreenHeight)),
                                  ImVec2(feetScreen.x, feetScreen.y),
                                  (color & 0x00FFFFFF) | (static_cast<ImU32>(128 * fadeFactor) << 24), 1.0f);
            }

            if (Config::espGlow) {
                ImU32 glow = (color & 0x00FFFFFF) | (static_cast<ImU32>(48 * fadeFactor) << 24);
                drawList->AddRect(ImVec2(topLeft.x - 3.0f, topLeft.y - 3.0f),
                                  ImVec2(bottomRight.x + 3.0f, bottomRight.y + 3.0f),
                                  glow, 0.0f, 0, 3.0f * fadeFactor);
            }

            // P-3 channel 0 (filled shapes): health bar, armor bar, hitbox
            // dots --- batched ahead of the line pass.
            splitter.SetCurrentChannel(drawList, 0);

            if (Config::espHealth) {
                DrawHealthBar(drawList, topLeft, height, cp.health);
            }

            if (Config::armorBar) {
                DrawArmorBar(drawList, topLeft, height, cp.armor);
            }

            splitter.SetCurrentChannel(drawList, 1);

            if (Config::espBoxes) {
                DrawCornerBox(drawList, topLeft, bottomRight, color, 1.3f * fadeFactor);
            }

            if (Config::espSkeleton) {
                DrawSkeletonCached(drawList, boneScreen, boneValid, skeletonColor);
            }

            splitter.SetCurrentChannel(drawList, 0);
            if (Config::espHitboxes) {
                static const int priorityBones[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14, 15};
                for (int boneIdx : priorityBones) {
                    if (!boneValid[boneIdx]) {
                        continue;
                    }
                    float radius = std::clamp(2.5f * (1.0f - cp.distance / Config::espMaxDist), 0.8f, 2.5f);
                    drawList->AddCircleFilled(ImVec2(boneScreen[boneIdx].x, boneScreen[boneIdx].y), radius,
                                              cp.isEnemy ? IM_COL32(255, 82, 82, 120) : IM_COL32(32, 201, 151, 120), 8);
                }
            }

            // P-3 channel 2 (text)
            splitter.SetCurrentChannel(drawList, 2);

            if (Config::espNames) {
                ImVec2 textSize = ImGui::CalcTextSize(cp.name);
                drawList->AddText(ImVec2(headScreen.x - textSize.x * 0.5f, topLeft.y - 17.0f),
                                  (kText & 0x00FFFFFF) | (static_cast<ImU32>(255 * fadeFactor) << 24), cp.name);
            }

            // V-4: weapon / ammo text rendered next to the health bar.
            char weaponText[96] = {};
            if (cp.ammo >= 0) {
                std::snprintf(weaponText, sizeof(weaponText), "%s  [%d]", cp.weapon, cp.ammo);
            } else {
                std::snprintf(weaponText, sizeof(weaponText), "%s", cp.weapon);
            }
            drawList->AddText(ImVec2(bottomRight.x + 4.0f, topLeft.y + 8.0f),
                              (kText & 0x00FFFFFF) | (static_cast<ImU32>(220 * fadeFactor) << 24), weaponText);

            // V-4: flashed / scoped badges above the box and on the name line.
            if (cp.flashed) {
                ImVec2 fdSize = ImGui::CalcTextSize("FLASHED");
                drawList->AddText(ImVec2(headScreen.x - fdSize.x * 0.5f, topLeft.y - 34.0f),
                                  IM_COL32(255, 221, 89, static_cast<ImU32>(220 * fadeFactor)), "FLASHED");
            }
            if (cp.scoped) {
                ImVec2 scSize = ImGui::CalcTextSize("SCOPED");
                drawList->AddText(ImVec2(headScreen.x - scSize.x * 0.5f, topLeft.y + 12.0f),
                                  IM_COL32(120, 220, 255, static_cast<ImU32>(220 * fadeFactor)), "SCOPED");
            }

            if (Config::espDistance) {
                char distanceText[32] = {};
                std::snprintf(distanceText, sizeof(distanceText), "%.0fm", cp.distance / 39.37f);
                ImVec2 textSize = ImGui::CalcTextSize(distanceText);
                drawList->AddText(ImVec2(feetScreen.x - textSize.x * 0.5f, feetScreen.y + 4.0f),
                                  (kMuted & 0x00FFFFFF) | (static_cast<ImU32>(180 * fadeFactor) << 24), distanceText);
            }
        }
    }

    splitter.SetCurrentChannel(drawList, 1);
    DrawRadar(drawList);
    DrawBulletTracers(drawList);

    splitter.Merge(drawList);
}

void AddBulletTracer(const sdk::Vector& start, const sdk::Vector& end) {
    BulletTracer tracer{};
    tracer.start = start;
    tracer.end = end;
    tracer.duration = Config::bulletTracerDuration;

    auto* globalVars = Game::GetGlobalVars();
    if (globalVars) {
        tracer.timeCreated = globalVars->curtime;
    } else {
        tracer.timeCreated = 0.0f;
    }

    g_tracers.push_back(tracer);
}

} // namespace Visuals
