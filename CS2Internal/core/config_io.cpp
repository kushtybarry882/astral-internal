// AUDIT: P2-1, P2-9, P4-1, P4-2, P4-3, P4-4
// UPGRADE: M-3, S7-1, S7-2, S7-3, S7-4, S7-5, S7-6
#include "config_io.h"
#include "config.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ConfigIO {

namespace {

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string SanitizeName(const std::string& value) {
    std::string result = Trim(value);
    if (result.empty()) {
        result = "default";
    }

    for (char& c : result) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }

    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }
    if (result.empty()) {
        result = "default";
    }
    if (result.size() > 48) {
        result.resize(48);
    }
    return result;
}

std::filesystem::path ProfilePath(const std::string& name) {
    return ConfigDirectory() / (SanitizeName(name) + ".cfg");
}

float ToFloat(const std::string& value, float fallback = 0.0f) {
    char* end = nullptr;
    float result = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) {
        return fallback;
    }
    // M-1: reject NaN / Inf — they bypass every clamp (NaN compares false to everything).
    if (!std::isfinite(result)) {
        return fallback;
    }
    return result;
}

int ToInt(const std::string& value, int fallback = 0) {
    char* end = nullptr;
    long result = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<int>(result);
}

bool ToBool(const std::string& value, bool fallback = false) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        return false;
    }
    return fallback;
}

void ParseColor(const std::string& value, float (&color)[4]) {
    std::istringstream stream(value);
    for (int i = 0; i < 4; ++i) {
        if (!(stream >> color[i])) {
            return;
        }
        color[i] = std::clamp(color[i], 0.0f, 1.0f);
    }
}

void WriteColor(std::ofstream& file, const char* key, const float (&color)[4]) {
    file << key << '=' << color[0] << ' ' << color[1] << ' ' << color[2] << ' ' << color[3] << '\n';
}

void ClampSettings() {
    // M-1: reject NaN/Inf before clamping — NaN compares false to everything.
    auto clampf = [](float& v, float lo, float hi) {
        if (!std::isfinite(v)) v = lo;
        v = std::clamp(v, lo, hi);
    };
    auto clamplegacy = [](int& v, int lo, int hi) {
        v = std::clamp(v, lo, hi);
    };

    clampf(Config::aimFov, 0.1f, 30.0f);
    clampf(Config::aimSmooth, 1.0f, 20.0f);
    clamplegacy(Config::aimBone, 0, 3);
    clamplegacy(Config::aimTargetSelection, 0, 3);
    clampf(Config::triggerDelay, 0.0f, 500.0f);
    clampf(Config::triggerRcsX, 0.0f, 2.0f);
    clampf(Config::triggerRcsY, 0.0f, 2.0f);
    clampf(Config::aimRcsX, 0.0f, 2.0f);
    clampf(Config::aimRcsY, 0.0f, 2.0f);
    clampf(Config::espMaxDist, 100.0f, 10000.0f);
    clampf(Config::chamsBrightness, 0.0f, 100.0f);
    clampf(Config::glowIntensity, 0.0f, 100.0f);
    clampf(Config::radarSize, 80.0f, 400.0f);
    clampf(Config::glowRange, 100.0f, 10000.0f);
    clampf(Config::bulletTracerDuration, 0.1f, 10.0f);
    clampf(Config::rageMinDamage, 1.0f, 120.0f);
    clampf(Config::rageHitchance, 1.0f, 100.0f);
    clamplegacy(Config::backtrackTicks, 1, 12);
    clampf(Config::antiAimSpinSpeed, 1.0f, 360.0f);
    clampf(Config::antiAimDesyncAngle, 1.0f, 120.0f);
    clampf(Config::fovValue, 60.0f, 140.0f);
    clampf(Config::viewmodelFov, 40.0f, 120.0f);
    clampf(Config::thirdpersonDistance, 50.0f, 500.0f);
    clampf(Config::autoStrafeHumanity, 0.0f, 100.0f);
    clampf(Config::bhopHumanity, 0.0f, 100.0f);
    clampf(Config::autoStrafeMaxTurn, 1.0f, 720.0f);
    clamplegacy(Config::autoStrafeMode, 0, 1);
    clamplegacy(Config::triggerKeyMode, 0, 1);
    clampf(Config::triggerMinDelay, 0.0f, 1000.0f);
    clampf(Config::triggerMaxDelay, Config::triggerMinDelay, 1000.0f);
    clamplegacy(Config::triggerBurstCount, 1, 20);
    clamplegacy(Config::autoStrafeSwitchKey, 0, 255);

    // M-12: clamp anti-aim pitch/yaw enums — any int from config is accepted unclamped.
    clamplegacy(Config::antiAimPitch, 0, 4);
    clamplegacy(Config::antiAimYaw, 0, 3);
}

} // namespace

