// AUDIT: P1-5, P1-10, P2-1, P2-2, P2-3, P2-4, P3-2, P3-3, P3-6, P4-1, P4-2, P4-3, P4-4
// UPGRADE: M-1, M-2, P-1, V-1
#pragma once

#include "../core/sdk.h"
#include <vector>

namespace Misc {

struct SpectatorInfo {
    char name[64] = {};
    int targetIndex = 0;
};

void Run(sdk::CUserCmd* cmd);
void Apply();
void PreCreateMove(); // called before origCreateMove: movement-services pre-write for auto-strafe
void InstallHooks();
void ApplyAutoAccept();
void ApplyAutoDefuse(sdk::CUserCmd* cmd);
void RevealRanks();

std::vector<SpectatorInfo> GetSpectators();
void StartBackgroundThread();
void StopBackgroundThread();

} // namespace Misc
