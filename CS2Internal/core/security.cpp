#include "security.h"

#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "log.h"

namespace Security {

static std::uint32_t g_originalCRC = 0;

std::uint32_t ComputeCRC32(const void* data, std::size_t size) {
    if (!data || size == 0) return 0;

    static bool tableInit = false;
    static std::uint32_t crcTable[256];

    if (!tableInit) {
        for (std::size_t i = 0; i < 256; ++i) {
            std::uint32_t crc = static_cast<std::uint32_t>(i);
            for (std::size_t j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320u;
                } else {
                    crc >>= 1;
                }
            }
            crcTable[i] = crc;
        }
        tableInit = true;
    }

    std::uint32_t crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ crcTable[(crc ^ bytes[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t GetTextSectionCRC(HMODULE hModule) {
    if (!hModule) return 0;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hModule);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(hModule) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".text", 5) == 0) {
            void* sectionStart = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(hModule) + sections[i].VirtualAddress);
            return ComputeCRC32(sectionStart, sections[i].Misc.VirtualSize);
        }
    }
    return 0;
}

// C-6: measure the DLL itself, never the host EXE. The no-arg overload is
// removed — callers must pass the module they want to protect.
std::uint32_t GetTextSectionCRC() {
    return 0; // disabled: measuring the host EXE is meaningless for DLL integrity
}

void InitializeIntegrityCheck() {
    // Disabled: no-arg overload measured the host EXE, which is meaningless
    // for DLL integrity. Use InitializeIntegrityCheck(HMODULE) instead.
}

void InitializeIntegrityCheck(HMODULE hModule) {
    if (!hModule) return;
    g_originalCRC = GetTextSectionCRC(hModule);
}

bool VerifyIntegrity() {
    // Fail closed: if the original CRC was never captured, integrity is unknown.
    if (g_originalCRC == 0) {
        return false;
    }
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(static_cast<std::uint32_t(*)(void)>(&GetTextSectionCRC)), &self);
    if (!self) return false;
    std::uint32_t currentCRC = GetTextSectionCRC(self);
    return currentCRC == g_originalCRC;
}

void InitializeNTSClear() {
    __try {
        // Clear BeingDebugged in PEB
        auto teb = reinterpret_cast<PTEB>(NtCurrentTeb());
        if (teb && teb->ProcessEnvironmentBlock) {
            teb->ProcessEnvironmentBlock->BeingDebugged = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void HideFromDebugger() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return;

    using NtSetInformationThread_t = NTSTATUS(NTAPI*)(
        HANDLE, THREADINFOCLASS, PVOID, ULONG);

    auto fn = reinterpret_cast<NtSetInformationThread_t>(
        GetProcAddress(hNtdll, "NtSetInformationThread"));
    
    if (fn) {
        // ThreadHideFromDebugger = 0x11 — the only flag that matters for thread hiding.
        fn(GetCurrentThread(), static_cast<THREADINFOCLASS>(0x11), nullptr, 0);
    }
}

void ApplyZeroRWX(HMODULE hModule) {
    if (!hModule) return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hModule);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(hModule) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto* sections = IMAGE_FIRST_SECTION(nt);
    DWORD oldProtect = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        void* sectionAddr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(hModule) + sections[i].VirtualAddress);
        SIZE_T sectionSize = sections[i].Misc.VirtualSize;

        if (sectionSize == 0) continue;

        if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".text", 5) == 0) {
            VirtualProtect(sectionAddr, sectionSize, PAGE_EXECUTE_READ, &oldProtect);
        }
        else if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".rdata", 6) == 0) {
            VirtualProtect(sectionAddr, sectionSize, PAGE_READONLY, &oldProtect);
        }
        else if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".data", 5) == 0) {
            VirtualProtect(sectionAddr, sectionSize, PAGE_READWRITE, &oldProtect);
        }
        else if (strncmp(reinterpret_cast<const char*>(sections[i].Name), ".rsrc", 5) == 0) {
            VirtualProtect(sectionAddr, sectionSize, PAGE_READONLY, &oldProtect);
        }
    }
}

void CreateHiddenThread(void* (*fn)(void*), void* param) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll || !fn) return;

    using NtCreateThreadEx_t = NTSTATUS(NTAPI*)(
        PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE,
        PVOID, PVOID, ULONG, ULONG_PTR, SIZE_T, SIZE_T, PVOID);
    using NtSetInformationThread_t = NTSTATUS(NTAPI*)(
        HANDLE, THREADINFOCLASS, PVOID, ULONG);

    auto ntCreateThread = reinterpret_cast<NtCreateThreadEx_t>(
        GetProcAddress(hNtdll, "NtCreateThreadEx"));
    auto ntSetInfoThread = reinterpret_cast<NtSetInformationThread_t>(
        GetProcAddress(hNtdll, "NtSetInformationThread"));
    
    if (ntCreateThread) {
        HANDLE hThread = nullptr;
        OBJECT_ATTRIBUTES attrs = {};
        attrs.Length = sizeof(attrs);
        
        NTSTATUS status = ntCreateThread(&hThread, THREAD_ALL_ACCESS, &attrs, GetCurrentProcess(),
                       fn, param, 0, 0, 0, 0, nullptr);
        
        if (status >= 0 && hThread) {
            // H-4: hide the new thread from the debugger immediately.
            if (ntSetInfoThread) {
                ntSetInfoThread(hThread, static_cast<THREADINFOCLASS>(0x11), nullptr, 0);
            }
            CloseHandle(hThread);
        } else {
            cs2::FileLog("CreateHiddenThread: NtCreateThreadEx failed status=0x%08X", status);
        }
    }
}

} // namespace Security