// AUDIT: P1-1, P2-1
// UPGRADE: F-1, F-2, F-3
// CRASH-FIX: Cause 3 --- every cmd/base-command/viewangles pointer is validated
// before use; on failure the function returns false and logs once ([CMD] ...).
// Offset proof: include/engine/cbaseusercmdpb.h documents has_bits at +0x10,
// forwardmove at +0x58 (bit 0x40), leftmove at +0x5C (bit 0x80) and viewangles
// at +0x40 (bit 0x4). No offsets below were changed; only the protobuf
// presence bits are now set so the engine actually consumes the written fields.
#include "usercmd.h"
#include "memory.h"
#include "game.h"
#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace sdk {

namespace {

// CUserCmd layout (include/engine/cusercmd.h, verified):
//   +0x10: CCSGOUserCmdPB embedded (its own fields start at +0x10)
//   +0x40: m_csgoUserCmd_base = CBaseUserCmdPB*   <-- CUserCmd-relative
// Note: base is +0x30 inside CCSGOUserCmdPB (include/engine/ccsgousercmdpb.h);
// 0x10 (embedded PB start) + 0x30 = 0x40. So kCmdBaseCommand is 0x40.
constexpr uintptr_t kCmdBaseCommand = 0x40;
constexpr uintptr_t kCmdButtonState = 0x58;
constexpr uintptr_t kButtonValue = 0x08;
constexpr uintptr_t kButtonChanged = 0x10;

constexpr uintptr_t kBaseButtonState = 0x38;
constexpr uintptr_t kBaseViewAngles = 0x40;
constexpr uintptr_t kBaseForwardMove = 0x58;
constexpr uintptr_t kBaseLeftMove = 0x5C;

// CBaseUserCmdPB presence bits (see include/engine/cbaseusercmdpb.h).
constexpr uintptr_t kBaseHasBits = 0x10;
constexpr uint32_t kHasButtons = 0x2;      // field 3 buttons_pb
constexpr uint32_t kHasViewAngles = 0x4;   // field 4 viewangles
constexpr uint32_t kHasForwardMove = 0x40; // field 5 forwardmove
constexpr uint32_t kHasLeftMove = 0x80;    // field 6 leftmove

// CInButtonStatePB presence bits (see include/engine/cinbuttonstatepb.h).
constexpr uint32_t kPbButtonState1 = 0x1;  // buttonstate1 (held mask)
constexpr uint32_t kPbButtonState2 = 0x2;  // buttonstate2 (changed mask)

void MarkFieldPresent(uintptr_t baseAddress, uint32_t bit) {
    uint32_t hasBits = 0;
    if (memory::Read(baseAddress + kBaseHasBits, hasBits)) {
        memory::Write(baseAddress + kBaseHasBits, hasBits | bit);
    }
}
constexpr uintptr_t kPbButtonValue = 0x18;
constexpr uintptr_t kPbButtonChanged = 0x20;
constexpr uintptr_t kPbAngleValue = 0x18;

constexpr uintptr_t kInputHistoryCurrent = 0x30;
constexpr uintptr_t kInputHistoryTotal = 0x34;
constexpr uintptr_t kInputHistoryRep = 0x38;
constexpr uintptr_t kHistoryViewAngles = 0x18;

uintptr_t GetClientBase() {
    Game::GameState state = Game::GetStateSnapshot();
    if (state.clientBase) {
        return state.clientBase;
    }
    HMODULE client = GetModuleHandleA("client.dll");
    return client ? reinterpret_cast<uintptr_t>(client) : 0;
}

void* ReadPointer(uintptr_t address) {
    uintptr_t value = 0;
    if (!memory::Read(address, value) || !memory::IsValidPtr(value)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(value);
}

// One-time-per-site logger for the stale-offset failure mode (Cause 3/CRASH-FIX).
void LogStaleCmdOnce(const char* what) {
    static const char* seen[32] = {};
    static int count = 0;
    if (count >= 32) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(seen[i], what) == 0) {
            return;
        }
    }
    seen[count++] = what;
    cs2::FileLog("[CMD] %s --- CUserCmd offsets stale?", what);
}

} // namespace

