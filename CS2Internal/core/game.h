// AUDIT: P1-2, P1-3, P2-5, P2-6
// UPGRADE: V-1, C-4, V-4
#pragma once

#include <cstddef>
#include <vector>
#include "sdk.h"

namespace Game {

void Refresh();

// One entry per alive player (excluding the local pawn). Rebuilt at most
// once per second. Returned by const reference so callers can iterate without
// paying for a vector copy every frame.
struct CachedPlayer {
    void* pawn = nullptr;
    void* controller = nullptr;
    int index = 0;
};
// Returns a snapshot BY VALUE: the backing cache is rebuilt under
// g_gameStateMutex, so handing out a reference would race with a concurrent
// rebuild on another thread (CreateMove vs the 10 Hz background thread).
std::vector<CachedPlayer> GetCachedPlayers();

void* GetEntity(int index);
int GetEntityIndex(void* entity);
void* GetActiveWeaponForPawn(void* pawn);
int GetWeaponClip1(void* pawn);
int GetWeaponDefIndex(void* pawn);
const char* GetWeaponName(int defIndex);
float GetFlashDuration(void* pawn);
bool IsPawnScoped(void* pawn);
void* GetControllerForPawn(void* pawn);
void* GetPlayerPawn(void* controller); // resolve C_CSPlayerPawn from a controller handle

bool GetEntityOrigin(void* entity, sdk::Vector& out);
bool GetBonePosition(void* entity, int boneIndex, sdk::Vector& out);
// Overload for callers that already read the entity origin this frame (ESP bone
// pass): skips the redundant origin read per bone.
bool GetBonePosition(void* entity, int boneIndex, const sdk::Vector& entityOrigin, sdk::Vector& out);
bool GetLocalEyePosition(sdk::Vector& out);
bool GetAimPunch(sdk::Vector& out);
bool IsVisible(void* entity);
bool IsAlive(void* entity);
bool IsValidPlayer(void* entity, bool teamCheck);
bool WorldToScreen(const sdk::Vector& world, sdk::Vector& screen);

std::uint8_t GetTeam(void* entity);
int GetHealth(void* entity);
int GetShotsFired();
int GetActiveWeaponDefIndex();
void* GetActiveWeapon();
bool CanFire();
int GetLocalRank();
bool GetControllerName(void* controller, char* out, std::size_t capacity);
bool GetPlayerName(void* pawn, char* out, std::size_t capacity);

sdk::GlobalVars* GetGlobalVars();
sdk::GlobalVars* GetGlobalVars(uintptr_t clientBase);

void* FindPlantBomb();
bool IsBombPlanted();
float GetBombDefuseTimeRemaining();
bool IsDefusing();
void* GetDefuserEntity();

struct GameState {
    uintptr_t clientBase;
    void* localPlayerPawn;
    void* localPlayerController;
    uintptr_t entityListBase;
    sdk::ViewMatrix viewMatrix;
};

GameState GetStateSnapshot();

int GetHighestEntityIndex();

} // namespace Game
