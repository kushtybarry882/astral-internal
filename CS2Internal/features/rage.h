// AUDIT: P1-2, P1-4, P3-4, P3-5
// UPGRADE: C-2, C-3, C-4, C-6, A-1, A-2
#pragma once

#include "../core/sdk.h"
#include "../core/math.h"
#include <vector>

namespace Rage {

// C-3: hitchance estimation from the weapon's accuracy penalty.
float EstimateHitchance(void* entity, const sdk::Vector& point, const sdk::Vector& eye);

// Ragebot and anti-aim functions
void RunAntiAim(sdk::CUserCmd* cmd);
void RunRage(sdk::CUserCmd* cmd);
void RunResolver();
void HandlePeek();
void Reset();
void SetTickBase(int tickBase);

} // namespace Rage