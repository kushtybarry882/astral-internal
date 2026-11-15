// AUDIT: P1-2, P1-4, P3-4, P3-5
// UPGRADE: C-1, C-5, V-1
// CRASH-FIX: Cause 2 — RunTriggerbot validates the pawn pointer.
#include "combat.h"
#include "../core/config.h"
#include "../core/game.h"
#include "../core/memory.h"
#include "../core/usercmd.h"
#include "../core/sdk.h"
#include "../features/rage.h"
#include "../ui/menu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <chrono>

namespace Combat {

namespace {

constexpr float kRadToDeg = 57.29577951308232f;

// include/verified_features.json (live build 14184): stock agent skeleton is
// 1 pelvis, 2-5 spine_0-3, 6 neck_0, 7 head_0. The old values (head=6) aimed
// at the neck; these match the dump.
constexpr int kBoneHead  = 7;
constexpr int kBoneNeck  = 6;
constexpr int kBoneChest = 4;
constexpr int kBoneStomach = 3;
constexpr int kBonePelvis = 1;

static std::mt19937 g_rng(std::chrono::steady_clock::now().time_since_epoch().count());
static std::uniform_real_distribution<float> g_jitterDist(-1.0f, 1.0f);
static std::uniform_real_distribution<float> g_stepDist(0.82f, 1.18f);
static bool g_silentFired = false;
static float g_triggerSampledDelay = 0.0f; // sampled once per triggerbot acquisition

sdk::Vector CalculateAngle(const sdk::Vector& from, const sdk::Vector& to) {
    sdk::Vector delta = to - from;
    float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);

    sdk::Vector angle{};
    angle.x = -std::atan2(delta.z, horizontal) * kRadToDeg;
    angle.y = std::atan2(delta.y, delta.x) * kRadToDeg;
    angle.z = 0.0f;

