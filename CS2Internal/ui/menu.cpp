// AUDIT: P2-1, P2-2, P4-1, P4-2, P4-3, P4-4
// UPGRADE: S7-7, S7-8, S7-9, S7-10, S7-11, A-1
#include "menu.h"
#include "theme.h"

#include "../core/config.h"
#include "../core/config_io.h"
#include "../core/gameinfo.h"
#include "../core/memory.h"
#include "../core/sdk.h"
#include "../features/misc.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace menu {

static int g_selectedTab = 1;

namespace {

const char* kTabNames[] = { "LEGIT", "VISUALS", "MISC", "RAGE", "SETTINGS" };
constexpr int kTabCount = 5;

std::string g_selectedProfile;
std::vector<std::string> g_profiles;
std::chrono::steady_clock::time_point g_profilesNextRefresh = std::chrono::steady_clock::now();
char g_profileName[64] = "default";
std::string g_status;
bool g_statusError = false;
std::chrono::steady_clock::time_point g_statusUntil = std::chrono::steady_clock::now();
float g_fadeAlpha = 0.0f;

void SyncColor3To4(const ImVec4& v4, float (&color)[4]) {
    color[0] = v4.x;
    color[1] = v4.y;
    color[2] = v4.z;
    color[3] = v4.w; // M-9/M-11: write alpha explicitly, never silently drop it.
}

void SetStatus(const std::string& text, bool error = false, uint64_t durationMs = 3500) {
    g_status = text;
    g_statusError = error;
    g_statusUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
}

void RefreshProfiles(bool force = false) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now < g_profilesNextRefresh) {
        return;
    }
    g_profilesNextRefresh = now + std::chrono::seconds(1);
    g_profiles = ConfigIO::ListProfiles();

    if (!g_selectedProfile.empty() &&
        std::find(g_profiles.begin(), g_profiles.end(), g_selectedProfile) == g_profiles.end()) {
        g_selectedProfile.clear();
    }
}

void DoSave() {
    std::string actualName;
    std::string error;
    if (ConfigIO::SaveProfile(g_profileName, &actualName, &error)) {
        std::snprintf(g_profileName, sizeof(g_profileName), "%s", actualName.c_str());
        g_selectedProfile = actualName;
        RefreshProfiles(true);
        SetStatus("Saved " + actualName);
    } else {
        SetStatus(error.empty() ? "Save failed" : error, true);
    }
}

void DoLoad() {
    std::string name = g_profileName[0] ? std::string(g_profileName) : g_selectedProfile;
    if (name.empty()) {
        SetStatus("Select a profile first", true);
        return;
    }

    std::string error;
    if (ConfigIO::LoadProfile(name, &error)) {
        std::snprintf(g_profileName, sizeof(g_profileName), "%s", name.c_str());
        g_selectedProfile = name;
        RefreshProfiles(true);
        SetStatus("Loaded " + name);
    } else {
        SetStatus(error.empty() ? "Load failed" : error, true);
    }
}

void DoDelete() {
    std::string name = g_profileName[0] ? std::string(g_profileName) : g_selectedProfile;
    if (name.empty()) {
        SetStatus("Select a profile first", true);
        return;
    }

    std::string error;
    if (ConfigIO::DeleteProfile(name, &error)) {
        if (_stricmp(g_selectedProfile.c_str(), name.c_str()) == 0) {
            g_selectedProfile.clear();
        }
        RefreshProfiles(true);
        SetStatus("Deleted " + name);
    } else {
        SetStatus(error.empty() ? "Delete failed" : error, true);
    }
}

void DoReset() {
    Config::ResetDefaults();
    std::snprintf(g_profileName, sizeof(g_profileName), "%s", "default");
    SetStatus("Settings reset");
}

