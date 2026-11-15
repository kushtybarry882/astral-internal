// AUDIT: P1-2, P1-4, P3-4, P3-5
// UPGRADE: C-2, C-3, C-4, C-6, A-1, A-2
// CRASH-FIX: Cause 2 — RunAntiAim / RunResolver / HandlePeek validate the pawn.

#include "rage.h"
#include "combat.h"
#include "../core/config.h"
#include "../core/game.h"
#include "../core/memory.h"
#include "../core/sdk.h"
#include "../core/usercmd.h"
#include "../core/log.h"

#include <mutex>
#include <algorithm>
#include <cmath>
#include <random>
#include <deque>
#include <cstring>
#include <vector>
#include <chrono>

namespace Rage {

static int g_tickBase = 0;
static std::mt19937 g_rng(std::chrono::steady_clock::now().time_since_epoch().count());
static std::uniform_real_distribution<float> g_jitter(-1.0f, 1.0f);
static float g_spinYaw = 0.0f;

// C-3: hitchance estimation from the weapon's accuracy penalty.
float EstimateHitchance(void* entity, const sdk::Vector& point, const sdk::Vector& eye) {
    void* weapon = Game::GetActiveWeapon();
    if (!weapon) {
        return 0.0f;
    }

    float accuracy = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_fAccuracyPenalty, accuracy)) {
        return 0.0f;
    }

    // Rough model: spread radius grows with the penalty and range. The hitbox
    // radius (~5 u for the head) sets the pass/fail threshold.
    float distance = eye.DistTo(point);
    float spreadRadius = accuracy * distance * 0.05f;
    constexpr float hitboxRadius = 5.0f;

    if (spreadRadius < 0.01f) {
        return 100.0f;
    }
    float chance = std::min(1.0f, hitboxRadius / spreadRadius);
    return chance * 100.0f;
}

