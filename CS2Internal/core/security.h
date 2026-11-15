#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace Security {

inline constexpr std::uint32_t fnv1a_const(const char* str, std::size_t len) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint32_t>(str[i]);
        hash *= 16777619u;
    }
    return hash;
}

std::uint32_t ComputeCRC32(const void* data, std::size_t size);
std::uint32_t GetTextSectionCRC();
std::uint32_t GetTextSectionCRC(HMODULE hModule);
void InitializeIntegrityCheck();
void InitializeIntegrityCheck(HMODULE hModule);
bool VerifyIntegrity();
void InitializeNTSClear();
void HideFromDebugger();
void ApplyZeroRWX(HMODULE hModule);
void CreateHiddenThread(void* (*fn)(void*), void* param);

} // namespace Security