    angle.x = std::clamp(angle.x, -89.0f, 89.0f);
    angle.y = sdk::NormalizeYaw(angle.y);
    return angle;
}

bool IsAimKeyDown() {
    if (!Config::aimbotEnabled) {
        return false;
    }

    int key = Config::aimKey;
    if (key == 0) {
        key = VK_LBUTTON;
    }

    bool down = (GetAsyncKeyState(key) & 0x8000) != 0;

    if (Config::aimKeyMode == 0) {
        return down;
    }

    static bool toggled = false;
    static bool wasDown = false;
    if (down && !wasDown) {
        toggled = !toggled;
    }
    wasDown = down;
    return toggled;
}

// P3-11: renamed updateCamera -> updateViewAngles to match what the flag does.
void ApplyCommandAngles(sdk::CUserCmd* cmd, const sdk::Vector& angles, bool updateViewAngles, bool updateSubtick) {
    if (!cmd) {
        return;
    }

    sdk::Vector normalized = sdk::NormalizeAngles(angles);
    normalized.x = std::clamp(normalized.x, -89.0f, 89.0f);

    sdk::SetCmdAngles(cmd, normalized, updateSubtick);
    if (updateViewAngles) {
        sdk::SetViewAngles(normalized);
    }
}

struct TargetResult {
    sdk::Vector angle{};
    sdk::Vector position{};
    void* entity = nullptr;
    float fov = 0.0f;
    float distance = 0.0f;
    int health = 0;
    int bone = kBoneHead;
};

int GetTargetBone() {
    switch (Config::aimBone) {
        case 1: return kBoneNeck;
        case 2: return kBoneChest;
        case 3: return kBonePelvis;
        default: return kBoneHead;
    }
}

// Try multiple hitboxes in priority order for maximum hit probability
int GetPriorityBone(int priority) {
    switch (priority) {
        case 0: return kBoneHead;
        case 1: return kBoneNeck;
        case 2: return kBoneChest;
        case 3: return kBoneStomach;
        case 4: return kBonePelvis;
        default: return kBoneHead;
    }
}

sdk::Vector GetHitboxPosition(void* entity, int bone) {
    sdk::Vector pos{};
    if (Game::GetBonePosition(entity, bone, pos) && pos.IsFinite() && !pos.IsZero()) {
        return pos;
    }

    // Fallback to origin + offset
    sdk::Vector origin{};
    if (Game::GetEntityOrigin(entity, origin) && origin.IsFinite()) {
        if (bone == kBoneHead || bone == kBoneNeck) {
            return {origin.x, origin.y, origin.z + 64.0f};
        }
        return origin;
    }
    return {};
}

TargetResult FindBestTarget(const sdk::Vector& eyePos, const sdk::Vector& viewAngles) {
    TargetResult result{};
    result.fov = std::clamp(Config::aimFov, 0.1f, 30.0f);
    bool found = false;

    float bestScore = -9999.0f;
    float targetFov = std::clamp(Config::aimFov, 0.1f, 30.0f);

    // V-1: iterate the shared 1 Hz cache instead of walking the entity chunks.
    for (const Game::CachedPlayer& cp : Game::GetCachedPlayers()) {
        void* entity = cp.pawn;
        if (!Game::IsValidPlayer(entity, Config::aimTeamCheck)) {
            continue;
        }

        if (Config::aimVisibleCheck && !Config::aimAutoWall && !Game::IsVisible(entity)) {
            continue;
        }

        // Try primary bone first
        sdk::Vector targetPos = GetHitboxPosition(entity, GetTargetBone());
        if (targetPos.IsZero()) {
            // Try head as fallback
            targetPos = GetHitboxPosition(entity, kBoneHead);
            if (targetPos.IsZero()) {
                continue;
            }
        }

        sdk::Vector angle = CalculateAngle(eyePos, targetPos);
        float fov = sdk::AngleDelta(viewAngles, angle);
        if (fov > targetFov) {
            continue;
        }

        // C-2: hitchance estimation from weapon accuracy penalty.
        float hitchance = Rage::EstimateHitchance(entity, targetPos, eyePos);
        if (hitchance < Config::aimHitchance) {
            continue;
        }

        float distance = eyePos.DistTo(targetPos);
        int health = Game::GetHealth(entity);

        // Calculate score based on selection mode
        float score = 0.0f;
        switch (Config::aimTargetSelection) {
            case 0: // FOV
                score = targetFov - fov;
                break;
            case 1: // Distance
                score = 10000.0f - distance;
                break;
            case 2: // Health
                score = static_cast<float>(health);
                break;
            case 3: // Crosshair
                score = -fov;
                break;
            case 4: // Threat
                score = static_cast<float>(health) / distance;
                break;
            default:
                score = targetFov - fov;
                break;
        }

        if (score > bestScore) {
            bestScore = score;
            result.angle = angle;
            result.position = targetPos;
            result.entity = entity;
            result.fov = fov;
            result.distance = distance;
            result.health = health;
            result.bone = GetTargetBone();
            found = true;
        }
    }

    if (!found) {
        result.fov = -1.0f;
    }
    return result;
}

TargetResult FindMultiPointTarget(const sdk::Vector& eyePos, const sdk::Vector& viewAngles) {
    TargetResult result{};
    result.fov = std::clamp(Config::aimFov, 0.1f, 30.0f);
    bool found = false;

    float bestFov = result.fov;
    float bestScore = -1e9f;
    float targetFov = std::clamp(Config::aimFov, 0.1f, 30.0f);

    for (const Game::CachedPlayer& cp : Game::GetCachedPlayers()) {
        void* entity = cp.pawn;
        if (!Game::IsValidPlayer(entity, Config::aimTeamCheck)) {
            continue;
        }

        if (Config::aimVisibleCheck && !Config::aimAutoWall && !Game::IsVisible(entity)) {
            continue;
        }

        // Try each hitbox in priority order
        for (int bonePriority = 0; bonePriority < 5; ++bonePriority) {
            int bone = GetPriorityBone(bonePriority);
            sdk::Vector targetPos = GetHitboxPosition(entity, bone);
            if (targetPos.IsZero()) {
                continue;
            }

            sdk::Vector angle = CalculateAngle(eyePos, targetPos);
            float fov = sdk::AngleDelta(viewAngles, angle);
            if (fov > bestFov || fov > targetFov) {
                continue;
            }

            float distance = eyePos.DistTo(targetPos);
            int health = Game::GetHealth(entity);

            float distanceFactor = std::max(0.1f, distance / 100.0f);
            float score = targetFov - fov + (1.0f / distanceFactor);

            if (score > bestScore || !found) {
                bestScore = score;
                bestFov = fov;
                result.angle = angle;
                result.position = targetPos;
                result.entity = entity;
                result.fov = fov;
                result.distance = distance;
                result.health = health;
                result.bone = bone;
                found = true;
                break; // Found valid hitbox for this entity
            }
        }
    }

    if (!found) {
        result.fov = -1.0f;
    }
    return result;
}

} // namespace

sdk::Vector Combat::CalculateAngle(const sdk::Vector& from, const sdk::Vector& to) {
    return ::CalculateAngle(from, to);
}

