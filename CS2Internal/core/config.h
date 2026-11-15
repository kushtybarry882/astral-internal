// AUDIT: P2-1, P2-2, P2-9, P4-1, P4-2, P4-3, P4-4
// UPGRADE: M-3, M-4, M-5, C-1, C-5, S7-1, S7-2, S7-3
#pragma once

#include "sdk.h"

namespace Config {

constexpr int kWeaponProfileCount = 5;

// Aimbot
inline bool aimbotEnabled = true;
inline int aimKey = VK_LBUTTON;
inline int aimKeyMode = 0; // 0=hold, 1=toggle
inline float aimFov = 2.5f;
inline float aimSmooth = 1.0f;
inline bool aimSilent = true;
inline bool aimAutoWall = false;
inline bool aimVisibleCheck = false;
inline bool aimTeamCheck = true;
inline int aimBone = 0; // 0=head, 1=neck, 2=chest, 3=pelvis
inline int aimTargetSelection = 0; // 0=FOV, 1=Distance, 2=Health, 3=Crosshair
inline bool aimSilentAutoFire = false; // if true, silent aim fires WITHOUT the triggerbot
inline bool aimRcs = false;
inline float aimRcsX = 1.0f;
inline float aimRcsY = 1.0f;
inline float aimHitchance = 65.0f; // 1-100% reject below this percentage

// Triggerbot
inline bool triggerEnabled = true;
inline int triggerKey = VK_LBUTTON;
inline int triggerKeyMode = 0; // 0=hold, 1=toggle
inline float triggerDelay = 15.0f;
inline bool triggerVisibleCheck = false;
inline bool triggerTeamCheck = true;
inline bool triggerHeadOnly = false;
inline bool triggerRcs = true;
inline float triggerRcsX = 1.0f;
inline float triggerRcsY = 1.0f;
inline float triggerMinDelay = 10.0f; // Gaussian lower bound (ms)
inline float triggerMaxDelay = 60.0f; // Gaussian upper bound (ms)
inline bool triggerBurstMode = false;
inline int triggerBurstCount = 3;     // shots per burst

// Visuals
inline bool espEnabled = true;
inline bool espBoxes = true;
inline bool espNames = true;
inline bool espHealth = true;
inline bool espDistance = false;
inline bool espSkeleton = true;
inline bool espSnaplines = false;
inline bool espGlow = true;
inline bool espHitboxes = false;
inline float espMaxDist = 5000.0f;
inline bool radarEnabled = true;
inline float radarSize = 180.0f;

// Glow ESP
inline bool glowEnabled = true;
inline float glowEnemyColor[4] = {0.93f, 0.26f, 0.36f, 0.65f};
inline float glowTeamColor[4] = {0.13f, 0.79f, 0.59f, 0.65f};
inline float glowOccludedEnemyColor[4] = {0.93f, 0.26f, 0.36f, 0.40f};
inline float glowOccludedTeamColor[4] = {0.13f, 0.79f, 0.59f, 0.40f};
inline float glowRange = 2000.0f;
inline bool glowTeamCheck = true;
inline bool glowOccluded = false;

// Bullet Tracers
inline bool bulletTracersEnabled = true;
inline float bulletTracerColor[4] = {0.0f, 1.0f, 0.45f, 0.85f};
inline float bulletTracerDuration = 2.0f;

inline float espEnemyColor[4] = {0.93f, 0.26f, 0.36f, 1.00f};
inline float espOccludedColor[4] = {0.50f, 0.50f, 0.50f, 1.00f};
inline float espTeamColor[4] = {0.13f, 0.79f, 0.59f, 1.00f};
inline float espOccludedTeamColor[4] = {0.20f, 0.20f, 0.20f, 1.00f};
inline float espSkeletonColor[4] = {0.91f, 0.92f, 0.93f, 1.00f};

// Chams
inline bool chamsEnabled = true;
inline bool chamsEnemyVisible = true;
inline bool chamsEnemyOccluded = true;
inline float chamsEnemyColor[4] = {0.93f, 0.26f, 0.36f, 1.00f};
inline float chamsEnemyOccludedColor[4] = {0.93f, 0.26f, 0.36f, 0.45f};
inline bool chamsTeamVisible = false;
inline bool chamsTeamOccluded = false;
inline float chamsTeamColor[4] = {0.13f, 0.79f, 0.59f, 1.00f};
inline float chamsTeamOccludedColor[4] = {0.13f, 0.79f, 0.59f, 0.45f};
inline float chamsBrightness = 75.0f;

// Visuals extras
inline bool armorBar = false;
inline bool crosshairEnabled = true;
inline float glowIntensity = 40.0f;

// Misc
inline bool bhopEnabled = true;
inline int bhopKey = VK_SPACE;
inline float bhopHumanity = 0.0f;        // 0-100
inline bool autoStrafeEnabled = true;
inline int autoStrafeMode = 0;           // 0=Assist, 1=Auto
inline float autoStrafeHumanity = 0.0f;  // 0-100
inline int autoStrafeSwitchKey = VK_XBUTTON2;
inline float autoStrafeMaxTurn = 140.0f; // deg/sec clamp for Auto mode
inline bool noFlashEnabled = true;
inline bool noSmokeEnabled = true;
inline bool noRecoilEnabled = true;
inline bool noSpreadEnabled = true;
inline bool backtrackEnabled = false; // consume backtrackTicks in the ragebot
inline bool fovChangerEnabled = true;
inline float fovValue = 110.0f;
inline bool viewmodelFovEnabled = true;
inline float viewmodelFov = 90.0f;
inline bool thirdpersonEnabled = false;
inline int thirdpersonKey = 'V';        // 0 = always on when enabled
inline float thirdpersonDistance = 150.0f;
inline bool watermarkEnabled = true;
inline bool clanChangerEnabled = false;
inline bool spectatorListEnabled = false;
inline bool revealRanksEnabled = false;
inline bool autoAcceptEnabled = false;
inline bool autoDefuseEnabled = false;
inline bool antiAimEnabled = false;
inline bool ragebotEnabled = false;
inline bool rageAutoFire = false;
inline bool rageAutoWall = false;
inline float rageFov = 180.0f;       // wide rage FOV (legit aimFov is far too narrow)
inline float rageMinDamage = 20.0f;
inline float rageHitchance = 65.0f;
inline int rageTargetSelection = 0;
inline int rageBone = 0;
inline bool rageAutoPeek = false;
inline bool resolverEnabled = false;
inline int backtrackTicks = 6;
inline char clanTag[64] = "ASTRAL";

// Anti-Aim settings
inline int antiAimPitch = 0;      // 0=Down, 1=Fake, 2=Zero, 3=Custom
inline int antiAimYaw = 0;         // 0=Spin, 1=Jitter, 2=Static, 3=LBY
inline float antiAimSpinSpeed = 180.0f;
inline float antiAimYawOffset = 0.0f;
inline bool antiAimJitter = false;
inline bool antiAimLBY = false;
inline bool antiAimEdge = false;
inline float antiAimDesyncAngle = 60.0f;
inline bool antiAimFreestanding = false;

// Menu appearance
inline float menuAccent[4] = {0.0f, 0.88f, 0.95f, 1.0f};
inline bool menuBlur = true;

void ResetDefaults();

} // namespace Config