bool GetViewAngles(Vector& out) {
    uintptr_t client = GetClientBase();
    if (!client) {
        return false;
    }

    Vector angles{};
    if (!memory::Read(client + dwViewAngles, angles) || !angles.IsFinite()) {
        return false;
    }
    out = angles;
    return true;
}

bool SetViewAngles(const Vector& angles) {
    uintptr_t client = GetClientBase();
    if (!client) {
        return false;
    }
    return memory::Write(client + dwViewAngles, angles);
}

bool SetCmdAngles(CUserCmd* cmd, const Vector& angles, bool updateSubtick) {
    if (!cmd || !memory::IsValidPtr(reinterpret_cast<uintptr_t>(cmd))) {
        return false;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = nullptr;
    if (!memory::Read(cmdAddress + kCmdBaseCommand, baseCommand) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(baseCommand))) {
        static bool loggedBase = false;
        if (!loggedBase) {
            loggedBase = true;
            cs2::FileLog("[CMD] baseCommand read failed / null --- CUserCmd offsets stale?");
        }
        return false;
    }

    auto baseAddress = reinterpret_cast<uintptr_t>(baseCommand);
    void* viewAngles = nullptr;
    if (!memory::Read(baseAddress + kBaseViewAngles, viewAngles) ||
        !memory::IsValidPtr(reinterpret_cast<uintptr_t>(viewAngles))) {
        static bool loggedBaseVA = false;
        if (!loggedBaseVA) {
            loggedBaseVA = true;
            cs2::FileLog("[CMD] base+0x40 viewangles pointer invalid --- offsets stale?");
        }
        return false;
    }

    Vector normalized = NormalizeAngles(angles);
    if (!memory::Write(reinterpret_cast<uintptr_t>(viewAngles) + kPbAngleValue, normalized)) {
        return false;
    }
    MarkFieldPresent(baseAddress, kHasViewAngles);

    if (!updateSubtick) {
        return true;
    }

    uintptr_t rep = 0;
    if (!memory::Read(cmdAddress + kInputHistoryRep, rep) || !memory::IsValidPtr(rep)) {
        return true;
    }

    int current = 0;
    int total = 0;
    memory::Read(cmdAddress + kInputHistoryCurrent, current); // size = number of entries present
    memory::Read(cmdAddress + kInputHistoryTotal, total);    // capacity of the rep array

    // Count of valid history entries: the size field (+0x30), never *(rep)
    // (that would read the first entry pointer as a count). Cap at 32.
    int count = std::min({current, total, 32});
    if (count <= 0) {
        return true;
    }

    for (int i = 0; i < count; ++i) {
        // rep is the entry array; entry i is at rep + i*sizeof(void*). The old
        // rep + 0x8 + i*8 skipped entry 0 and misread the count.
        uintptr_t entry = 0;
        if (!memory::Read(rep + static_cast<uintptr_t>(i) * sizeof(void*), entry) ||
            !memory::IsValidPtr(entry)) {
            continue;
        }

        uintptr_t entryViewAngles = 0;
        if (!memory::Read(entry + kHistoryViewAngles, entryViewAngles) ||
            !memory::IsValidPtr(entryViewAngles)) {
            continue;
        }
        memory::Write(entryViewAngles + kPbAngleValue, normalized);
        // Mark this entry's view_angles field as present (has-bit 0x1) so the
        // server actually consumes the written angles for lag compensation.
        MarkFieldPresent(entry, 0x1);
    }
    return true;
}