void Run(sdk::CUserCmd* cmd) {
    if (!cmd || !g_LocalPlayerPawn || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        return;
    }

    if (!Config::aimbotEnabled) {
        return;
    }
    if (!IsAimKeyDown()) {
        return;
    }
    // The triggerbot already stands down while the menu is open; do the same
    // here so LMB clicks on the menu cannot silently rewrite command angles.
    if (menu::g_ShowMenu) {
        return;
    }

    // C-5: no-spread � clear accuracy penalty every tick so firearm
    // crosshair stays on-point regardless of movement or mode.
    if (Config::noSpreadEnabled) {
        void* weapon = Game::GetActiveWeapon();
        if (weapon) {
            float penalty = 0.0f;
            if (memory::Read(reinterpret_cast<uintptr_t>(weapon) + sdk::m_fAccuracyPenalty, penalty)) {
                memory::Write(reinterpret_cast<uintptr_t>(weapon) + sdk::m_fAccuracyPenalty, 0.0f);
            }
        }
    }

    sdk::Vector eye{};
    sdk::Vector viewAngles{};
    if (!Game::GetLocalEyePosition(eye)) {
        return;
    }

    if (Config::aimSilent) {
        if (!sdk::GetCmdViewAngles(cmd, viewAngles)) {
            if (!sdk::GetViewAngles(viewAngles)) {
                return;
            }
        }
    } else {
        if (!sdk::GetViewAngles(viewAngles)) {
            return;
        }
    }

    Game::GameState state = Game::GetStateSnapshot();
    if (!state.localPlayerController) {
        return;
    }

    float smooth = std::clamp(Config::aimSmooth, 1.0f, 20.0f);

    TargetResult target = FindBestTarget(eye, viewAngles);

    if (target.fov < 0.0f) {
        // Try multi-point
        target = FindMultiPointTarget(eye, viewAngles);
        if (target.fov < 0.0f) {
            return;
        }
    }

    sdk::Vector desired = target.angle;

    if (Config::aimRcs && Game::GetShotsFired() > 0) {
        sdk::Vector punch{};
        if (Game::GetAimPunch(punch) && punch.IsFinite()) {
            desired.x -= punch.x * Config::aimRcsX;
            desired.y -= punch.y * Config::aimRcsY;
            desired = sdk::NormalizeAngles(desired);
            desired.x = std::clamp(desired.x, -89.0f, 89.0f);
        }
    }

    // Apply smoothing (exponential decay for humanized ease-in/ease-out curve)
    if (smooth > 1.0f) {
        float rate = 1.0f / Config::aimSmooth;
        sdk::Vector delta = desired - viewAngles;
        delta.x = sdk::NormalizeYaw(delta.x);
        delta.y = sdk::NormalizeYaw(delta.y);
        delta.z = 0.0f;

        // Exponential decay: angle += delta * (1 - exp(-k * dt))
        desired = viewAngles + delta * (1.0f - std::exp(-rate));
        desired = sdk::NormalizeAngles(desired);
        desired.x = std::clamp(desired.x, -89.0f, 89.0f);
    }

    // Apply jitter for anti-detection
    if (Config::aimSmooth > 1.0f) {
        desired.x += g_jitterDist(g_rng) * 0.1f;
        desired.y += g_jitterDist(g_rng) * 0.1f;
    }

    // C-1: silent aim writes the *command* angles the server processes and never
    // calls SetViewAngles(). If aimSilentAutoFire is set we also assert IN_ATTACK
    // on the same tick — that is what makes silent aim fire.
    if (Config::aimSilent) {
        ApplyCommandAngles(cmd, desired, false, true);
        if (Config::aimSilentAutoFire) {
            sdk::SetCmdButton(cmd, sdk::IN_ATTACK, true);
        }
        g_silentFired = true;
    } else {
        ApplyCommandAngles(cmd, desired, true, true);
    }
}

bool SilentAimFired() {
    bool fired = g_silentFired;
    g_silentFired = false;
    return fired;
}

