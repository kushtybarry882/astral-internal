// AUDIT: P2-10, N-C-2, N-C-3, N-C-4, N-C-5, N-M-1, PERF-1
#pragma once

#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <vector>

namespace memory {

inline bool IsValidPtr(uintptr_t address) {
    return address > 0x10000 && address < 0x00007FFFFFFFFFFFULL;
}

// PERF-1: fast path — SEH-guarded memcpy only. No VirtualQuery, no
// VirtualProtect per access. For in-process DLL writes to game memory this is
// orders of magnitude faster than syscall-heavy validation. If the page is
// protected, SEH catches the access violation and we return false.
template <typename T>
inline bool Read(uintptr_t address, T& out) {
    if (!IsValidPtr(address)) {
        return false;
    }
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

template <typename T>
inline bool Write(uintptr_t address, const T& value) {
    if (!IsValidPtr(address)) {
        return false;
    }
    __try {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// PERF-1: batched write — VirtualProtect once per page region, write many
// values, restore once. Use this when writing multiple fields on the same
// page (e.g. glow object, aim punch, etc.).
struct WriteBatch {
    uintptr_t baseAddr = 0;
    size_t baseSize = 0;
    DWORD oldProtect = 0;
    bool active = false;

    inline bool Begin(uintptr_t addr, size_t size) {
        if (!IsValidPtr(addr) || size == 0) return false;
        baseAddr = addr;
        baseSize = size;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_READWRITE, &oldProtect)) {
            return false;
        }
        active = true;
        return true;
    }

    template <typename T>
    inline bool Write(uintptr_t addr, const T& value) {
        if (!active) return false;
        __try {
            std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    inline bool End() {
        if (!active) return false;
        DWORD dummy = 0;
        // Restore the FULL range that was opened, not a single byte.
        VirtualProtect(reinterpret_cast<void*>(baseAddr), baseSize, oldProtect, &dummy);
        active = false;
        return true;
    }
};

// Full-range validation (VirtualQuery based) — use sparingly, only when you
// need to confirm a whole buffer is accessible before touching it.
inline bool IsRangeReadable(uintptr_t address, size_t size) {
    if (!IsValidPtr(address) || size == 0) return false;
    uintptr_t current = address;
    uintptr_t end = address + size;
    if (end < address) return false;
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd;
    }
    return true;
}

inline bool IsRangeWritable(uintptr_t address, size_t size) {
    if (!IsValidPtr(address) || size == 0) return false;
    uintptr_t current = address;
    uintptr_t end = address + size;
    if (end < address) return false;
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        DWORD protect = mbi.Protect & 0xFF;
        if (protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
            protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd;
    }
    return true;
}

inline bool WriteBytes(uintptr_t address, const void* data, size_t size) {
    if (!data || !IsValidPtr(address) || size == 0) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &dummy);
    return true;
}

inline bool WriteString(uintptr_t address, const char* str) {
    if (!str) return false;
    size_t len = std::strlen(str) + 1;
    return WriteBytes(address, str, len);
}

inline bool ReadString(uintptr_t address, char* out, size_t capacity) {
    if (!out || capacity == 0 || !IsValidPtr(address)) {
        return false;
    }
    out[0] = '\0';
    if (!IsRangeReadable(address, capacity)) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out[capacity - 1] = '\0';
    for (size_t i = 0; i < capacity - 1; ++i) {
        if (out[i] == '\0') return i > 0;
        unsigned char uc = static_cast<unsigned char>(out[i]);
        if (uc < 0x20 || uc == 0x7F) {
            out[i] = '\0';
            return false;
        }
    }
    return true;
}

inline uintptr_t ResolveRipRelative(uintptr_t instruction, size_t displacementOffset, size_t instructionSize) {
    int32_t displacement = 0;
    if (!Read(instruction + displacementOffset, displacement)) return 0;
    return instruction + instructionSize + static_cast<intptr_t>(displacement);
}

// PERF-1: FindPattern — 4096 byte pattern limit, optional all-section scan.
inline uintptr_t FindPattern(HMODULE module, const char* signature, bool scanAllSections = false) {
    if (!module || !signature || !signature[0]) return 0;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    constexpr size_t kMaxPattern = 4096;
    uint8_t bytes[kMaxPattern] = {};
    uint8_t mask[kMaxPattern] = {};
    size_t count = 0;
    const char* cursor = signature;
    while (*cursor && count < kMaxPattern) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        if (*cursor == '?') {
            bytes[count] = 0; mask[count] = 0; ++count; ++cursor;
            if (*cursor == '?') ++cursor;
            continue;
        }
        auto hexValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int high = hexValue(cursor[0]);
        int low = hexValue(cursor[1]);
        if (high < 0 || low < 0) return 0;
        bytes[count] = static_cast<uint8_t>((high << 4) | low);
        mask[count] = 0xFF;
        ++count;
        cursor += 2;
    }
    if (count == 0) return 0;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!scanAllSections && (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        auto* start = base + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        if (size == 0) size = section->SizeOfRawData;
        if (size < count) continue;
        for (size_t offset = 0; offset <= size - count; ++offset) {
            bool found = true;
            for (size_t j = 0; j < count; ++j) {
                if (mask[j] != 0 && start[offset + j] != bytes[j]) { found = false; break; }
            }
            if (found) return reinterpret_cast<uintptr_t>(start + offset);
        }
    }
    return 0;
}

inline uintptr_t FindPatternExec(HMODULE module, const char* signature) {
    return FindPattern(module, signature, false);
}

} // namespace memory
