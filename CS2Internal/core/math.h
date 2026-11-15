// AUDIT: P2-7
#pragma once

#include "sdk.h"
#include <cmath>
#include <optional>

namespace Math {

sdk::Vector CalcAngle(const sdk::Vector& from, const sdk::Vector& to);
void NormalizeAngles(sdk::Vector& angles);
float NormalizeYaw(float yaw);
float GetFov(const sdk::Vector& viewAngles, const sdk::Vector& targetAngles);
sdk::Vector GetForwardVector(const sdk::Vector& angles);
std::optional<sdk::Vector> ExtrapolatePosition(void* entity, float timeDelta);
float DotProduct(const sdk::Vector& v1, const sdk::Vector& v2);
void AngleVectors(const sdk::Vector& angles, sdk::Vector* forward, sdk::Vector* right = nullptr, sdk::Vector* up = nullptr);
float AngleNormalize(float angle);

} // namespace Math