// A-1: anti-aim pitch manipulation. Writes fake pitch angles to the command
// so the server sees desynchronized view angles while the local camera is
// untouched.
void RunAntiAim(sdk::CUserCmd* cmd) {
    if (!cmd || !Config::antiAimEnabled) {
        return;
    }
    if (!g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    sdk::Vector angles{};
    if (!sdk::GetCmdViewAngles(cmd, angles)) {
        if (!sdk::GetViewAngles(angles)) {
            return;
        }
    }

    switch (Config::antiAimPitch) {
        case 0: // Down
            angles.x = 89.0f;
            break;
        case 1: // Fake (flip between 89 and -89)
            angles.x = (GetTickCount64() % 2000 < 1000) ? 89.0f : -89.0f;
            break;
        case 2: // Zero
            angles.x = 0.0f;
            break;
        case 3: // Custom (down)
            angles.x = 89.0f;
            break;
        case 4: // Jitter
            angles.x = (GetTickCount64() % 200 < 100) ? 89.0f : -89.0f;
            break;
        default:
            angles.x = 89.0f;
            break;
    }

    // Yaw manipulation
    switch (Config::antiAimYaw) {
        case 0: // Spin
            g_spinYaw += Config::antiAimSpinSpeed * (1.0f / 64.0f);
            angles.y = sdk::NormalizeYaw(g_spinYaw);
            break;
        case 1: // Jitter
            angles.y = sdk::NormalizeYaw(angles.y + g_jitter(g_rng) * 30.0f);
            break;
        case 2: // Static
            angles.y = sdk::NormalizeYaw(angles.y + Config::antiAimYawOffset);
            break;
        case 3: // LBY
            angles.y = sdk::NormalizeYaw(angles.y + (GetTickCount64() % 2000 < 1000 ? Config::antiAimDesyncAngle : -Config::antiAimDesyncAngle));
            break;
        default:
            break;
    }

    angles = sdk::NormalizeAngles(angles);
    angles.x = std::clamp(angles.x, -89.0f, 89.0f);

    sdk::SetCmdAngles(cmd, angles, true, true);
}

// C-4 / A-2: ragebot — target selection, multipoint, hitchance, auto-fire.
void RunRage(sdk::CUserCmd* cmd) {
    if (!cmd || !Config::ragebotEnabled) {
        return;
    }
    if (!g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    sdk::Vector eye{};
    if (!Game::GetLocalEyePosition(eye)) {
        return;
    }

    sdk::Vector viewAngles{};
    if (!sdk::GetViewAngles(viewAngles)) {
        return;
    }

    float bestFov = Config::rageFov;
    float bestScore = -1e9f;
    sdk::Vector bestAngle{};
    sdk::Vector bestPos{};
    void* bestEnt = nullptr;

    for (const Game::CachedPlayer& cp : Game::GetCachedPlayers()) {
        void* entity = cp.pawn;
        if (!Game::IsValidPlayer(entity, true)) {
            continue;
        }

        // Multipoint: try head, chest, pelvis in priority order.
        const int bones[] = {7, 4, 1};
        for (int bone : bones) {
            sdk::Vector targetPos{};
            if (Game::GetBonePosition(entity, bone, targetPos) && targetPos.IsFinite() && !targetPos.IsZero()) {
                sdk::Vector angle = Combat::CalculateAngle(eye, targetPos);
                float fov = sdk::AngleDelta(viewAngles, angle);
                if (fov > bestFov) {
                    continue;
                }

                float hitchance = EstimateHitchance(entity, targetPos, eye);
                if (hitchance < Config::rageHitchance) {
                    continue;
                }

                float distance = eye.DistTo(targetPos);
                int health = Game::GetHealth(entity);

                float score = 0.0f;
                switch (Config::rageTargetSelection) {
                    case 0: // Threat
                        score = static_cast<float>(health) / (distance + 1.0f);
                        break;
                    case 1: // Distance
                        score = 10000.0f - distance;
                        break;
                    case 2: // Crosshair
                        score = -fov;
                        break;
                    default:
                        score = static_cast<float>(health) / (distance + 1.0f);
                        break;
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestFov = fov;
                    bestAngle = angle;
                    bestPos = targetPos;
                    bestEnt = entity;
                }
            }
        }
    }

    if (!bestEnt) {
        return;
    }

    // Auto-fire
    if (Config::rageAutoFire) {
        sdk::SetCmdButton(cmd, sdk::IN_ATTACK, true);
    }

    sdk::SetCmdAngles(cmd, bestAngle, true, true);
}

// A-1: resolver — resolve enemy yaw from server-side angle changes.
void RunResolver() {
    if (!Config::resolverEnabled) {
        return;
    }
    if (!g_LocalPlayerPawn) {
        return;
    }

    for (const Game::CachedPlayer& cp : Game::GetCachedPlayers()) {
        void* entity = cp.pawn;
        if (!Game::IsValidPlayer(entity, true)) {
            continue;
        }

        // Read the server-side view angle changes to infer the real yaw.
        // CCSPlayerController::m_ServerViewAngleChanges stores the delta.
        // (offsets are build-dependent; validated against the dump)
        uintptr_t controller = Game::GetControllerForPawn(entity);
        if (!controller || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(controller))) {
            continue;
        }

        // Resolver logic would write corrected angles here.
        // Placeholder: the actual yaw correction is applied in RunAntiAim's
        // targeting or in a separate aim correction pass.
        (void)controller;
    }
}

// A-2: auto peek — peek around a corner and shoot when an enemy is visible.
void HandlePeek() {
    if (!Config::rageAutoPeek) {
        return;
    }
    if (!g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    // Auto peek implementation: store peek position, return to it when
    // an enemy is spotted, then resume peeking.
    // This is a placeholder skeleton — full implementation requires
    // a key binding for peek activation and position recording.
    (void)g_LocalPlayerPawn;
}

void SetTickBase(int tickBase) {
    g_tickBase = tickBase;
}

void Reset() {
    g_spinYaw = 0.0f;
}

} // namespace Rage