bool SetCmdButton(CUserCmd* cmd, std::uint64_t button, bool down) {
    if (!cmd || button == 0) {
        return false;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    uintptr_t stateAddress = cmdAddress + kCmdButtonState;

    std::uint64_t value = 0;
    std::uint64_t changed = 0;
    if (!memory::Read(stateAddress + kButtonValue, value) ||
        !memory::Read(stateAddress + kButtonChanged, changed)) {
        return false;
    }

    std::uint64_t oldValue = value;
    if (down) {
        value |= button;
    } else {
        value &= ~button;
    }
    changed |= (oldValue ^ value);

    memory::Write(stateAddress + kButtonValue, value);
    memory::Write(stateAddress + kButtonChanged, changed);

    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in SetCmdButton");
        return true;
    }

    auto baseAddress = reinterpret_cast<uintptr_t>(baseCommand);
    uintptr_t pbState = 0;
    if (!memory::Read(baseAddress + kBaseButtonState, pbState) || !memory::IsValidPtr(pbState)) {
        LogStaleCmdOnce("pb button state invalid in SetCmdButton");
        return true;
    }

    std::uint64_t pbValue = 0;
    std::uint64_t pbChanged = 0;
    memory::Read(pbState + kPbButtonValue, pbValue);
    memory::Read(pbState + kPbButtonChanged, pbChanged);

    std::uint64_t oldPbValue = pbValue;
    if (down) {
        pbValue |= button;
    } else {
        pbValue &= ~button;
    }
    pbChanged |= (oldPbValue ^ pbValue);

    memory::Write(pbState + kPbButtonValue, pbValue);
    memory::Write(pbState + kPbButtonChanged, pbChanged);

    // The command fields we wrote are only present to the server if the
    // protobuf has-bits are set: buttons_pb (field 3, bit 0x2) on the base
    // command, and buttonstate1/2 (bits 0x1/0x2) on the PB itself. Without
    // these the server may ignore the injected jump/attack.
    MarkFieldPresent(baseAddress, kHasButtons);
    MarkFieldPresent(pbState, kPbButtonState1 | kPbButtonState2);
    return true;
}

float GetCmdForwardMove(CUserCmd* cmd) {
    if (!cmd) {
        return 0.0f;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in movement getter");
        return 0.0f;
    }

    float forward = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(baseCommand) + kBaseForwardMove, forward)) {
        return 0.0f;
    }
    return forward;
}

float GetCmdSideMove(CUserCmd* cmd) {
    if (!cmd) {
        return 0.0f;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in movement getter");
        return 0.0f;
    }

    float side = 0.0f;
    if (!memory::Read(reinterpret_cast<uintptr_t>(baseCommand) + kBaseLeftMove, side)) {
        return 0.0f;
    }
    return side;
}

bool GetCmdButtons(CUserCmd* cmd, std::uint64_t& outButtons) {
    if (!cmd) {
        return false;
    }
    return memory::Read(reinterpret_cast<uintptr_t>(cmd) + kCmdButtonState + kButtonValue,
                        outButtons);
}

void SetCmdForwardMove(CUserCmd* cmd, float value) {
    if (!cmd) {
        return;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in movement setter");
        return;
    }

    memory::Write(reinterpret_cast<uintptr_t>(baseCommand) + kBaseForwardMove, value);
    MarkFieldPresent(reinterpret_cast<uintptr_t>(baseCommand), kHasForwardMove);
}

void SetCmdSideMove(CUserCmd* cmd, float value) {
    if (!cmd) {
        return;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in movement setter");
        return;
    }

    memory::Write(reinterpret_cast<uintptr_t>(baseCommand) + kBaseLeftMove, value);
    MarkFieldPresent(reinterpret_cast<uintptr_t>(baseCommand), kHasLeftMove);
}

bool GetCmdViewAngles(CUserCmd* cmd, Vector& out) {
    if (!cmd) {
        return false;
    }

    auto cmdAddress = reinterpret_cast<uintptr_t>(cmd);
    void* baseCommand = ReadPointer(cmdAddress + kCmdBaseCommand);
    if (!baseCommand) {
        LogStaleCmdOnce("baseCommand null in GetCmdViewAngles");
        return false;
    }

    auto baseAddress = reinterpret_cast<uintptr_t>(baseCommand);
    void* viewAngles = ReadPointer(baseAddress + kBaseViewAngles);
    if (!viewAngles) {
        LogStaleCmdOnce("viewAngles pointer invalid in GetCmdViewAngles");
        return false;
    }

    return memory::Read(reinterpret_cast<uintptr_t>(viewAngles) + kPbAngleValue, out);
}

} // namespace sdk
