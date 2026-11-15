// AUDIT: P1-1, P2-1
// UPGRADE: F-1, F-2, F-3
#pragma once

#include "sdk.h"

namespace sdk {

inline constexpr std::uint64_t IN_ATTACK = 1ull << 0;
inline constexpr std::uint64_t IN_JUMP = 1ull << 1;
inline constexpr std::uint64_t IN_USE = 1ull << 4;
inline constexpr std::uint64_t IN_MOVELEFT = 1ull << 9;
inline constexpr std::uint64_t IN_MOVERIGHT = 1ull << 10;

bool GetViewAngles(Vector& out);
bool SetViewAngles(const Vector& angles);
bool SetCmdAngles(CUserCmd* cmd, const Vector& angles, bool updateSubtick);
bool SetCmdButton(CUserCmd* cmd, std::uint64_t button, bool down);

    float GetCmdForwardMove(CUserCmd* cmd);
    float GetCmdSideMove(CUserCmd* cmd);
    void SetCmdForwardMove(CUserCmd* cmd, float value);
    void SetCmdSideMove(CUserCmd* cmd, float value);
    bool GetCmdButtons(CUserCmd* cmd, std::uint64_t& outButtons);

    bool GetCmdViewAngles(CUserCmd* cmd, Vector& out);

} // namespace sdk