std::filesystem::path ConfigDirectory() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    std::filesystem::path root;
    if (localAppData && localAppData[0]) {
        root = std::filesystem::path(localAppData) / "ASTRAL" / "profiles";
    } else {
        char temp[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, temp) != 0) {
            root = std::filesystem::path(temp) / "ASTRAL" / "profiles";
        } else {
            root = std::filesystem::path("ASTRAL") / "profiles";
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

std::string ConfigPath() {
    return ConfigDirectory().string();
}

std::vector<std::string> ListProfiles() {
    std::vector<std::string> result;
    std::error_code ec;

    auto directory = ConfigDirectory();
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != ".cfg") {
            continue;
        }
        result.push_back(entry.path().stem().string());
    }

    std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    return result;
}

bool SaveProfile(const std::string& name, std::string* actualName, std::string* error) {
    std::string profile = SanitizeName(name);
    if (actualName) {
        *actualName = profile;
    }

    std::error_code ec;
    std::filesystem::create_directories(ConfigDirectory(), ec);
    if (ec) {
        if (error) *error = "Could not create config directory";
        return false;
    }

    std::ofstream file(ProfilePath(profile).wstring(), std::ios::trunc);
    if (!file.is_open()) {
        if (error) *error = "Could not open profile file";
        return false;
    }

    file << "# ASTRAL configuration\n";
    file << "version=1\n";

    file << "aimbotEnabled=" << (Config::aimbotEnabled ? 1 : 0) << '\n';
    file << "aimKey=" << Config::aimKey << '\n';
    file << "aimKeyMode=" << Config::aimKeyMode << '\n';
    file << "aimFov=" << Config::aimFov << '\n';
    file << "aimSmooth=" << Config::aimSmooth << '\n';
    file << "aimSilent=" << (Config::aimSilent ? 1 : 0) << '\n';
    file << "aimAutoWall=" << (Config::aimAutoWall ? 1 : 0) << '\n';
    file << "aimVisibleCheck=" << (Config::aimVisibleCheck ? 1 : 0) << '\n';
    file << "aimTeamCheck=" << (Config::aimTeamCheck ? 1 : 0) << '\n';
    file << "aimBone=" << Config::aimBone << '\n';
    file << "aimTargetSelection=" << Config::aimTargetSelection << '\n';
    file << "aimSilentAutoFire=" << (Config::aimSilentAutoFire ? 1 : 0) << '\n';
    file << "aimRcs=" << (Config::aimRcs ? 1 : 0) << '\n';
    file << "aimRcsX=" << Config::aimRcsX << '\n';
    file << "aimRcsY=" << Config::aimRcsY << '\n';

    file << "triggerEnabled=" << (Config::triggerEnabled ? 1 : 0) << '\n';
    file << "triggerKey=" << Config::triggerKey << '\n';
    file << "triggerKeyMode=" << Config::triggerKeyMode << '\n';
    file << "triggerDelay=" << Config::triggerDelay << '\n';
    file << "triggerVisibleCheck=" << (Config::triggerVisibleCheck ? 1 : 0) << '\n';
    file << "triggerTeamCheck=" << (Config::triggerTeamCheck ? 1 : 0) << '\n';
    file << "triggerHeadOnly=" << (Config::triggerHeadOnly ? 1 : 0) << '\n';
    file << "triggerRcs=" << (Config::triggerRcs ? 1 : 0) << '\n';
    file << "triggerRcsX=" << Config::triggerRcsX << '\n';
    file << "triggerRcsY=" << Config::triggerRcsY << '\n';
    file << "triggerMinDelay=" << Config::triggerMinDelay << '\n';
    file << "triggerMaxDelay=" << Config::triggerMaxDelay << '\n';
    file << "triggerBurstMode=" << (Config::triggerBurstMode ? 1 : 0) << '\n';
    file << "triggerBurstCount=" << Config::triggerBurstCount << '\n';

    file << "espEnabled=" << (Config::espEnabled ? 1 : 0) << '\n';
    file << "espBoxes=" << (Config::espBoxes ? 1 : 0) << '\n';
    file << "espNames=" << (Config::espNames ? 1 : 0) << '\n';
    file << "espHealth=" << (Config::espHealth ? 1 : 0) << '\n';
    file << "espDistance=" << (Config::espDistance ? 1 : 0) << '\n';
    file << "espSkeleton=" << (Config::espSkeleton ? 1 : 0) << '\n';
    file << "espSnaplines=" << (Config::espSnaplines ? 1 : 0) << '\n';
    file << "espGlow=" << (Config::espGlow ? 1 : 0) << '\n';
    file << "espHitboxes=" << (Config::espHitboxes ? 1 : 0) << '\n';
    file << "armorBar=" << (Config::armorBar ? 1 : 0) << '\n';
    file << "crosshairEnabled=" << (Config::crosshairEnabled ? 1 : 0) << '\n';
    file << "glowIntensity=" << Config::glowIntensity << '\n';
    file << "espMaxDist=" << Config::espMaxDist << '\n';
    file << "radarEnabled=" << (Config::radarEnabled ? 1 : 0) << '\n';
    file << "radarSize=" << Config::radarSize << '\n';
    WriteColor(file, "espEnemyColor", Config::espEnemyColor);
    WriteColor(file, "espOccludedColor", Config::espOccludedColor);
    WriteColor(file, "espTeamColor", Config::espTeamColor);
    WriteColor(file, "espOccludedTeamColor", Config::espOccludedTeamColor);
    WriteColor(file, "espSkeletonColor", Config::espSkeletonColor);

    file << "glowEnabled=" << (Config::glowEnabled ? 1 : 0) << '\n';
    file << "glowTeamCheck=" << (Config::glowTeamCheck ? 1 : 0) << '\n';
    file << "glowOccluded=" << (Config::glowOccluded ? 1 : 0) << '\n';
    file << "glowRange=" << Config::glowRange << '\n';
    WriteColor(file, "glowEnemyColor", Config::glowEnemyColor);
    WriteColor(file, "glowTeamColor", Config::glowTeamColor);
    WriteColor(file, "glowOccludedEnemyColor", Config::glowOccludedEnemyColor);
    WriteColor(file, "glowOccludedTeamColor", Config::glowOccludedTeamColor);

    file << "bulletTracersEnabled=" << (Config::bulletTracersEnabled ? 1 : 0) << '\n';
    file << "TracerDuration=" << Config::bulletTracerDuration << '\n';
    WriteColor(file, "bulletTracerColor", Config::bulletTracerColor);

    file << "chamsEnabled=" << (Config::chamsEnabled ? 1 : 0) << '\n';
    file << "chamsEnemyVisible=" << (Config::chamsEnemyVisible ? 1 : 0) << '\n';
    file << "chamsEnemyOccluded=" << (Config::chamsEnemyOccluded ? 1 : 0) << '\n';
    WriteColor(file, "chamsEnemyColor", Config::chamsEnemyColor);
    WriteColor(file, "chamsEnemyOccludedColor", Config::chamsEnemyOccludedColor);
    file << "chamsTeamVisible=" << (Config::chamsTeamVisible ? 1 : 0) << '\n';
    file << "chamsTeamOccluded=" << (Config::chamsTeamOccluded ? 1 : 0) << '\n';
    WriteColor(file, "chamsTeamColor", Config::chamsTeamColor);
    WriteColor(file, "chamsTeamOccludedColor", Config::chamsTeamOccludedColor);
    file << "chamsBrightness=" << Config::chamsBrightness << '\n';

    file << "bhopEnabled=" << (Config::bhopEnabled ? 1 : 0) << '\n';
    file << "bhopKey=" << Config::bhopKey << '\n';
    file << "bhopHumanity=" << Config::bhopHumanity << '\n';
    file << "autoStrafeEnabled=" << (Config::autoStrafeEnabled ? 1 : 0) << '\n';
    file << "autoStrafeMode=" << Config::autoStrafeMode << '\n';
    file << "autoStrafeHumanity=" << Config::autoStrafeHumanity << '\n';
    file << "autoStrafeSwitchKey=" << Config::autoStrafeSwitchKey << '\n';
    file << "autoStrafeMaxTurn=" << Config::autoStrafeMaxTurn << '\n';
    file << "noFlashEnabled=" << (Config::noFlashEnabled ? 1 : 0) << '\n';
    file << "noSmokeEnabled=" << (Config::noSmokeEnabled ? 1 : 0) << '\n';
    file << "noRecoilEnabled=" << (Config::noRecoilEnabled ? 1 : 0) << '\n';
    file << "noSpreadEnabled=" << (Config::noSpreadEnabled ? 1 : 0) << '\n';
    file << "backtrackEnabled=" << (Config::backtrackEnabled ? 1 : 0) << '\n';
    file << "fovChangerEnabled=" << (Config::fovChangerEnabled ? 1 : 0) << '\n';
    file << "fovValue=" << Config::fovValue << '\n';
    file << "viewmodelFovEnabled=" << (Config::viewmodelFovEnabled ? 1 : 0) << '\n';
    file << "viewmodelFov=" << Config::viewmodelFov << '\n';
    file << "thirdpersonEnabled=" << (Config::thirdpersonEnabled ? 1 : 0) << '\n';
    file << "thirdpersonKey=" << Config::thirdpersonKey << '\n';
    file << "thirdpersonDistance=" << Config::thirdpersonDistance << '\n';
    file << "watermarkEnabled=" << (Config::watermarkEnabled ? 1 : 0) << '\n';
    file << "clanChangerEnabled=" << (Config::clanChangerEnabled ? 1 : 0) << '\n';
    file << "spectatorListEnabled=" << (Config::spectatorListEnabled ? 1 : 0) << '\n';
    file << "revealRanksEnabled=" << (Config::revealRanksEnabled ? 1 : 0) << '\n';
    file << "autoAcceptEnabled=" << (Config::autoAcceptEnabled ? 1 : 0) << '\n';
    file << "autoDefuseEnabled=" << (Config::autoDefuseEnabled ? 1 : 0) << '\n';
    file << "antiAimEnabled=" << (Config::antiAimEnabled ? 1 : 0) << '\n';
    file << "ragebotEnabled=" << (Config::ragebotEnabled ? 1 : 0) << '\n';
    file << "rageAutoFire=" << (Config::rageAutoFire ? 1 : 0) << '\n';
    file << "rageAutoWall=" << (Config::rageAutoWall ? 1 : 0) << '\n';
    file << "rageMinDamage=" << Config::rageMinDamage << '\n';
    file << "rageHitchance=" << Config::rageHitchance << '\n';
    file << "rageTargetSelection=" << Config::rageTargetSelection << '\n';
    file << "rageBone=" << Config::rageBone << '\n';
    file << "rageAutoPeek=" << (Config::rageAutoPeek ? 1 : 0) << '\n';
    file << "resolverEnabled=" << (Config::resolverEnabled ? 1 : 0) << '\n';
    file << "backtrackTicks=" << Config::backtrackTicks << '\n';
    file << "antiAimPitch=" << Config::antiAimPitch << '\n';
    file << "antiAimYaw=" << Config::antiAimYaw << '\n';
    file << "antiAimSpinSpeed=" << Config::antiAimSpinSpeed << '\n';
    file << "antiAimYawOffset=" << Config::antiAimYawOffset << '\n';
    file << "antiAimJitter=" << (Config::antiAimJitter ? 1 : 0) << '\n';
    file << "antiAimLBY=" << (Config::antiAimLBY ? 1 : 0) << '\n';
    file << "antiAimEdge=" << (Config::antiAimEdge ? 1 : 0) << '\n';
    file << "antiAimDesyncAngle=" << Config::antiAimDesyncAngle << '\n';
    file << "antiAimFreestanding=" << (Config::antiAimFreestanding ? 1 : 0) << '\n';
    WriteColor(file, "menuAccent", Config::menuAccent);
    file << "menuBlur=" << (Config::menuBlur ? 1 : 0) << '\n';
    file << "clanTag" << '=';
    for (int i = 0; i < 64 && Config::clanTag[i]; ++i)
        file << Config::clanTag[i];
    file << '\n';

    file.flush();
    if (!file.good()) {
        if (error) *error = "Could not write profile";
        return false;
    }
    return true;
}

