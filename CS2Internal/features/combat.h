// AUDIT: P1-2, P1-11, P3-11
// UPGRADE: C-1, C-5
#pragma once

#include "../core/sdk.h"

namespace Combat {

void Run(sdk::CUserCmd* cmd);
void RunTriggerbot(sdk::CUserCmd* cmd);

// Computes the view angle from eye position to target position.
sdk::Vector CalculateAngle(const sdk::Vector& from, const sdk::Vector& to);

// Returns true (and clears the flag) when the last Run() wrote target angles
// to the command. Used by hkCreateMove to restore the local pawn's eye angles
// so the camera never snaps.
bool SilentAimFired();

} // namespace Combat