void RunTriggerbot(sdk::CUserCmd* cmd) {
    if (!cmd) {
        return;
    }

    if (!Config::triggerEnabled) {
        return;
    }

    static uint64_t acquiredAt = 0;
    static int lastTarget = 0;
    static int burstShotsRemaining = 0;
    static uint64_t nextBurstShotAt = 0;

    auto resetState = [&]() {
        acquiredAt = 0;
        lastTarget = 0;
        burstShotsRemaining = 0;
        nextBurstShotAt = 0;
    };

    if (menu::g_ShowMenu) {
        resetState();
        return;
    }

    // P1-11: honour triggerKeyMode and do not force IN_ATTACK when the trigger
    // key is not active (that would block the player's own fire button).
    int triggerKey = (Config::triggerKey == 0) ? VK_LBUTTON : Config::triggerKey;
    bool down = (GetAsyncKeyState(triggerKey) & 0x8000) != 0;

    bool shouldHold = down;
    if (Config::triggerKeyMode == 1) {
        static bool toggled = false;
        static bool wasDown = false;
        if (down && !wasDown) {
            toggled = !toggled;
        }
        wasDown = down;
        shouldHold = toggled;
    }

    if (!shouldHold) {
        resetState();
        return;
    }

    // CRASH-FIX: Cause 2 — never use a null/invalid local pawn.
    if (!g_LocalPlayerPawn ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(g_LocalPlayerPawn))) {
        resetState();
        return;
    }

    // Get entity index under crosshair
    int crosshairIndex = 0;
    bool haveCrosshair = memory::Read(
        reinterpret_cast<uintptr_t>(g_LocalPlayerPawn) + sdk::m_iIDEntIndex, crosshairIndex);

    int targetIndex = haveCrosshair ? (crosshairIndex & 0x7FFF) : 0;
    void* target = (targetIndex > 0 && targetIndex <= 64) ? Game::GetEntity(targetIndex) : nullptr;
    void* resolvedTarget = Game::GetPlayerPawn(target);
    if (resolvedTarget) target = resolvedTarget;

    bool valid = Game::IsValidPlayer(target, Config::triggerTeamCheck);
    if (valid && Config::triggerVisibleCheck) {
        valid = Game::IsVisible(target);
    }

    if (valid && Config::triggerHeadOnly) {
        sdk::Vector eye{}, head{}, view{};
        if (Game::GetLocalEyePosition(eye) && Game::GetBonePosition(target, kBoneHead, head) &&
            sdk::GetViewAngles(view)) {
            // Check if crosshair is near the head bone
            sdk::Vector angleToHead = CalculateAngle(eye, head);
            float angleDelta = sdk::AngleDelta(view, angleToHead);
            if (angleDelta > 3.0f) {
                valid = false;
            }
        }
    }

    if (!valid || !Game::CanFire()) {
        // Never clear the player's own IN_ATTACK here — that would block
        // normal fire whenever the crosshair isn't on a target (e.g. silent
        // aim's target is off-crosshair). Just stop attempting to fire.
        resetState();
        return;
    }

    // Restart the (once-sampled) delay whenever the target under the
    // crosshair changes.
    if (lastTarget != targetIndex) {
        lastTarget = targetIndex;
        acquiredAt = 0;
        burstShotsRemaining = Config::triggerBurstCount;
        nextBurstShotAt = 0;
        g_triggerSampledDelay = 0.0f;
    }

    if (acquiredAt == 0) {
        acquiredAt = GetTickCount64();
        nextBurstShotAt = acquiredAt;
    }

    // C-5: humanized delay — Gaussian sample inside [triggerMinDelay,
    // triggerMaxDelay]. Sampled ONCE per target acquisition so it does not
    // drift every tick.
    if (g_triggerSampledDelay <= 0.0f) {
        float minDelay = std::clamp(std::min(Config::triggerMinDelay, Config::triggerMaxDelay), 0.0f, 1000.0f);
        float maxDelay = std::clamp(std::max(Config::triggerMinDelay, Config::triggerMaxDelay), minDelay, 1000.0f);
        if (maxDelay <= minDelay) {
            maxDelay = minDelay + 1.0f;
        }
        float mid = (minDelay + maxDelay) * 0.5f;
        float sd = (maxDelay - minDelay) * 0.3f;
        static std::normal_distribution<float> g_delayDist(0.0f, 1.0f);
        float sample = mid + g_delayDist(g_rng) * sd;
        g_triggerSampledDelay = std::clamp(sample, minDelay, maxDelay);
    }

    uint64_t now = GetTickCount64();
    if (now >= acquiredAt + static_cast<uint64_t>(g_triggerSampledDelay) &&
        now >= nextBurstShotAt) {
        sdk::SetCmdButton(cmd, sdk::IN_ATTACK, true);

        if (Config::triggerBurstMode) {
            // C-5: burst mode — fire triggerBurstCount rounds, then pause.
            --burstShotsRemaining;
            if (burstShotsRemaining <= 0) {
                burstShotsRemaining = std::max(1, Config::triggerBurstCount);
                static std::uniform_int_distribution<int> g_burstPause(80, 150);
                nextBurstShotAt = now + static_cast<uint64_t>(g_burstPause(g_rng));
            } else {
                // Let the weapon's own cycle time pace shots within a burst.
                nextBurstShotAt = now + 1;
            }
        } else {
            // Hold IN_ATTACK; the engine releases it when we stop setting it.
            nextBurstShotAt = now + 1;
        }
    }
}

} // namespace Combat