bool LoadProfile(const std::string& name, std::string* error) {
    std::ifstream file(ProfilePath(name).wstring());
    if (!file.is_open()) {
        if (error) *error = "Profile not found";
        return false;
    }

    Config::ResetDefaults();

    std::string line;
    int configVersion = 0;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        std::string key = Trim(line.substr(0, separator));
        std::string value = Trim(line.substr(separator + 1));

        // N-L-12: track config version; migrate or reject profiles from
        // unknown versions. Current version is 1.
        if (key == "version") {
            configVersion = ToInt(value, 0);
            if (configVersion > 1) {
                if (error) *error = "Config version mismatch — profile from a newer build";
                return false;
            }
            if (configVersion < 1) {
                // v0 → v1 migration: defaults were already applied by
                // ResetDefaults(), so missing fields stay at their defaults.
                cs2::FileLog("[CONFIG] Migrating profile from v%d to v1", configVersion);
            }
            continue;
        }

        if (key == "aimbotEnabled") Config::aimbotEnabled = ToBool(value);
        else if (key == "aimKey") Config::aimKey = ToInt(value, VK_LBUTTON);
        else if (key == "aimKeyMode") Config::aimKeyMode = ToInt(value, 0);
        else if (key == "aimFov") Config::aimFov = ToFloat(value, 2.5f);
        else if (key == "aimSmooth") Config::aimSmooth = ToFloat(value, 1.0f);
        else if (key == "aimSilent") Config::aimSilent = ToBool(value, true);
        else if (key == "aimAutoWall") Config::aimAutoWall = ToBool(value);
        else if (key == "aimVisibleCheck") Config::aimVisibleCheck = ToBool(value, false);
        else if (key == "aimTeamCheck") Config::aimTeamCheck = ToBool(value, true);
        else if (key == "aimBone") Config::aimBone = ToInt(value, 0);
        else if (key == "aimSilentAutoFire") Config::aimSilentAutoFire = ToBool(value);
        else if (key == "aimTargetSelection") Config::aimTargetSelection = ToInt(value, 0);
        else if (key == "aimRcs") Config::aimRcs = ToBool(value);
        else if (key == "aimRcsX") Config::aimRcsX = ToFloat(value, 1.0f);
        else if (key == "aimRcsY") Config::aimRcsY = ToFloat(value, 1.0f);
        else if (key == "triggerEnabled") Config::triggerEnabled = ToBool(value);
        else if (key == "triggerKey") Config::triggerKey = ToInt(value, VK_LBUTTON);
        else if (key == "triggerKeyMode") Config::triggerKeyMode = ToInt(value, 0);
        else if (key == "triggerDelay") Config::triggerDelay = ToFloat(value, 15.0f);
        else if (key == "triggerVisibleCheck") Config::triggerVisibleCheck = ToBool(value, false);
        else if (key == "triggerTeamCheck") Config::triggerTeamCheck = ToBool(value, true);
        else if (key == "triggerHeadOnly") Config::triggerHeadOnly = ToBool(value);
        else if (key == "triggerRcs") Config::triggerRcs = ToBool(value, true);
        else if (key == "triggerRcsX") Config::triggerRcsX = ToFloat(value, 1.0f);
        else if (key == "triggerRcsY") Config::triggerRcsY = ToFloat(value, 1.0f);
        else if (key == "triggerMinDelay") Config::triggerMinDelay = ToFloat(value, 10.0f);
        else if (key == "triggerMaxDelay") Config::triggerMaxDelay = ToFloat(value, 60.0f);
        else if (key == "triggerBurstMode") Config::triggerBurstMode = ToBool(value);
        else if (key == "triggerBurstCount") Config::triggerBurstCount = ToInt(value, 3);
        else if (key == "espEnabled") Config::espEnabled = ToBool(value);
        else if (key == "espBoxes") Config::espBoxes = ToBool(value, true);
        else if (key == "espNames") Config::espNames = ToBool(value, true);
        else if (key == "espHealth") Config::espHealth = ToBool(value, true);
        else if (key == "espDistance") Config::espDistance = ToBool(value);
        else if (key == "espSkeleton") Config::espSkeleton = ToBool(value);
        else if (key == "espSnaplines") Config::espSnaplines = ToBool(value);
        else if (key == "espGlow") Config::espGlow = ToBool(value);
        else if (key == "espHitboxes") Config::espHitboxes = ToBool(value);
        else if (key == "espMaxDist") Config::espMaxDist = ToFloat(value, 5000.0f);
        else if (key == "radarEnabled") Config::radarEnabled = ToBool(value);
        else if (key == "radarSize") Config::radarSize = ToFloat(value, 180.0f);
        else if (key == "espEnemyColor") ParseColor(value, Config::espEnemyColor);
        else if (key == "espOccludedColor") ParseColor(value, Config::espOccludedColor);
        else if (key == "espTeamColor") ParseColor(value, Config::espTeamColor);
        else if (key == "espOccludedTeamColor") ParseColor(value, Config::espOccludedTeamColor);
         else if (key == "espSkeletonColor") ParseColor(value, Config::espSkeletonColor);
         else if (key == "glowEnabled") Config::glowEnabled = ToBool(value);
         else if (key == "glowTeamCheck") Config::glowTeamCheck = ToBool(value, true);
         else if (key == "glowOccluded") Config::glowOccluded = ToBool(value);
         else if (key == "glowRange") Config::glowRange = ToFloat(value, 2000.0f);
         else if (key == "glowEnemyColor") ParseColor(value, Config::glowEnemyColor);
         else if (key == "glowTeamColor") ParseColor(value, Config::glowTeamColor);
         else if (key == "glowOccludedEnemyColor") ParseColor(value, Config::glowOccludedEnemyColor);
         else if (key == "glowOccludedTeamColor") ParseColor(value, Config::glowOccludedTeamColor);
         else if (key == "bulletTracersEnabled") Config::bulletTracersEnabled = ToBool(value);
         else if (key == "TracerDuration") Config::bulletTracerDuration = ToFloat(value, 2.0f);
         else if (key == "bulletTracerColor") ParseColor(value, Config::bulletTracerColor);
         else if (key == "chamsEnabled") Config::chamsEnabled = ToBool(value);
        else if (key == "chamsEnemyVisible") Config::chamsEnemyVisible = ToBool(value);
        else if (key == "chamsEnemyOccluded") Config::chamsEnemyOccluded = ToBool(value);
        else if (key == "chamsEnemyColor") ParseColor(value, Config::chamsEnemyColor);
        else if (key == "chamsEnemyOccludedColor") ParseColor(value, Config::chamsEnemyOccludedColor);
        else if (key == "chamsTeamVisible") Config::chamsTeamVisible = ToBool(value);
        else if (key == "chamsTeamOccluded") Config::chamsTeamOccluded = ToBool(value);
        else if (key == "chamsTeamColor") ParseColor(value, Config::chamsTeamColor);
        else if (key == "chamsTeamOccludedColor") ParseColor(value, Config::chamsTeamOccludedColor);
        else if (key == "chamsBrightness") Config::chamsBrightness = ToFloat(value, 75.0f);
        else if (key == "armorBar") Config::armorBar = ToBool(value);
        else if (key == "crosshairEnabled") Config::crosshairEnabled = ToBool(value);
        else if (key == "glowIntensity") Config::glowIntensity = ToFloat(value, 40.0f);
         else if (key == "bhopEnabled") Config::bhopEnabled = ToBool(value);
         else if (key == "bhopKey") Config::bhopKey = ToInt(value, VK_SPACE);
         else if (key == "bhopHumanity") Config::bhopHumanity = ToFloat(value);
         else if (key == "autoStrafeEnabled") Config::autoStrafeEnabled = ToBool(value);
         else if (key == "autoStrafeMode") Config::autoStrafeMode = ToInt(value, 0);
         else if (key == "autoStrafeHumanity") Config::autoStrafeHumanity = ToFloat(value);
         else if (key == "autoStrafeSwitchKey") Config::autoStrafeSwitchKey = ToInt(value, VK_XBUTTON2);
         else if (key == "autoStrafeMaxTurn") Config::autoStrafeMaxTurn = ToFloat(value, 140.0f);
         else if (key == "noFlashEnabled") Config::noFlashEnabled = ToBool(value);
         else if (key == "noSmokeEnabled") Config::noSmokeEnabled = ToBool(value);
         else if (key == "noRecoilEnabled") Config::noRecoilEnabled = ToBool(value);
          else if (key == "noSpreadEnabled") Config::noSpreadEnabled = ToBool(value);
          else if (key == "backtrackEnabled") Config::backtrackEnabled = ToBool(value);
         else if (key == "fovChangerEnabled") Config::fovChangerEnabled = ToBool(value);
         else if (key == "fovValue") Config::fovValue = ToFloat(value, 90.0f);
         else if (key == "viewmodelFovEnabled") Config::viewmodelFovEnabled = ToBool(value);
         else if (key == "viewmodelFov") Config::viewmodelFov = ToFloat(value, 68.0f);
         else if (key == "thirdpersonEnabled") Config::thirdpersonEnabled = ToBool(value);
         else if (key == "thirdpersonKey") Config::thirdpersonKey = ToInt(value, 'V');
         else if (key == "thirdpersonDistance") Config::thirdpersonDistance = ToFloat(value, 150.0f);
         else if (key == "watermarkEnabled") Config::watermarkEnabled = ToBool(value, true);
         else if (key == "clanChangerEnabled") Config::clanChangerEnabled = ToBool(value);
         else if (key == "spectatorListEnabled") Config::spectatorListEnabled = ToBool(value);
         else if (key == "revealRanksEnabled") Config::revealRanksEnabled = ToBool(value);
         else if (key == "autoAcceptEnabled") Config::autoAcceptEnabled = ToBool(value);
         else if (key == "autoDefuseEnabled") Config::autoDefuseEnabled = ToBool(value);
         else if (key == "antiAimEnabled") Config::antiAimEnabled = ToBool(value);
         else if (key == "ragebotEnabled") Config::ragebotEnabled = ToBool(value);
         else if (key == "rageAutoFire") Config::rageAutoFire = ToBool(value);
         else if (key == "rageAutoWall") Config::rageAutoWall = ToBool(value);
         else if (key == "rageMinDamage") Config::rageMinDamage = ToFloat(value, 20.0f);
         else if (key == "rageHitchance") Config::rageHitchance = ToFloat(value, 65.0f);
         else if (key == "rageTargetSelection") Config::rageTargetSelection = ToInt(value, 0);
         else if (key == "rageBone") Config::rageBone = ToInt(value, 0);
         else if (key == "rageAutoPeek") Config::rageAutoPeek = ToBool(value);
         else if (key == "resolverEnabled") Config::resolverEnabled = ToBool(value);
         else if (key == "backtrackTicks") Config::backtrackTicks = ToInt(value, 6);
         else if (key == "antiAimPitch") Config::antiAimPitch = ToInt(value, 0);
         else if (key == "antiAimYaw") Config::antiAimYaw = ToInt(value, 0);
         else if (key == "antiAimSpinSpeed") Config::antiAimSpinSpeed = ToFloat(value, 180.0f);
         else if (key == "antiAimYawOffset") Config::antiAimYawOffset = ToFloat(value);
         else if (key == "antiAimJitter") Config::antiAimJitter = ToBool(value);
         else if (key == "antiAimLBY") Config::antiAimLBY = ToBool(value);
         else if (key == "antiAimEdge") Config::antiAimEdge = ToBool(value);
         else if (key == "antiAimDesyncAngle") Config::antiAimDesyncAngle = ToFloat(value, 60.0f);
         else if (key == "antiAimFreestanding") Config::antiAimFreestanding = ToBool(value);
         else if (key == "menuAccent") ParseColor(value, Config::menuAccent);
         else if (key == "menuBlur") Config::menuBlur = ToBool(value, true);
        else if (key == "clanTag") {
            std::snprintf(Config::clanTag, sizeof(Config::clanTag), "%s", value.c_str());
        }
    }

    ClampSettings();
    return true;
}

