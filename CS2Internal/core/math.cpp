// AUDIT: P2-7
#include "math.h"
#include "memory.h"
#include "game.h"

#include <cmath>
#include <algorithm>
#include <optional>

namespace Math {

constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kDegToRad = 0.01745329251f;

void NormalizeAngles(sdk::Vector& angles) {
    // Delegate to the sdk QAngle normalization (pitch [-89,89], yaw wrapped,
    // z zeroed) so every caller gets identical, server-safe angles.
    angles = sdk::NormalizeAngles(angles);
}

float NormalizeYaw(float yaw) {
    yaw = std::remainder(yaw, 360.0f);
    // std::remainder already returns [-180, 180] for this input, but clamp
    // defensively to guard against platform-specific edge cases.
    if (yaw >= 180.0f) yaw = 179.999f;
    if (yaw <= -180.0f) yaw = -179.999f;
    return yaw;
}

sdk::Vector CalcAngle(const sdk::Vector& from, const sdk::Vector& to) {
    sdk::Vector delta = to - from;
    float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    sdk::Vector angle{};
    angle.x = -std::atan2(delta.z, horizontal) * kRadToDeg;
    angle.y = std::atan2(delta.y, delta.x) * kRadToDeg;
    angle.z = 0.0f;
    NormalizeAngles(angle);
    return angle;
}

float GetFov(const sdk::Vector& viewAngles, const sdk::Vector& targetAngles) {
    sdk::Vector delta = targetAngles - viewAngles;
    // M-7: pitch delta must NOT be yaw-normalized — near-opposite pitch flips sign magnitude.
    // Only yaw wraps; pitch stays in [-89, 89] so raw delta in [-178, 178] is correct.
    delta.y = NormalizeYaw(delta.y);
    delta.x = std::clamp(delta.x, -180.0f, 180.0f);
    delta.z = 0.0f;
    return std::sqrt(delta.x * delta.x + delta.y * delta.y);
}

void AngleVectors(const sdk::Vector& angles, sdk::Vector* forward, sdk::Vector* right, sdk::Vector* up) {
    float sp, cp;
    float sy, cy;
    sp = std::sin(angles.x * kDegToRad);
    cp = std::cos(angles.x * kDegToRad);
    sy = std::sin(angles.y * kDegToRad);
    cy = std::cos(angles.y * kDegToRad);

    if (forward) {
        forward->x = cp * cy;
        forward->y = cp * sy;
        forward->z = -sp;
    }

    if (right) {
        right->x = sy;
        right->y = -cy;
        right->z = 0.0f;
    }

    if (up) {
        up->x = sp * cy;
        up->y = sp * sy;
        up->z = cp;
    }
}

sdk::Vector GetForwardVector(const sdk::Vector& angles) {
    sdk::Vector forward;
    AngleVectors(angles, &forward, nullptr, nullptr);
    return forward;
}

float AngleNormalize(float angle) {
    return NormalizeYaw(angle);
}

// P2-7: Math::WorldToScreen was a duplicate of Game::WorldToScreen and had no
// callers. Game::WorldToScreen is the single projection implementation.

float DotProduct(const sdk::Vector& v1, const sdk::Vector& v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}

// M-5: return std::optional so callers can't accidentally use a zero vector on failure.
std::optional<sdk::Vector> ExtrapolatePosition(void* entity, float timeDelta) {
    sdk::Vector origin{};
    if (!Game::GetEntityOrigin(entity, origin)) {
        return std::nullopt;
    }

    sdk::Vector velocity{};
    if (memory::Read(reinterpret_cast<uintptr_t>(entity) + sdk::m_vecVelocity, velocity)) {
        origin.x += velocity.x * timeDelta;
        origin.y += velocity.y * timeDelta;
        origin.z += velocity.z * timeDelta;
    }

    return origin;
}

} // namespace Math