void DrawStatus() {
    if (g_status.empty() || std::chrono::steady_clock::now() > g_statusUntil) {
        return;
    }
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bgCol = g_statusError
        ? IM_COL32(229, 69, 82, 50)
        : IM_COL32(0, 229, 184, 50);
    dl->AddRectFilled(ImVec2(pos.x - 8.0f, pos.y - 2.0f),
                     ImVec2(pos.x + 200.0f, pos.y + 18.0f), bgCol, 4.0f);
    ImGui::PushFont(theme::SmallFont(), 12.0f);
    ImGui::TextColored(g_statusError ? ImVec4(0.83f, 0.11f, 0.20f, 1.0f) : ImVec4(0.0f, 0.88f, 0.72f, 1.0f), "%s", g_status.c_str());
    ImGui::PopFont();
}

void DrawProfileList(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.22f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.24f, 0.28f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("##profiles", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    if (g_profiles.empty()) {
        ImGui::PushFont(theme::SmallFont(), 12.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.65f, 0.7f, 1.0f), "No profiles saved");
        ImGui::PopFont();
    } else {
        for (const std::string& profile : g_profiles) {
            bool selected = (profile == g_selectedProfile);
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  selected ? ImVec4(0.20f, 0.55f, 0.65f, 0.30f) : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.55f, 0.65f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.55f, 0.65f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(0.20f, 0.85f, 0.85f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

            ImGui::PushID(profile.c_str());
            bool clicked = ImGui::Selectable(profile.c_str(), selected,
                                             ImGuiSelectableFlags_AllowDoubleClick,
                                             ImVec2(0.0f, 22.0f));
            if (clicked) {
                g_selectedProfile = profile;
                std::snprintf(g_profileName, sizeof(g_profileName), "%s", profile.c_str());
            }
            ImGui::PopID();
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::EndChild();
}

const char* GetKeyName(int key) {
    if (key == VK_LBUTTON) return "MOUSE1";
    if (key == VK_RBUTTON) return "MOUSE2";
    if (key == VK_MBUTTON) return "MOUSE3";
    if (key == VK_XBUTTON1) return "MOUSE4";
    if (key == VK_XBUTTON2) return "MOUSE5";
    if (key >= 'A' && key <= 'Z') {
        static char buf[2] = {0, 0};
        buf[0] = static_cast<char>(key);
        return buf;
    }
    if (key >= '0' && key <= '9') {
        static char buf[2] = {0, 0};
        buf[0] = static_cast<char>(key);
        return buf;
    }
    switch (key) {
        case VK_SPACE: return "SPACE";
        case VK_RETURN: return "ENTER";
        case VK_TAB: return "TAB";
        case VK_LSHIFT: return "LSHIFT";
        case VK_RSHIFT: return "RSHIFT";
        case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_LMENU: return "LAlt";
        case VK_RMENU: return "RAlt";
        case VK_ESCAPE: return "ESC";
        case VK_BACK: return "BACK";
        case VK_DELETE: return "DEL";
        case VK_INSERT: return "INS";
        case VK_HOME: return "HOME";
        case VK_END: return "END";
        case VK_PRIOR: return "PGUP";
        case VK_NEXT: return "PGDN";
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        case VK_F1: return "F1";
        case VK_F2: return "F2";
        case VK_F3: return "F3";
        case VK_F4: return "F4";
        case VK_F5: return "F5";
        case VK_F6: return "F6";
        case VK_F7: return "F7";
        case VK_F8: return "F8";
        case VK_F9: return "F9";
        case VK_F10: return "F10";
        case VK_F11: return "F11";
        case VK_F12: return "F12";
        default: return "...";
    }
}

bool KeybindButton(const char* label, int& key, float width = 110.0f) {
    static bool g_listening = false;
    static const char* g_listenLabel = nullptr;
    static int g_listenKey = 0;

    const char* display = GetKeyName(key);

    ImGui::PushID(label);
    bool clicked = ImGui::Button(g_listening ? "PRESS KEY..." : display, ImVec2(width, 22.0f));
    if (clicked) {
        g_listening = true;
        g_listenLabel = label;
        g_listenKey = 0;
    }
    ImGui::PopID();

    if (g_listening && g_listenLabel == label) {
        // M-8: require a key transition (down then up) before sampling, so the
        // mouse button that opened the binder isn't immediately captured.
        for (int i = 1; i < 256; ++i) {
            bool down = (GetAsyncKeyState(i) & 0x8000) != 0;
            if (down && g_listenKey == 0) {
                g_listenKey = i; // first seen down
            }
            if (!down && g_listenKey == i) {
                key = i;
                g_listening = false;
                g_listenLabel = nullptr;
                g_listenKey = 0;
                return true;
            }
        }
    }

    return false;
}

void DrawLegitTab() {
    ImGui::PushID("legit");

    // --- AIMBOT Section ---
    ImGui::Text("AIMBOT");
    ImGui::Separator();
    ImGui::PushID("aimbot");
    ImGui::Checkbox("ENABLED", &Config::aimbotEnabled);
    KeybindButton("Aim Key", Config::aimKey);
    ImGui::SameLine(0, 8);
    static const char* keyModes[] = { "HOLD", "TOGGLE" };
    int km = Config::aimKeyMode;
    ImGui::Combo("##keymode", &km, keyModes, IM_ARRAYSIZE(keyModes));
    Config::aimKeyMode = std::clamp(km, 0, 1);

    static const char* bones[] = { "Head", "Neck", "Chest", "Pelvis" };
    int boneIdx = Config::aimBone;
    ImGui::Combo("Bone", &boneIdx, bones, IM_ARRAYSIZE(bones));
    Config::aimBone = std::clamp(boneIdx, 0, 3);

    static const char* targetModes[] = { "FOV", "Distance", "Health", "Crosshair" };
    int targetIdx = Config::aimTargetSelection;
    ImGui::Combo("Target Selection", &targetIdx, targetModes, IM_ARRAYSIZE(targetModes));
    Config::aimTargetSelection = std::clamp(targetIdx, 0, 3);

    ImGui::SliderFloat("FOV", &Config::aimFov, 0.1f, 30.0f, "%.1f deg");
    ImGui::SliderFloat("Smooth", &Config::aimSmooth, 1.0f, 20.0f, "%.1f");
    ImGui::Checkbox("Silent Aim", &Config::aimSilent);
    if (Config::aimSilent) {
        ImGui::Checkbox("Silent Auto-Fire", &Config::aimSilentAutoFire);
    }
    ImGui::Checkbox("Auto Wall", &Config::aimAutoWall);
    ImGui::Checkbox("Team Check", &Config::aimTeamCheck);
    ImGui::Checkbox("Visible Check", &Config::aimVisibleCheck);
    ImGui::Checkbox("Recoil Control", &Config::aimRcs);
    ImGui::SliderFloat("RCS X", &Config::aimRcsX, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("RCS Y", &Config::aimRcsY, 0.0f, 2.0f, "%.2f");
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- TRIGGERBOT Section ---
    ImGui::Text("TRIGGERBOT");
    ImGui::Separator();
    ImGui::PushID("triggerbot");
    ImGui::Checkbox("ENABLED", &Config::triggerEnabled);
    KeybindButton("Trig Key", Config::triggerKey);
    ImGui::SameLine(0, 8);
    int tkm = Config::triggerKeyMode;
    ImGui::Combo("##trigkeymode", &tkm, keyModes, IM_ARRAYSIZE(keyModes));
    Config::triggerKeyMode = std::clamp(tkm, 0, 1);
    ImGui::SliderFloat("Delay", &Config::triggerDelay, 0.0f, 500.0f, "%.0f ms");
    ImGui::SliderFloat("Min Delay", &Config::triggerMinDelay, 0.0f, 1000.0f, "%.0f ms");
    ImGui::SliderFloat("Max Delay", &Config::triggerMaxDelay, 0.0f, 1000.0f, "%.0f ms");
    ImGui::Checkbox("Burst Mode", &Config::triggerBurstMode);
    if (Config::triggerBurstMode) {
        ImGui::SliderInt("Burst Count", &Config::triggerBurstCount, 1, 20);
    }
    ImGui::Checkbox("Visible Check", &Config::triggerVisibleCheck);
    ImGui::Checkbox("Team Check", &Config::triggerTeamCheck);
    ImGui::Checkbox("Head Only", &Config::triggerHeadOnly);
    ImGui::PopID();
    ImGui::PopID();

    DrawStatus();
}

void DrawVisualsTab() {
    ImGui::BeginChild("LeftColumn", ImVec2(290, 310), false);
    {
        ImGui::Text("ESP");
        ImGui::Separator();
        ImGui::Checkbox("PLAYER ESP", &Config::espEnabled);
        ImGui::Checkbox("CHAMS", &Config::chamsEnabled);
        ImGui::Checkbox("BOX ESP", &Config::espBoxes);
        ImGui::Checkbox("HEALTH BAR", &Config::espHealth);
        ImGui::Checkbox("ARMOR BAR", &Config::armorBar);
        ImGui::Checkbox("DISTANCE", &Config::espDistance);
        ImGui::Checkbox("SNAKELINES", &Config::espSnaplines);
        ImGui::Checkbox("SKELETON", &Config::espSkeleton);
        ImGui::Checkbox("HITBOXES", &Config::espHitboxes);

        ImGui::Spacing();
        ImGui::Text("COLORS");
        ImGui::Separator();
        ImGui::ColorEdit3("Enemy Visible", Config::espEnemyColor, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Enemy Occluded", Config::espOccludedColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit3("Team Visible", Config::espTeamColor, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Team Occluded", Config::espOccludedTeamColor, ImGuiColorEditFlags_NoInputs);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("RightColumn", ImVec2(290, 310), false);
    {
        ImGui::SliderFloat("ESP DISTANCE", &Config::espMaxDist, 0.0f, 2000.0f, "%.0fm");
        ImGui::SliderFloat("CHAMS BRIGHTNESS", &Config::chamsBrightness, 0.0f, 100.0f, "%.0f%%");
        ImGui::SliderFloat("GLOW INTENSITY", &Config::glowIntensity, 0.0f, 100.0f, "%.0f%%");

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Checkbox("GLOW", &Config::glowEnabled);
    }
    ImGui::EndChild();

    DrawStatus();
}

void DrawMiscTab() {
    ImGui::PushID("misc");

    ImGui::Text("MOVEMENT");
    ImGui::Separator();
    ImGui::PushID("movement");
    ImGui::Checkbox("BUNNY HOP", &Config::bhopEnabled);
    if (Config::bhopEnabled) {
        KeybindButton("Bhop Key", Config::bhopKey);
        ImGui::SliderFloat("Bhop Humanity", &Config::bhopHumanity, 0.0f, 100.0f, "%.0f%%");
    }
    ImGui::Checkbox("AUTO STRAFE", &Config::autoStrafeEnabled);
    if (Config::autoStrafeEnabled) {
        static const char* strafeModes[] = { "ASSIST", "AUTO" };
        int strafeMode = Config::autoStrafeMode;
        ImGui::Combo("Strafe Mode", &strafeMode, strafeModes, IM_ARRAYSIZE(strafeModes));
        Config::autoStrafeMode = std::clamp(strafeMode, 0, 1);
        ImGui::SliderFloat("Strafe Humanity", &Config::autoStrafeHumanity, 0.0f, 100.0f, "%.0f%%");
        KeybindButton("Strafe Key", Config::autoStrafeSwitchKey);
        if (Config::autoStrafeMode == 1) {
            ImGui::SliderFloat("Max Turn", &Config::autoStrafeMaxTurn, 1.0f, 720.0f, "%.0f deg/s");
        }
    }
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("VISUAL");
    ImGui::Separator();
    ImGui::PushID("visual");
    ImGui::Checkbox("NO FLASH", &Config::noFlashEnabled);
    ImGui::Checkbox("NO SMOKE", &Config::noSmokeEnabled);
    ImGui::Checkbox("NO RECOIL", &Config::noRecoilEnabled);
    ImGui::Checkbox("NO SPREAD", &Config::noSpreadEnabled);
    ImGui::Checkbox("BACKTRACK", &Config::backtrackEnabled);
    ImGui::Checkbox("THIRDPERSON", &Config::thirdpersonEnabled);
    if (Config::thirdpersonEnabled) {
        bool tpAlways = (Config::thirdpersonKey == 0);
        if (ImGui::Checkbox("TP ALWAYS ON", &tpAlways)) {
            Config::thirdpersonKey = tpAlways ? 0 : 'V';
        }
        if (!tpAlways) {
            KeybindButton("TP Key", Config::thirdpersonKey);
        }
    }
    ImGui::SliderFloat("TP DISTANCE", &Config::thirdpersonDistance, 50.0f, 500.0f, "%.0f");
    ImGui::Checkbox("CROSSHAIR", &Config::crosshairEnabled);
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("CAMERA");
    ImGui::Separator();
    ImGui::PushID("camera");
    ImGui::Checkbox("FOV CHANGER", &Config::fovChangerEnabled);
    ImGui::SliderFloat("VIEW FOV", &Config::fovValue, 60.0f, 140.0f, "%.0f deg");
    ImGui::Checkbox("VIEWMODEL FOV", &Config::viewmodelFovEnabled);
    ImGui::SliderFloat("VM FOV", &Config::viewmodelFov, 40.0f, 120.0f, "%.0f");
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("MISCELLANEOUS");
    ImGui::Separator();
    ImGui::PushID("misc_misc");
    ImGui::Checkbox("CLAN TAG CHANGER", &Config::clanChangerEnabled);
    ImGui::InputTextWithHint("##clan_tag", "clan tag text", Config::clanTag, sizeof(Config::clanTag));
    ImGui::Checkbox("SPECTATOR LIST", &Config::spectatorListEnabled);
    ImGui::Checkbox("REVEAL RANKS", &Config::revealRanksEnabled);
    ImGui::Checkbox("AUTO ACCEPT", &Config::autoAcceptEnabled);
    ImGui::Checkbox("AUTO DEFUSE", &Config::autoDefuseEnabled);
    ImGui::PopID();
    ImGui::PopID();

    DrawStatus();
}

void DrawRageTab() {
    ImGui::PushID("rage");

    ImGui::Text("RAGEBOT");
    ImGui::Separator();
    ImGui::PushID("ragebot");
    ImGui::Checkbox("ENABLED", &Config::ragebotEnabled);
    ImGui::Checkbox("AUTO FIRE", &Config::rageAutoFire);
    ImGui::Checkbox("AUTO WALL", &Config::rageAutoWall);
    ImGui::SliderFloat("MIN DAMAGE", &Config::rageMinDamage, 1.0f, 120.0f, "%.0f hp");
    ImGui::SliderFloat("HITCHANCE", &Config::rageHitchance, 1.0f, 100.0f, "%.0f%%");
    ImGui::SliderFloat("FOV", &Config::rageFov, 1.0f, 180.0f, "%.0f deg");

    static const char* targets[] = { "Threat", "Distance", "Crosshair" };
    int targetIdx = Config::rageTargetSelection;
    ImGui::Combo("Target Selection", &targetIdx, targets, IM_ARRAYSIZE(targets));
    Config::rageTargetSelection = std::clamp(targetIdx, 0, 2);

    static const char* bones[] = { "Head", "Chest", "Pelvis" };
    int rb = Config::rageBone;
    ImGui::Combo("Bone", &rb, bones, IM_ARRAYSIZE(bones));
    Config::rageBone = std::clamp(rb, 0, 2);

    ImGui::Checkbox("AUTO PEEK", &Config::rageAutoPeek);
    ImGui::SliderInt("BACKTRACK TICKS", &Config::backtrackTicks, 1, 12, "%d");
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("ANTI-AIM");
    ImGui::Separator();
    ImGui::PushID("antiaim");
    ImGui::Checkbox("ENABLED", &Config::antiAimEnabled);

    static const char* pitches[] = { "Down", "Fake", "Zero", "Up", "Jitter" };
    int p = Config::antiAimPitch;
    ImGui::Combo("Pitch", &p, pitches, IM_ARRAYSIZE(pitches));
    Config::antiAimPitch = std::clamp(p, 0, 4);

    static const char* yaws[] = { "Spin", "Jitter", "Static", "LBY" };
    int y = Config::antiAimYaw;
    ImGui::Combo("Yaw", &y, yaws, IM_ARRAYSIZE(yaws));
    Config::antiAimYaw = std::clamp(y, 0, 3);

    if (Config::antiAimYaw == 0) {
        ImGui::SliderFloat("Spin Speed", &Config::antiAimSpinSpeed, 1.0f, 360.0f, "%.0f/s");
    }
    if (Config::antiAimYaw == 1 || Config::antiAimYaw == 3) {
        ImGui::Checkbox("JITTER", &Config::antiAimJitter);
    }
    if (Config::antiAimYaw == 2) {
        ImGui::SliderFloat("Yaw Offset", &Config::antiAimYawOffset, -180.0f, 180.0f, "%.0f");
    }

    ImGui::SliderFloat("DESYNC ANGLE", &Config::antiAimDesyncAngle, 1.0f, 120.0f, "%.0f");

    // Condensed anti-aim flags into a single row
    ImGui::Checkbox("LBY", &Config::antiAimLBY);
    ImGui::SameLine();
    ImGui::Checkbox("EDGE", &Config::antiAimEdge);
    ImGui::SameLine();
    ImGui::Checkbox("FREESTANDING", &Config::antiAimFreestanding);
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("RESOLVER");
    ImGui::Separator();
    ImGui::PushID("resolver");
    ImGui::Checkbox("ENABLED", &Config::resolverEnabled);
    ImGui::PopID();
    ImGui::PopID();

    DrawStatus();
}

void DrawSettingsTab() {
    ImGui::Text("PROFILES");
    ImGui::Separator();
    ImGui::InputTextWithHint("##profile_name", "profile name", g_profileName, sizeof(g_profileName));

    float buttonW = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    if (ImGui::Button("SAVE", ImVec2(buttonW, 28.0f))) {
        DoSave();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("LOAD", ImVec2(buttonW, 28.0f))) {
        DoLoad();
    }

    ImGui::Spacing();
    float thirdW = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
    if (ImGui::Button("DELETE", ImVec2(thirdW, 26.0f))) {
        DoDelete();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("RESET", ImVec2(thirdW, 26.0f))) {
        DoReset();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("RELOAD", ImVec2(thirdW, 26.0f))) {
        RefreshProfiles(true);
        SetStatus("Profile list refreshed");
    }

    ImGui::Spacing();
    float listH = std::max(120.0f, ImGui::GetContentRegionAvail().y - 24.0f);
    DrawProfileList(listH);

    ImGui::Spacing();
    ImGui::Text("INFORMATION");
    ImGui::Separator();
    ImGui::Text("Build: 14192");
    ImGui::Text("Loader: Manual map");
    ImGui::Text("Menu Toggle: RIGHT SHIFT");
    ImGui::Text("Config Path: %s", ConfigIO::ConfigPath().c_str());

    DrawStatus();
}

} // namespace

void Render() {
    if (g_ShowMenu) {
        g_fadeAlpha = std::min(1.0f, g_fadeAlpha + ImGui::GetIO().DeltaTime * 5.0f);
    } else {
        g_fadeAlpha = std::max(0.0f, g_fadeAlpha - ImGui::GetIO().DeltaTime * 8.0f);
    }

    RefreshProfiles();

    ImGuiIO& io = ImGui::GetIO();
    // N-L-16: DPI-aware menu sizing.
    float dpiScale = io.FontGlobalScale > 0.0f ? io.FontGlobalScale : 1.0f;
    ImVec2 windowSize(620.0f * dpiScale, 410.0f * dpiScale);
    ImVec2 windowPosition(
        std::max(0.0f, (io.DisplaySize.x - windowSize.x) * 0.5f),
        std::max(0.0f, (io.DisplaySize.y - windowSize.y) * 0.5f));

    ImGui::SetNextWindowSize(windowSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(windowPosition, ImGuiCond_FirstUseEver);
    if (ImGui::GetIO().DisplaySize.x > 0 && ImGui::GetIO().DisplaySize.y > 0) {
        // Allow manual resizing by not forcing fixed size
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_fadeAlpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.14f, 0.18f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.24f, 0.28f, 0.5f));

    ImGui::Begin("##LEGIONNAIRE_MENU", &g_ShowMenu,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoResize);

    if (g_fadeAlpha < 0.01f) {
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        return;
    }

    // Draggable title region — use ImGui's built-in window dragging instead of
    // fighting ImGui::SetWindowPos every frame (N-L-17).
    ImGui::InvisibleButton("##drag_region", ImVec2(ImGui::GetContentRegionAvail().x, 22));
    if (ImGui::IsItemActive() && ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos(ImVec2(pos.x + delta.x, pos.y + delta.y));
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);

    // --- TOP TABS BAR ---
    for (int i = 0; i < kTabCount; i++) {
        bool selected = (g_selectedTab == i);
        ImVec4 btnColor = selected
            ? ImVec4(0.20f, 0.85f, 0.85f, 0.15f)
            : ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
        ImVec4 tintColor = selected
            ? ImVec4(0.20f, 0.85f, 0.85f, 1.0f)
            : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.85f, 0.85f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.55f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, tintColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnColor);
            ImGui::PushStyleColor(ImGuiCol_Text, tintColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        }

        if (ImGui::Button(kTabNames[i], ImVec2(110.0f * dpiScale, 25.0f * dpiScale))) {
            g_selectedTab = i;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        if (i < kTabCount - 1) {
            ImGui::SameLine();
        }
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);

    // --- TAB CONTENT (scrollable if overflow) ---
    float contentHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##tab_content", ImVec2(0, contentHeight), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiChildFlags_AlwaysUseWindowPadding);

    switch (g_selectedTab) {
        case 0:  DrawLegitTab();     break;
        case 1:  DrawVisualsTab();   break;
        case 2:  DrawMiscTab();      break;
        case 3:  DrawRageTab();      break;
        case 4:  DrawSettingsTab();  break;
        default: DrawLegitTab();     break;
    }

    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DrawSpectatorList() {
    if (!Config::spectatorListEnabled || !g_LocalPlayerPawn) {
        return;
    }

    const auto spectators = Misc::GetSpectators();
    if (spectators.empty()) {
        return;
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    float startY = ImGui::GetIO().DisplaySize.y - 24.0f - static_cast<float>(spectators.size() * 20);
    ImVec2 pos(18.0f, startY);

    char title[64];
    std::snprintf(title, sizeof(title), "Spectators (%u)", static_cast<unsigned>(spectators.size()));

    drawList->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 4.0f),
                           ImVec2(pos.x + 180.0f, pos.y + 24.0f + spectators.size() * 20.0f),
                           IM_COL32(18, 25, 32, 204), 6.0f);

    drawList->AddText(ImVec2(pos.x, pos.y), IM_COL32(0, 229, 184, 255), title);
    pos.y += 24.0f;

    for (const auto& spec : spectators) {
        drawList->AddText(ImVec2(pos.x, pos.y), IM_COL32(245, 245, 247, 255), spec.name);
        pos.y += 18.0f;
    }
}

} // namespace menu