bool DeleteProfile(const std::string& name, std::string* error) {
    std::error_code ec;
    bool removed = std::filesystem::remove(ProfilePath(name), ec);
    if (!removed) {
        if (error) *error = ec ? "Could not delete profile" : "Profile not found";
        return false;
    }
    return true;
}

} // namespace ConfigIO

namespace Config {

void ResetDefaults() {
    aimbotEnabled = true;
    aimKey = VK_LBUTTON;
    aimKeyMode = 0;
    aimFov = 2.5f;
    aimSmooth = 1.0f;
    aimSilent = true;
    aimAutoWall = false;
    aimVisibleCheck = false;
    aimTeamCheck = true;
    aimBone = 0;
    aimTargetSelection = 0;
    aimSilentAutoFire = false;
    aimRcs = false;
    aimRcsX = 1.0f;
    aimRcsY = 1.0f;

    triggerEnabled = true;
    triggerKey = VK_LBUTTON;
    triggerKeyMode = 0;
    triggerDelay = 15.0f;
    triggerMinDelay = 10.0f;
    triggerMaxDelay = 60.0f;
    triggerBurstMode = false;
    triggerBurstCount = 3;
    triggerVisibleCheck = false;
    triggerTeamCheck = true;
    triggerHeadOnly = false;
    triggerRcs = true;
    triggerRcsX = 1.0f;
    triggerRcsY = 1.0f;

    espEnabled = true;
    espBoxes = true;
    espNames = true;
    espHealth = true;
    espDistance = false;
    espSkeleton = true;
    espSnaplines = false;
    espGlow = true;
    espMaxDist = 5000.0f;
    radarEnabled = true;
    radarSize = 180.0f;
    espEnemyColor[0] = 0.93f; espEnemyColor[1] = 0.26f; espEnemyColor[2] = 0.36f; espEnemyColor[3] = 1.00f;
    espOccludedColor[0] = 0.50f; espOccludedColor[1] = 0.50f; espOccludedColor[2] = 0.50f; espOccludedColor[3] = 1.00f;
    espTeamColor[0] = 0.13f; espTeamColor[1] = 0.79f; espTeamColor[2] = 0.59f; espTeamColor[3] = 1.00f;
    espOccludedTeamColor[0] = 0.20f; espOccludedTeamColor[1] = 0.20f; espOccludedTeamColor[2] = 0.20f; espOccludedTeamColor[3] = 1.00f;
    espSkeletonColor[0] = 0.91f; espSkeletonColor[1] = 0.92f; espSkeletonColor[2] = 0.93f; espSkeletonColor[3] = 1.0f;

    glowEnabled = true;
    glowTeamCheck = true;
    glowOccluded = false;
    glowRange = 2000.0f;
    glowEnemyColor[0] = 0.93f; glowEnemyColor[1] = 0.26f; glowEnemyColor[2] = 0.36f; glowEnemyColor[3] = 0.65f;
    glowTeamColor[0] = 0.13f; glowTeamColor[1] = 0.79f; glowTeamColor[2] = 0.59f; glowTeamColor[3] = 0.65f;
    glowOccludedEnemyColor[0] = 0.93f; glowOccludedEnemyColor[1] = 0.26f; glowOccludedEnemyColor[2] = 0.36f; glowOccludedEnemyColor[3] = 0.40f;
    glowOccludedTeamColor[0] = 0.13f; glowOccludedTeamColor[1] = 0.79f; glowOccludedTeamColor[2] = 0.59f; glowOccludedTeamColor[3] = 0.40f;

    bulletTracersEnabled = true;
    bulletTracerDuration = 2.0f;
    bulletTracerColor[0] = 0.0f; bulletTracerColor[1] = 1.0f; bulletTracerColor[2] = 0.45f; bulletTracerColor[3] = 0.85f;

    chamsEnabled = true;
    chamsEnemyVisible = true;
    chamsEnemyOccluded = true;
    chamsEnemyColor[0] = 0.93f; chamsEnemyColor[1] = 0.26f; chamsEnemyColor[2] = 0.36f; chamsEnemyColor[3] = 1.00f;
    chamsEnemyOccludedColor[0] = 0.93f; chamsEnemyOccludedColor[1] = 0.26f; chamsEnemyOccludedColor[2] = 0.36f; chamsEnemyOccludedColor[3] = 0.45f;
    chamsTeamVisible = false;
    chamsTeamOccluded = false;
    chamsTeamColor[0] = 0.13f; chamsTeamColor[1] = 0.79f; chamsTeamColor[2] = 0.59f; chamsTeamColor[3] = 1.00f;
    chamsTeamOccludedColor[0] = 0.13f; chamsTeamOccludedColor[1] = 0.79f; chamsTeamOccludedColor[2] = 0.59f; chamsTeamOccludedColor[3] = 0.45f;
    chamsBrightness = 75.0f;
    armorBar = false;
    crosshairEnabled = true;
    glowIntensity = 40.0f;

    bhopEnabled = true;
    bhopKey = VK_SPACE;
    bhopHumanity = 0.0f;
    autoStrafeEnabled = true;
    autoStrafeMode = 0;
    autoStrafeHumanity = 0.0f;
    autoStrafeSwitchKey = VK_XBUTTON2;
    autoStrafeMaxTurn = 140.0f;
    noFlashEnabled = true;
    noSmokeEnabled = true;
    noRecoilEnabled = true;
    noSpreadEnabled = true;
    backtrackEnabled = false;
    fovChangerEnabled = true;
    fovValue = 110.0f;
    viewmodelFovEnabled = true;
    viewmodelFov = 90.0f;
    thirdpersonEnabled = false;
    thirdpersonKey = 'V';
    thirdpersonDistance = 150.0f;
    watermarkEnabled = true;
    clanChangerEnabled = false;
    spectatorListEnabled = false;
    revealRanksEnabled = false;
    autoAcceptEnabled = false;
    autoDefuseEnabled = false;
    antiAimEnabled = false;
    ragebotEnabled = false;
    rageAutoFire = false;
    rageAutoWall = false;
    rageMinDamage = 20.0f;
    rageHitchance = 65.0f;
    rageTargetSelection = 0;
    rageBone = 0;
    rageAutoPeek = false;
    resolverEnabled = false;
    backtrackTicks = 6;
    antiAimPitch = 0;
    antiAimYaw = 0;
    antiAimSpinSpeed = 180.0f;
    antiAimYawOffset = 0.0f;
    antiAimJitter = false;
    antiAimLBY = false;
    antiAimEdge = false;
    antiAimDesyncAngle = 60.0f;
    antiAimFreestanding = false;
    menuAccent[0] = 0.0f; menuAccent[1] = 0.88f; menuAccent[2] = 0.95f; menuAccent[3] = 1.0f;
    menuBlur = true;
    std::memset(clanTag, 0, sizeof(clanTag));
    std::memcpy(clanTag, "ASTRAL", 7); // N-L-11: reset clanTag on defaults
}

} // namespace Config

namespace ConfigIO {

void AutoLoadDefault() {
    std::error_code ec;
    auto dir = ConfigDirectory();
    if (std::filesystem::exists(dir / "default.cfg", ec)) {
        std::string error;
        if (LoadProfile("default", &error)) {
            cs2::FileLog("[+] Auto-loaded default config profile");
        }
    }
}

} // namespace ConfigIO
