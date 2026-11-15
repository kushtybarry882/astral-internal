// UPGRADE: 0-import-wipe (import directory + strings zeroed after resolution),
//          0-debug-wipe (debug directory / CodeView PDB path zeroed).
//          KeyAuth flow intentionally untouched.
#include "external/keyauth/skStr.h"
#include "external/keyauth/auth.hpp"

#include "../core/evasion.h"

#include <winternl.h>
#include <tlhelp32.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>

// (MinHook not needed in loader)

#pragma comment(lib, "winmm.lib")

#define ASTRAL_TITLE "ASTRAL Loader"

// Target process
static const wchar_t* kTargetProcess = L"cs2.exe";
static const char* kTargetProcessA = "cs2.exe";

// Global running flag
static volatile bool g_running = true;

// Global debug file pointer for diagnostics
static FILE* g_dbgFile = nullptr;
static void DebugLog(const std::string& msg) {
    if (g_dbgFile) {
        fprintf(g_dbgFile, "%s\n", msg.c_str());
        fflush(g_dbgFile);
    }

}

// Helper: get current time string
static std::string GetTime() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &time);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm_info);
    return std::string(buffer);
}

// Helper: clear console
static void ClearConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD topLeft = {0, 0};
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    DWORD length = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD written;
    FillConsoleOutputCharacter(hOut, ' ', length, topLeft, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, length, topLeft, &written);
    SetConsoleCursorPosition(hOut, topLeft);
}

// Helper: find process by name
static DWORD FindProcessId(const wchar_t* processName) {
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

static bool IsProcessRunning(const wchar_t* processName) {
    return FindProcessId(processName) != 0;
}

// Helper: find main window for process
static HWND FindMainWindowForProcess(DWORD pid) {
    HWND result = nullptr;
    struct EnumData { DWORD pid; HWND hwnd; } data = {pid, nullptr};
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == d->pid && IsWindowVisible(hwnd)) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if ((rc.right - rc.left) * (rc.bottom - rc.top) > 10000) {
                d->hwnd = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

// Helper: get remote module base (case-insensitive, with name alias support)
static uintptr_t GetRemoteModuleBase(DWORD pid, const char* moduleName) {
    uintptr_t base = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(hSnap, &me)) {
        std::string searchName(moduleName);
        // Convert to lowercase for case-insensitive comparison
        for (auto& c : searchName) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        do {
            std::string modName(me.szModule);
            for (auto& c : modName) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (modName == searchName) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
            // Handle name aliases: d3dcompiler_47.dll might be loaded as d3dcompiler_47.dll
            // or the remote module might have a different full path
            if (searchName == "d3dcompiler_47.dll" || searchName == "d3dcompiler_47") {
                if (modName.find("d3dcompiler") != std::string::npos) {
                    base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                    break;
                }
            }
        } while (Module32Next(hSnap, &me));
    }
    CloseHandle(hSnap);
    return base;
}

// Load DLL file
static std::vector<uint8_t> LoadDLL(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file.good()) return {};
    return data;
}

// Zero a region of the remote process image. Chunked so any region works on
// any page size the kernel hands us.
static void ZeroRemoteRegion(HANDLE process, LPVOID remoteAddr, SIZE_T size) {
    if (!remoteAddr || size == 0) {
        return;
    }
    static const uint8_t zeros[4096] = {};
    SIZE_T offset = 0;
    while (offset < size) {
        SIZE_T chunk = std::min<SIZE_T>(sizeof(zeros), size - offset);
        evasion::EvasionWriteProcessMemory(process,
            reinterpret_cast<uint8_t*>(remoteAddr) + offset, zeros, chunk, nullptr);
        offset += chunk;
    }
}

// Manual map injection with hardened protections and evasion
static bool SafeManualMap(HANDLE hProcess, DWORD pid, const std::string& dllPath, const std::wstring& pipeNameW) {
    std::vector<uint8_t> dllData = LoadDLL(dllPath);
    if (dllData.empty()) {
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    std::cout << "  [+] SafeManualMap: Starting injection..." << std::endl;

    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(dllData.data());
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(dllData.data() + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    SIZE_T imageSize = ntHeaders->OptionalHeader.SizeOfImage;

    // Allocate memory in target process using evasion-wrapped VirtualAllocEx
    LPVOID remoteBase = evasion::EvasionVirtualAllocEx(hProcess, nullptr, imageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBase) {
        std::cout << "  [ ERROR ] : VirtualAllocEx failed." << std::endl;
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    // Write headers using evasion-wrapped WriteProcessMemory
    if (!evasion::EvasionWriteProcessMemory(hProcess, remoteBase, dllData.data(),
            ntHeaders->OptionalHeader.SizeOfHeaders, nullptr)) {
        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    // Write sections
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(ntHeaders);
    for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (section[i].SizeOfRawData == 0) continue;
        LPVOID dest = reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + section[i].VirtualAddress);
        if (!evasion::EvasionWriteProcessMemory(hProcess, dest,
                dllData.data() + section[i].PointerToRawData, section[i].SizeOfRawData, nullptr)) {
            evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            evasion::EvasionNtClose(hProcess);
            return false;
        }
    }

    // Helper: convert RVA to file offset. Returns 0 on failure; callers MUST check.
     auto RvaToFileOffset = [&dllData, &ntHeaders](DWORD rva) -> DWORD {
         if (rva == 0) return 0;
         PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(ntHeaders);
         for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
             DWORD virtSize = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
             if (rva >= sections[i].VirtualAddress && rva < sections[i].VirtualAddress + virtSize) {
                 return sections[i].PointerToRawData + (rva - sections[i].VirtualAddress);
             }
         }
         return 0; // invalid RVA — caller must abort
     };

     // Helper: validate that an RVA-derived file offset + size stays inside the DLL image.
     auto ValidateFileRange = [&dllData](DWORD fileOffset, DWORD size) -> bool {
         if (fileOffset == 0 || size == 0) return false;
         if (fileOffset > dllData.size()) return false;
         if (size > dllData.size() - fileOffset) return false;
         return true;
     };

// Process relocations — fail closed: any invalid RVA aborts mapping.
      DWORD relocRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
      if (relocRVA) {
          DWORD relocFileOffset = RvaToFileOffset(relocRVA);
          if (!ValidateFileRange(relocFileOffset, sizeof(IMAGE_BASE_RELOCATION))) {
              DebugLog("ERROR: relocation directory RVA invalid — aborting mapping");
              evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
              evasion::EvasionNtClose(hProcess);
              return false;
          }
          PIMAGE_BASE_RELOCATION reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(dllData.data() + relocFileOffset);
          uintptr_t delta = reinterpret_cast<uintptr_t>(remoteBase) - ntHeaders->OptionalHeader.ImageBase;
          uintptr_t relocEnd = reinterpret_cast<uintptr_t>(dllData.data()) + dllData.size();
          while (reloc->VirtualAddress && reinterpret_cast<uintptr_t>(reloc) < relocEnd) {
              if (reloc->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                  DebugLog("WARNING: relocation block SizeOfBlock underflow, skipping");
                  break;
              }
              if (reinterpret_cast<uintptr_t>(reloc) + reloc->SizeOfBlock > relocEnd) {
                  DebugLog("WARNING: relocation block exceeds file bounds, truncating");
                  break;
              }
              DWORD count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
              DWORD maxPossible = DWORD((dllData.size() - relocFileOffset) / sizeof(WORD));
              if (count > maxPossible) count = maxPossible;
              WORD* relocData = reinterpret_cast<WORD*>(reinterpret_cast<uintptr_t>(reloc) + sizeof(IMAGE_BASE_RELOCATION));
              for (DWORD j = 0; j < count; j++) {
                  if ((relocData[j] >> 12) == IMAGE_REL_BASED_DIR64) {
                      uintptr_t addr = reinterpret_cast<uintptr_t>(remoteBase) + reloc->VirtualAddress + (relocData[j] & 0xFFF);
                      uintptr_t value = 0;
                      SIZE_T read = 0;
                      if (evasion::EvasionReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &value, sizeof(value), &read) && read == sizeof(value)) {
                          value += delta;
                          evasion::EvasionWriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(addr), &value, sizeof(value), nullptr);
                      }
                  }
              }
              reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(reinterpret_cast<uintptr_t>(reloc) + reloc->SizeOfBlock);
          }
      }

    // Resolve imports — fail closed: every remote address must be derived from the
// remote module base. Never write a loader-local address into the remote IAT.
    DebugLog("Resolving imports...");
    DWORD importRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importRVA) {
        DWORD importFileOffset = RvaToFileOffset(importRVA);
        if (!ValidateFileRange(importFileOffset, ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)) {
            DebugLog("ERROR: import directory RVA invalid — aborting mapping");
            evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            evasion::EvasionNtClose(hProcess);
            return false;
        }
        PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(dllData.data() + importFileOffset);
        DWORD importDirEnd = importFileOffset + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
        DWORD descCount = 0;
        while (importDesc->Name && descCount < ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size / sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            DWORD nameFileOffset = RvaToFileOffset(importDesc->Name);
            if (!ValidateFileRange(nameFileOffset, MAX_PATH)) {
                DebugLog("ERROR: import DLL name RVA invalid — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            char* dllName = reinterpret_cast<char*>(dllData.data() + nameFileOffset);
            HMODULE hMod = LoadLibraryA(dllName);
            if (!hMod) {
                DebugLog("ERROR: LoadLibraryA failed for import DLL — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }

            // Get the remote base address for this DLL in cs2's process.
            // With ASLR, the DLL may be loaded at a different base address
            // in cs2.exe than in our process, so we must resolve addresses
            // relative to the remote base.
            uintptr_t remoteModBase = GetRemoteModuleBase(pid, dllName);
            uintptr_t localModBase = reinterpret_cast<uintptr_t>(hMod);
            char importLog[256];
            snprintf(importLog, sizeof(importLog), "Import DLL: %s localBase=%p remoteBase=%p",
                     dllName, (void*)localModBase, (void*)remoteModBase);
            DebugLog(importLog);

            if (remoteModBase == 0) {
                DebugLog("ERROR: remote module base not found for import DLL — aborting (refusing to write local addresses into remote IAT)");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }

            DWORD firstThunkFileOffset = RvaToFileOffset(importDesc->FirstThunk);
            if (!ValidateFileRange(firstThunkFileOffset, sizeof(uintptr_t))) {
                DebugLog("ERROR: import FirstThunk RVA invalid — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(dllData.data() + firstThunkFileOffset);
            uintptr_t iatBaseRemote = reinterpret_cast<uintptr_t>(remoteBase) + importDesc->FirstThunk;
            SIZE_T thunkOffset = 0;
            while (thunk->u1.AddressOfData) {
                FARPROC func = nullptr;
                if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                    func = GetProcAddress(hMod, reinterpret_cast<LPCSTR>(thunk->u1.Ordinal & 0xFFFF));
                } else {
                    DWORD ibnFileOffset = RvaToFileOffset(thunk->u1.AddressOfData);
                    if (!ValidateFileRange(ibnFileOffset, 2)) {
                        DebugLog("ERROR: import-by-name RVA invalid — aborting mapping");
                        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                        evasion::EvasionNtClose(hProcess);
                        return false;
                    }
                    PIMAGE_IMPORT_BY_NAME import = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(dllData.data() + ibnFileOffset);
                    func = GetProcAddress(hMod, import->Name);
                }
                if (func) {
                    uintptr_t funcRVA = reinterpret_cast<uintptr_t>(func) - localModBase;
                    uintptr_t funcAddr = remoteModBase + funcRVA;
                    evasion::EvasionWriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(iatBaseRemote + thunkOffset),
                        &funcAddr, sizeof(funcAddr), nullptr);
                }
                thunk++;
                thunkOffset += sizeof(uintptr_t);
            }
            importDesc++;
            descCount++;
        }
    }

    // Wipe the import directory and its strings from the remote image. Every
    // IAT entry now holds an absolute address the DLL uses at runtime; the
    // directory and the name strings are never read again, so zeroing them
    // removes every "ntdll.dll" / "kernel32.dll" marker from the mapped image.
    // Sizes are clamped to remaining image bytes (C-4) and strings are
    // scanned with an explicit max length (H-2).
    {
        DWORD importRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        DWORD importSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
        if (importRVA && importSize) {
            // C-4: clamp importSize to remaining bytes of the image.
            DWORD importRvaEnd = importRVA + importSize;
            if (importRvaEnd < importRVA || importRvaEnd > ntHeaders->OptionalHeader.SizeOfImage) {
                DebugLog("ERROR: import directory extends beyond SizeOfImage — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            DWORD importFileOffset = RvaToFileOffset(importRVA);
            if (!ValidateFileRange(importFileOffset, importSize)) {
                DebugLog("ERROR: import directory file range invalid — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            PIMAGE_IMPORT_DESCRIPTOR desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(dllData.data() + importFileOffset);
            DWORD descMax = importSize / sizeof(IMAGE_IMPORT_DESCRIPTOR);
            for (DWORD d = 0; d < descMax && desc->Name; ++d, ++desc) {
                // 1) DLL name string — bounded scan against remaining image bytes.
                DWORD dllNameFileOffset = RvaToFileOffset(desc->Name);
                if (dllNameFileOffset) {
                    SIZE_T maxNameLen = dllData.size() - dllNameFileOffset;
                    if (maxNameLen > 0 && maxNameLen < SIZE_MAX) {
                        const char* name = reinterpret_cast<const char*>(dllData.data() + dllNameFileOffset);
                        size_t nameLen = strnlen(name, maxNameLen);
                        if (nameLen < maxNameLen) { // found NUL within bounds
                            ZeroRemoteRegion(hProcess,
                                reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + desc->Name),
                                nameLen + 1);
                        }
                    }
                }
                // 2) INT (OriginalFirstThunk) array — dead after resolution
                DWORD intRVA = desc->OriginalFirstThunk;
                if (intRVA) {
                    DWORD intFileOffset = RvaToFileOffset(intRVA);
                    if (ValidateFileRange(intFileOffset, 8)) {
                        DWORD count = 0;
                        DWORD maxCount = (DWORD)((dllData.size() - intFileOffset) / sizeof(uintptr_t));
                        PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(dllData.data() + intFileOffset);
                        while (count < maxCount && thunk->u1.AddressOfData) { ++count; ++thunk; }
                        ZeroRemoteRegion(hProcess,
                            reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + intRVA),
                            static_cast<SIZE_T>((count + 1) * sizeof(uintptr_t)));
                    }
                }
                // 3) hint/name strings — bounded scan.
                DWORD firstThunkFileOffset = RvaToFileOffset(desc->FirstThunk);
                if (firstThunkFileOffset) {
                    PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(dllData.data() + firstThunkFileOffset);
                    DWORD maxThunks = (DWORD)((dllData.size() - firstThunkFileOffset) / sizeof(uintptr_t));
                    for (DWORD t = 0; t < maxThunks && thunk->u1.AddressOfData; ++t, ++thunk) {
                        if (!(thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                            DWORD ibnFileOffset = RvaToFileOffset(thunk->u1.AddressOfData);
                            if (ibnFileOffset) {
                                SIZE_T maxNmLen = dllData.size() - (ibnFileOffset + 2);
                                if (maxNmLen > 0) {
                                    const char* nm = reinterpret_cast<const char*>(dllData.data() + ibnFileOffset + 2);
                                    size_t nmLen = strnlen(nm, maxNmLen);
                                    if (nmLen < maxNmLen) {
                                        ZeroRemoteRegion(hProcess,
                                            reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + thunk->u1.AddressOfData),
                                            nmLen + 3);  // hint(2) + name + nul
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // 4) the descriptor table itself
            ZeroRemoteRegion(hProcess,
                reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + importRVA), importSize);
        }
    }

    // Wipe the debug directory and its CodeView data (PDB path) from the remote
    // image. Nothing in the running DLL reads it. Sizes clamped to SizeOfImage.
    {
        DWORD debugRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
        DWORD debugSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
        if (debugRVA && debugSize) {
            // C-4: clamp debugSize to remaining bytes of the image.
            DWORD debugRvaEnd = debugRVA + debugSize;
            if (debugRvaEnd < debugRVA || debugRvaEnd > ntHeaders->OptionalHeader.SizeOfImage) {
                DebugLog("ERROR: debug directory extends beyond SizeOfImage — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            DWORD debugFileOffset = RvaToFileOffset(debugRVA);
            if (!ValidateFileRange(debugFileOffset, debugSize)) {
                DebugLog("ERROR: debug directory file range invalid — aborting mapping");
                evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                evasion::EvasionNtClose(hProcess);
                return false;
            }
            DWORD numEntries = debugSize / sizeof(IMAGE_DEBUG_DIRECTORY);
            const IMAGE_DEBUG_DIRECTORY* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(dllData.data() + debugFileOffset);
            for (DWORD i = 0; i < numEntries; ++i) {
                if (entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW &&
                    entries[i].AddressOfRawData && entries[i].SizeOfData) {
                    DWORD cvRvaEnd = entries[i].AddressOfRawData + entries[i].SizeOfData;
                    if (cvRvaEnd < entries[i].AddressOfRawData || cvRvaEnd > ntHeaders->OptionalHeader.SizeOfImage) {
                        continue; // skip out-of-bounds CodeView entry
                    }
                    DWORD cvFileOffset = RvaToFileOffset(entries[i].AddressOfRawData);
                    if (!ValidateFileRange(cvFileOffset, entries[i].SizeOfData)) {
                        continue;
                    }
                    ZeroRemoteRegion(hProcess,
                        reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + entries[i].AddressOfRawData),
                        entries[i].SizeOfData);
                }
            }
            ZeroRemoteRegion(hProcess,
                reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + debugRVA), debugSize);
        }
    }

     // Write the pipe name pointer to the exported g_pipeNamePtr global in the remote DLL.
     {
         DWORD exportRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
         DWORD exportFileOffset = RvaToFileOffset(exportRVA);
         PIMAGE_EXPORT_DIRECTORY exportDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
             dllData.data() + exportFileOffset);

         if (exportDir && exportDir->NumberOfNames > 0) {
             DWORD* nameTable = reinterpret_cast<DWORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfNames));
             DWORD* funcTable = reinterpret_cast<DWORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfFunctions));
             WORD* ordinalTable = reinterpret_cast<WORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfNameOrdinals));

             for (DWORD i = 0; i < exportDir->NumberOfNames; ++i) {
                 const char* exportName = reinterpret_cast<const char*>(dllData.data() + RvaToFileOffset(nameTable[i]));
                 if (std::string(exportName) == "g_pipeNamePtr") {
                     WORD ordinal = ordinalTable[i];
                     if (ordinal < exportDir->NumberOfFunctions) {
                         DWORD ptrRVA = funcTable[ordinal];
                         uintptr_t ptrRemoteAddr = reinterpret_cast<uintptr_t>(remoteBase) + ptrRVA;

                         SIZE_T pipeNameSize = (pipeNameW.size() + 1) * sizeof(wchar_t);
                         LPVOID pipeNameRemote = evasion::EvasionVirtualAllocEx(hProcess, nullptr, pipeNameSize,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                         if (pipeNameRemote) {
                             evasion::EvasionWriteProcessMemory(hProcess, pipeNameRemote,
                                 pipeNameW.c_str(), pipeNameSize, nullptr);
                             uintptr_t ptrValue = reinterpret_cast<uintptr_t>(pipeNameRemote);
                             evasion::EvasionWriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(ptrRemoteAddr),
                                 &ptrValue, sizeof(ptrValue), nullptr);
                         }
                     }
                     break;
                 }
            }
         }
     }

    // Apply section protections AFTER import resolution so IAT writes succeed.
    for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (section[i].SizeOfRawData == 0) continue;
        LPVOID dest = reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(remoteBase) + section[i].VirtualAddress);
        DWORD protect = PAGE_READONLY;
        bool isExec = (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool isWrite = (section[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        if (isExec && isWrite) {
            protect = PAGE_EXECUTE_READWRITE;
        } else if (isExec) {
            protect = PAGE_EXECUTE_READ;
        } else if (isWrite) {
            protect = PAGE_READWRITE;
        }
        DWORD oldProtect = 0;
        SIZE_T sizeToProtect = section[i].Misc.VirtualSize;
        if (sizeToProtect == 0) sizeToProtect = section[i].SizeOfRawData;
        evasion::EvasionVirtualProtectEx(hProcess, dest, sizeToProtect, protect, &oldProtect);
    }

    // Resolve and call DllMainCRTExport from the export table.
    // This trampoline calls _dllmainCRTStartup with DLL_PROCESS_ATTACH (1),
    // ensuring CRT is properly initialized before DllMain spawns threads
    // that use CRT functions (std::vector, etc.).
    PVOID dllMainAddr = reinterpret_cast<PVOID>(reinterpret_cast<uintptr_t>(remoteBase) + ntHeaders->OptionalHeader.AddressOfEntryPoint);

    DWORD exportRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (exportRVA) {
        DWORD exportFileOffset = RvaToFileOffset(exportRVA);
        PIMAGE_EXPORT_DIRECTORY exportDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(dllData.data() + exportFileOffset);
        if (exportDir && exportDir->NumberOfNames > 0) {
            DWORD* nameTable = reinterpret_cast<DWORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfNames));
            DWORD* funcTable = reinterpret_cast<DWORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfFunctions));
            WORD* ordinalTable = reinterpret_cast<WORD*>(dllData.data() + RvaToFileOffset(exportDir->AddressOfNameOrdinals));

            for (DWORD i = 0; i < exportDir->NumberOfNames; ++i) {
                const char* exportName = reinterpret_cast<const char*>(dllData.data() + RvaToFileOffset(nameTable[i]));
                if (std::string(exportName) == "DllMainCRTExport") {
                    WORD ordinal = ordinalTable[i];
                    if (ordinal < exportDir->NumberOfFunctions) {
                        DWORD funcRVA = funcTable[ordinal];
                        dllMainAddr = reinterpret_cast<PVOID>(reinterpret_cast<uintptr_t>(remoteBase) + funcRVA);
                    }
                    break;
                }
            }
        }
    }

    // Register .pdata with RtlAddFunctionTable for SEH/exception support
    // in manually-mapped DLL. Without this, exception handling code generated
    // by MSVC will crash when it tries to look up function tables.
    {
        DWORD pdataRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
        DWORD pdataSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;

        if (pdataRVA && pdataSize) {
            DWORD pdataFileOffset = RvaToFileOffset(pdataRVA);

            LPVOID pdataRemote = evasion::EvasionVirtualAllocEx(
                hProcess, nullptr, pdataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);

            if (pdataRemote) {
                evasion::EvasionWriteProcessMemory(hProcess, pdataRemote,
                    dllData.data() + pdataFileOffset, pdataSize, nullptr);

                HMODULE ntdll = GetModuleHandleA("ntdll.dll");
                FARPROC rtlFn = ntdll ? GetProcAddress(ntdll, "RtlAddFunctionTable") : nullptr;

                 if (rtlFn) {
                      DWORD numEntries = pdataSize / sizeof(RUNTIME_FUNCTION);

                      uintptr_t remoteNtdll = GetRemoteModuleBase(pid, "ntdll.dll");
                      HMODULE localNtdll = GetModuleHandleA("ntdll.dll");
                      uintptr_t rtlRva = localNtdll ?
                          (reinterpret_cast<uintptr_t>(rtlFn) - reinterpret_cast<uintptr_t>(localNtdll)) : 0;
                      uintptr_t remoteRtlFn = (remoteNtdll && rtlRva) ?
                          (remoteNtdll + rtlRva) : 0;

                      if (!remoteRtlFn) {
                          DebugLog("ERROR: could not resolve remote ntdll!RtlAddFunctionTable — aborting mapping (no SEH stub)");
                          evasion::EvasionVirtualFreeEx(hProcess, pdataRemote, 0, MEM_RELEASE);
                          evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                          evasion::EvasionNtClose(hProcess);
                          return false;
                      }

                      // Resolve remote ExitThread for clean thread termination
                      HMODULE localKernel32 = GetModuleHandleA("kernel32.dll");
                      uintptr_t remoteKernel32 = GetRemoteModuleBase(pid, "kernel32.dll");
                      PVOID pExitThread = localKernel32 ? GetProcAddress(localKernel32, "ExitThread") : nullptr;
                      uintptr_t exitThreadRVA = (localKernel32 && remoteKernel32 && pExitThread) ?
                          (reinterpret_cast<uintptr_t>(pExitThread) - reinterpret_cast<uintptr_t>(localKernel32)) : 0;
                      uintptr_t remoteExitThread = (remoteKernel32 && exitThreadRVA) ?
                          (remoteKernel32 + exitThreadRVA) : 0;

                      if (!remoteExitThread) {
                          DebugLog("ERROR: could not resolve remote kernel32!ExitThread — aborting mapping");
                          evasion::EvasionVirtualFreeEx(hProcess, pdataRemote, 0, MEM_RELEASE);
                          evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                          evasion::EvasionNtClose(hProcess);
                          return false;
                      }

                        // Build stub: calls RtlAddFunctionTable, stores result in ECX, calls ExitThread(result)
                        // x64 ABI: RCX=FunctionTable, RDX=NumEntries, R8=BaseAddress, R9=0
                        std::vector<uint8_t> regStub;
                        auto emit8  = [&](uint8_t b)        { regStub.push_back(b); };
                        auto emit32 = [&](uint32_t v) {
                            emit8(static_cast<uint8_t>(v)); emit8(static_cast<uint8_t>(v >> 8));
                            emit8(static_cast<uint8_t>(v >> 16)); emit8(static_cast<uint8_t>(v >> 24));
                        };
                        auto emit64 = [&](uintptr_t v) {
                            for (int k = 0; k < 8; ++k) emit8(static_cast<uint8_t>(v >> (k * 8)));
                        };

                        emit8(0x48); emit8(0x83); emit8(0xEC); emit8(0x28);                   // sub rsp, 40
                        emit8(0x48); emit8(0xB9); emit64((uintptr_t)pdataRemote);              // mov rcx, pdataRemote
                        emit8(0xBA); emit32(numEntries);                                          // mov edx, numEntries (FIXED: was remoteBase)
                        emit8(0x49); emit8(0xB8); emit64((uintptr_t)remoteBase);               // mov r8, remoteBase (FIXED: was numEntries)
                        emit8(0x4D); emit8(0x31); emit8(0xC9);                                  // xor r9, r9
                        emit8(0x48); emit8(0xB8); emit64(remoteRtlFn);                          // mov rax, remoteRtlAddFunctionTable
                        emit8(0xFF); emit8(0xD0);                                               // call rax
                        emit8(0x8B); emit8(0xC8);                                               // mov ecx, eax (store result as exit code)
                        emit8(0x48); emit8(0xB8); emit64(remoteExitThread);                     // mov rax, remote ExitThread
                        emit8(0xFF); emit8(0xD0);                                               // call rax (ExitThread does not return)

                        SIZE_T stubSize = regStub.size();
                     LPVOID stubRemote = evasion::EvasionVirtualAllocEx(
                         hProcess, nullptr, stubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

                     if (stubRemote) {
                         evasion::EvasionWriteProcessMemory(hProcess, stubRemote, regStub.data(), stubSize, nullptr);
                         HANDLE hRegThread = evasion::CreateRemoteThreadHashed(hProcess, stubRemote, nullptr);
                         if (hRegThread) {
                             WaitForSingleObject(hRegThread, 5000);
                             DWORD exitCode = 0;
                             GetExitCodeThread(hRegThread, &exitCode);
                             evasion::EvasionNtClose(hRegThread);
                             char regLog[256];
                             snprintf(regLog, sizeof(regLog),
                                 "RtlAddFunctionTable result: %s (exitCode=%lu) entries=%lu base=0x%llx",
                                 exitCode ? "SUCCESS" : "FAILED", exitCode, numEntries,
                                 (unsigned long long)remoteBase);
                             DebugLog(regLog);
                         }
                         evasion::EvasionVirtualFreeEx(hProcess, stubRemote, 0, MEM_RELEASE);
                     }
                }
            }
        }
    }

    // OS-level init the OS loader normally performs but our manual mapper skips.
    // Collect the DLL's TLS callbacks and dispatch them on THIS thread before
    // DllMain. The first callback is the VCRUNTIME per-thread bootstrap (locale /
    // __security_cookie helpers); running it is what lets the static C++
    // initializers that _CRT_INIT runs afterwards touch the CRT without a
    // STATUS_STACK_BUFFER_OVERRUN.
    //
    // AddressOfCallBacks and each callback array entry are ambiguous in the
    // wild: the linker may emit a preferred-base VA, a relocated VA, or a raw
    // RVA. Normalize every candidate to the callback's REMOTE virtual address so
    // we never dereference a non-relocated preferred-base VA inside the target.
    std::vector<uintptr_t> tlsCallbacks;
    {
        const auto& tlsDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        const uintptr_t imageBase = ntHeaders->OptionalHeader.ImageBase;
        const uintptr_t remoteLo  = reinterpret_cast<uintptr_t>(remoteBase);
        const uintptr_t remoteHi  = remoteLo + imageSize;

        auto ToRemoteAddr = [&](uintptr_t field) -> uintptr_t {
            if (field >= remoteLo && field < remoteHi)        return field;             // relocated VA
            if (field >= imageBase && field < imageBase + imageSize)   // preferred-base VA
                return remoteLo + (field - imageBase);
            if (field < imageSize)                            return remoteLo + field;  // raw RVA
            return 0;                                         // uninterpretable -> skip
        };

        if (tlsDir.VirtualAddress && tlsDir.Size) {
            IMAGE_TLS_DIRECTORY64 tlsRemote = {};
            SIZE_T tlsRead = 0;
            if (evasion::EvasionReadProcessMemory(hProcess,
                    reinterpret_cast<LPCVOID>(remoteLo + tlsDir.VirtualAddress),
                    &tlsRemote, sizeof(tlsRemote), &tlsRead) && tlsRead == sizeof(tlsRemote)) {

                if (!tlsRemote.AddressOfCallBacks) {
                    DebugLog("[ASTRAL] TLS: directory present, no callbacks");
                } else {
                    uintptr_t cbArray = ToRemoteAddr(tlsRemote.AddressOfCallBacks);
                    if (!cbArray) {
                        DebugLog("[ASTRAL] TLS: unresolvable AddressOfCallBacks");
                    } else {
                        for (int guard = 0; guard < 64; ++guard) {
                            uintptr_t entry = 0;
                            SIZE_T cbR = 0;
                            if (!evasion::EvasionReadProcessMemory(hProcess,
                                    reinterpret_cast<LPCVOID>(cbArray), &entry, sizeof(entry), &cbR) ||
                                cbR != sizeof(entry) || entry == 0) {
                                break;
                            }
                            uintptr_t cbAddr = ToRemoteAddr(entry);
                            if (cbAddr) tlsCallbacks.push_back(cbAddr);
                            cbArray += sizeof(uintptr_t);
                        }
                    }
                }
            } else {
                DebugLog("[ASTRAL] TLS: failed to read TLS directory from remote image");
            }
        }
    }

    // Build one remote entry stub that the loader's thread will execute:
    //   - run every TLS callback with DLL_PROCESS_ATTACH (VCRUNTIME bootstrap)
    //   - call DllMain(hModule, DLL_PROCESS_ATTACH, nullptr)   (== AddressOfEntryPoint)
    //   - ExitThread(result)
    // Running TLS + DllMain on the same thread reproduces the OS loader ordering.
    std::vector<uint8_t> stub;
    auto emit  = [&](uint8_t b)        { stub.push_back(b); };
    auto emit32 = [&](uint32_t v) {
        emit(static_cast<uint8_t>(v)); emit(static_cast<uint8_t>(v >> 8));
        emit(static_cast<uint8_t>(v >> 16)); emit(static_cast<uint8_t>(v >> 24));
    };
    auto emit64 = [&](uintptr_t v) {
        for (int k = 0; k < 8; ++k) emit(static_cast<uint8_t>(v >> (k * 8)));
    };
    auto mov_rcx_imm = [&](uintptr_t v) { emit(0x48); emit(0xB9); emit64(v); }; // mov rcx, imm64

    emit(0x48); emit(0x83); emit(0xEC); emit(0x28);            // sub rsp, 40 (shadow + align)
    for (uintptr_t cb : tlsCallbacks) {
        mov_rcx_imm((uintptr_t)remoteBase);                    // DllHandle
        emit(0xBA); emit32(1);                                 // mov edx, DLL_PROCESS_ATTACH
        emit(0x4D); emit(0x31); emit(0xC0);                    // xor r8, r8 (Reserved)
        emit(0x48); emit(0xB8); emit64(cb);                    // mov rax, TLSCallback
        emit(0xFF); emit(0xD0);                                 // call rax
    }
    mov_rcx_imm((uintptr_t)remoteBase);                        // DllHandle
    emit(0xBA); emit32(1);                                     // mov edx, DLL_PROCESS_ATTACH
    emit(0x4D); emit(0x31); emit(0xC0);                        // xor r8, r8
    emit(0x48); emit(0xB8); emit64((uintptr_t)dllMainAddr);    // mov rax, DllMain (entry)
    emit(0xFF); emit(0xD0);                                     // call rax  (BOOL in eax)
    emit(0x8B); emit(0xC8);                                     // mov ecx, eax (exit code)
    PVOID pExitThread = GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitThread");
    uintptr_t remoteKernel32 = GetRemoteModuleBase(pid, "kernel32.dll");
    HMODULE localKernel32 = GetModuleHandleA("kernel32.dll");
    uintptr_t exitThreadRVA = (localKernel32 && remoteKernel32) ?
        (reinterpret_cast<uintptr_t>(pExitThread) - reinterpret_cast<uintptr_t>(localKernel32)) : 0;
    if (!remoteKernel32 || !exitThreadRVA) {
        DebugLog("ERROR: could not resolve remote kernel32!ExitThread for entry stub — aborting mapping");
        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        evasion::EvasionNtClose(hProcess);
        return false;
    }
    uintptr_t remoteExitThread = remoteKernel32 + exitThreadRVA;
    emit(0x48); emit(0xB8); emit64(remoteExitThread);    // mov rax, remote ExitThread
    emit(0xFF); emit(0xD0);                                     // call rax

    LPVOID stubRemote = evasion::EvasionVirtualAllocEx(
        hProcess, nullptr, stub.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubRemote) {
        DebugLog("ERROR: entry stub allocation failed");
        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        evasion::EvasionNtClose(hProcess);
        return false;
    }
    SIZE_T stubWritten = 0;
    if (!evasion::EvasionWriteProcessMemory(hProcess, stubRemote, stub.data(),
            stub.size(), &stubWritten) || stubWritten != stub.size()) {
        DebugLog("ERROR: entry stub write failed");
        evasion::EvasionVirtualFreeEx(hProcess, stubRemote, 0, MEM_RELEASE);
        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    char stubLog[128];
    snprintf(stubLog, sizeof(stubLog), "[ASTRAL] entry stub: %u TLS cb, DllMain=%p",
        static_cast<unsigned>(tlsCallbacks.size()), dllMainAddr);
    DebugLog(stubLog);

    DebugLog("[LOADER] calling entry stub at 0x" + std::to_string(reinterpret_cast<uintptr_t>(stubRemote)) +
             " dllMain=0x" + std::to_string(reinterpret_cast<uintptr_t>(dllMainAddr)));

    HANDLE hThread = evasion::CreateRemoteThreadHashed(hProcess, stubRemote, remoteBase);
    if (!hThread) {
        DebugLog("ERROR: Failed to create DllMain remote thread");
        evasion::EvasionVirtualFreeEx(hProcess, stubRemote, 0, MEM_RELEASE);
        evasion::EvasionVirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        evasion::EvasionNtClose(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    DebugLog("[LOADER] stub returned exitCode=" + std::to_string(exitCode));
    if (exitCode == STILL_ACTIVE) {
        DebugLog("[LOADER] WARNING: stub still running after 10s - likely deadlock in DllMain");
    }
    evasion::EvasionVirtualFreeEx(hProcess, stubRemote, 0, MEM_RELEASE);
    evasion::EvasionNtClose(hThread);


    // Close handle to target process via NtClose to eliminate handle telemetry.
    evasion::EvasionNtClose(hProcess);

    return true;
}

// Main entry point
int main(int argc, char* argv[]) {
    bool skipAuth = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--skip-auth") {
            skipAuth = true;
            break;
        }
    }

    // Console setup
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    HWND hConsoleWnd = GetConsoleWindow();

    // White/grey text
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);

    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    cfi.nFont = 0;
    cfi.dwFontSize.X = 8;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(hConsole, FALSE, &cfi);

    // Resize console to 80x30
    COORD coord = {80, 30};
    SetConsoleScreenBufferSize(hConsole, coord);
    SMALL_RECT sr = {0, 0, 79, 29};
    SetConsoleWindowInfo(hConsole, TRUE, &sr);

    // Slightly transparent
    if (hConsoleWnd) {
        LONG_PTR exStyle = GetWindowLongPtrA(hConsoleWnd, GWL_EXSTYLE);
        exStyle |= WS_EX_LAYERED;
        SetWindowLongPtrA(hConsoleWnd, GWL_EXSTYLE, exStyle);
        SetLayeredWindowAttributes(hConsoleWnd, 0, 230, LWA_ALPHA);

        // Cool title bar: PC name + quote
        char pcName[256] = "UNKNOWN";
        DWORD pcNameLen = sizeof(pcName);
        GetComputerNameA(pcName, &pcNameLen);

        std::string title = std::string("[*] ASTRAL    |    \"") + pcName +
                            std::string("\"    |    The cosmos favors the bold    |    [PREMIUM]");
        SetConsoleTitleA(title.c_str());
    }

    DWORD mode = 0;
    GetConsoleMode(hConsole, &mode);
    SetConsoleMode(hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    freopen("CONOUT$", "w", stdout);
    freopen("CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);

    // Also log to file for diagnostics (into Downloads\ASTRAL so it's easy to find).
    {
        wchar_t userProfile[MAX_PATH] = {};
        std::wstring logPath;
        if (ExpandEnvironmentStringsW(L"%USERPROFILE%", userProfile, MAX_PATH) > 0) {
            std::wstring dir = std::wstring(userProfile) + L"\\Downloads\\ASTRAL";
            CreateDirectoryW(dir.c_str(), nullptr);
            logPath = dir + L"\\loader_debug.log";
        } else {
            wchar_t tmpPath[MAX_PATH] = {};
            DWORD tmpN = GetTempPathW(MAX_PATH, tmpPath);
            logPath = (tmpN > 0) ? (std::wstring(tmpPath) + L"loader_debug.log")
                                 : std::wstring(L"loader_debug.log");
        }
        FILE* dbgFile = nullptr;
        _wfopen_s(&dbgFile, logPath.c_str(), L"w");
        g_dbgFile = dbgFile;
    }
    DebugLog("Loader started");
    DebugLog("Initializing KeyAuth...");

    // ASTRAL ASCII banner
    std::cout << "\n";
    std::cout << "                _             _\n";
    std::cout << "      /\\       | |           | |\n";
    std::cout << "     /  \\   ___| |_ _ __ __ _| |\n";
    std::cout << "    / /\\ \\ / __| __| '__/ _` | |\n";
    std::cout << "   / ____ \\\\__ \\ |_| | | (_| | |\n";
    std::cout << "  /_/    \\_\\___/\\__|_|  \\__,_|_|\n";
    std::cout << "\n\n";

    // KeyAuth authentication
    std::string name = "KEYAUTH";
    std::string ownerid = "oZf5UTf8iE";
    std::string version = "1.0";
    std::string url = "https://keyauth.win/api/1.2/";
    std::string secret = "b784bed72d59cc16881f945c8e8518c80194f6ad25d646e83569b52391682ec6";

    DebugLog("Initializing KeyAuth...");
    std::string display_user = "Astral_User";

    if (skipAuth) {
        DebugLog("Skipping KeyAuth (test mode)");
        std::cout << "  [ SYSTEM  ] : Skipping KeyAuth (--skip-auth)" << std::endl;
        std::cout << "  [ SUCCESS ] : Access Granted. Welcome, " << display_user << std::endl;
        std::cout << "  [ SYSTEM  ] : Initializing ASTRAL Engine..." << std::endl;
        std::cout << "  [+] Initializing settings..." << std::endl;
        std::cout << "  [+] Initializing input..." << std::endl;
        std::cout << "  [+] Initializing memory..." << std::endl;
        std::cout << "\n  [ SUCCESS ] : Loading session..." << std::endl;
        std::cout << "\n  [+] Opening cs2... " << std::flush;
    } else {
        KeyAuth::api KeyAuthApp(name, ownerid, version, url, secret);

        DebugLog("KeyAuth init() starting...");
        if (!KeyAuthApp.init()) {
            std::cout << "\n  [ ERROR ] : KeyAuth Init Failed: " << KeyAuthApp.response.message << std::endl;
            system("pause");
            if (g_dbgFile) fclose(g_dbgFile);
            return 1;
        }
        KeyAuthApp.clear_secret(); // S-12: clear secret from memory after init

        std::cout << "\n  [ SYSTEM  ] : Awaiting authentication...  " << GetTime() << std::endl;
        std::cout << "\n  LICENSE KEY > " << std::flush;

        std::string licenseKey;
        std::getline(std::cin, licenseKey);
        std::cout << "  [ SYSTEM  ] : Verifying key..." << std::endl;
        DebugLog("KeyAuth license() starting...");
        KeyAuthApp.license(licenseKey);
        DebugLog("KeyAuth license() returned success=" + std::string(KeyAuthApp.response.success ? "true" : "false"));

        while (!KeyAuthApp.response.success) {
            std::cout << "  [-] Invalid key: " << KeyAuthApp.response.message << std::endl;
            std::cout << "  LICENSE KEY > " << std::flush;
            std::getline(std::cin, licenseKey);
            std::cout << "  [ SYSTEM  ] : Verifying key..." << std::endl;
            KeyAuthApp.license(licenseKey);
        }

        std::cout << "  [ SUCCESS ] : Access Granted. Welcome, " << KeyAuthApp.get_user_data().username << std::endl;
        std::cout << "  [ SYSTEM  ] : Initializing ASTRAL Engine..." << std::endl;
        std::cout << "  [+] Initializing settings..." << std::endl;
        std::cout << "  [+] Initializing input..." << std::endl;
        std::cout << "  [+] Initializing memory..." << std::endl;
        std::cout << "\n  [ SUCCESS ] : Loading session..." << std::endl;
        std::cout << "\n  [+] Opening cs2... " << std::flush;
    }

    // STEP 2 — CS2 Loading Spinner
    bool isRunning = IsProcessRunning(L"cs2.exe");
    if (!isRunning) {
        ShellExecuteW(NULL, L"open", L"steam://rungameid/730", NULL, NULL, SW_SHOWNORMAL);
    }

    const char* spinner[] = { "|", "/", "-", "\\" };
    int idx = 0;
    DWORD pid = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        pid = FindProcessId(L"cs2.exe");
        if (pid) break;
        std::cout << "\r  [+] Opening cs2... " << spinner[idx] << std::flush;
        idx = (idx + 1) % 4;
        Sleep(1000);
    }
    if (!pid) {
        std::cout << "\r  [+] Opening cs2... FAILED.         \n" << std::flush;
        std::cout << "  [ ERROR ] : cs2.exe not found after 30s." << std::endl;
        system("pause");
        if (g_dbgFile) fclose(g_dbgFile);
        return 1;
    }
    std::cout << "\r  [+] Opening cs2... DONE.             \n" << std::flush;

    // Silently wait 10 seconds after detection to ensure full load
    Sleep(10000);

    // Get DLL path - search in loader dir, current dir, and common build paths
    char dllPath[MAX_PATH];
    GetModuleFileNameA(NULL, dllPath, MAX_PATH);
    std::string path(dllPath);
    size_t lastSlash = path.find_last_of("\\/");
    std::string dllDir = path.substr(0, lastSlash + 1);
    std::string dllName = "CS2Internal.dll";
    std::string dllFile;

    char cwdBuf[MAX_PATH];
    std::string cwdDir;
    if (GetCurrentDirectoryA(MAX_PATH, cwdBuf)) {
        cwdDir = std::string(cwdBuf) + "\\";
    }

    std::vector<std::string> searchPaths = {
        dllDir + dllName,
        cwdDir + dllName,
        dllDir + "..\\" + dllName,
        dllDir + "..\\bin\\Release\\" + dllName,
        dllDir + "..\\..\\" + dllName,
        dllDir + "..\\x64\\" + dllName,
        dllDir + "..\\Release\\" + dllName,
    };

    // Fallback: scan for any .dll in the loader's directory
    std::vector<std::string> dllSearchDirs = {dllDir, cwdDir};
    for (const auto& dir : dllSearchDirs) {
        if (dir.empty()) continue;
        std::string searchPattern = dir + "*.dll";
        WIN32_FIND_DATAA findData;
        HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                searchPaths.push_back(dir + findData.cFileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    bool dllFound = false;
    for (const auto& candidate : searchPaths) {
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            dllFile = candidate;
            dllFound = true;
            break;
        }
    }

    if (!dllFound) {
        std::cout << "  [ ERROR ] : CS2Internal.dll not found!" << std::endl;
        std::cout << "  [ ERROR ] : Looked in: " << std::endl;
        for (const auto& p : searchPaths) {
            std::cout << "    - " << p << std::endl;
        }
        std::cout << "  [ ERROR ] : Also scanned for *.dll in: " << dllDir << " and " << cwdDir << std::endl;
        system("pause");
        if (g_dbgFile) fclose(g_dbgFile);
        return 1;
    }

    // Validate DLL
    DebugLog("Validating DLL: " + dllFile);
    std::vector<uint8_t> testData = LoadDLL(dllFile);
    DebugLog("DLL loaded, size=" + std::to_string(testData.size()));
    if (testData.empty() || testData.size() < sizeof(IMAGE_DOS_HEADER)) {
        DebugLog("ERROR: Cannot read DLL file");
        std::cout << "  [ ERROR ] : Cannot read DLL file." << std::endl;
        system("pause");
        if (g_dbgFile) fclose(g_dbgFile);
        return 1;
    }
    DebugLog("DLL validated, size=" + std::to_string(testData.size()));

    // Named pipe handshake
    // Generate 32-byte session token cryptographically
    std::vector<uint8_t> sessionToken(32, 0);
    HCRYPTPROV hProv = 0;
    DebugLog("Initializing crypto provider");
    if (CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hProv, 32, sessionToken.data());

        // N-H-4: fully random pipe name — no predictable "ASTRAL_" prefix.
        std::string pipeName = "\\\\.\\pipe\\";
        char pipeSuffix[33] = {};
        for (int i = 0; i < 32; ++i) {
            uint8_t val = 0;
            CryptGenRandom(hProv, 1, &val);
            pipeSuffix[i] = "0123456789abcdef"[val % 16];
        }
        pipeName += pipeSuffix;

        CryptReleaseContext(hProv, 0);

        std::wstring pipeNameW(pipeName.begin(), pipeName.end());

        DebugLog("Pipe name: " + pipeName);

        HANDLE hPipe = CreateNamedPipeW(
            pipeNameW.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);

         if (hPipe == INVALID_HANDLE_VALUE) {
            DebugLog("ERROR: Failed to create named pipe");
            std::cout << "  [ ERROR ] : Failed to create named pipe." << std::endl;
            system("pause");
            if (g_dbgFile) fclose(g_dbgFile);
            return 1;
        }
        DebugLog("Pipe created successfully");

        // Open process
        DebugLog("Opening process PID=" + std::to_string(pid));
        HANDLE hProcess = evasion::EvasionOpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!hProcess) {
            DebugLog("ERROR: Failed to open process");
            std::cout << "  [ ERROR ] : Failed to open process. Run as Administrator." << std::endl;
            evasion::EvasionNtClose(hPipe);
            system("pause");
            if (g_dbgFile) fclose(g_dbgFile);
            return 1;
        }
        DebugLog("Process opened successfully");

        // Perform injection (passing the pipe name for the secure context handshake)
        bool injectionSuccess = SafeManualMap(hProcess, pid, dllFile, pipeNameW);

        // SafeManualMap closes hProcess internally

        if (!injectionSuccess) {
            DebugLog("Injection failed");
            std::cout << "  [ ERROR ] : Injection failed." << std::endl;
            evasion::EvasionNtClose(hPipe);
            system("pause");
            if (g_dbgFile) fclose(g_dbgFile);
            return 1;
        }

        std::cout << "\n  [ SUCCESS ] Injected Into Cs2.exe" << std::endl;

        evasion::EvasionNtClose(hPipe);

        // Handshake temporarily disabled for testing
        std::cout << "  [ SKIP ] Handshake disabled (testing mode)" << std::endl;
        DebugLog("Handshake skipped - testing mode");

        Sleep(1000);

        // STEP 3 — Splash Screen (only after successful injection)
        system("cls");

        std::string splash_user = display_user;
        if (splash_user.empty()) splash_user = "Astral_User";

        const std::string H  = "\xe2\x95\x80";  // ─
        const std::string V  = "\xe2\x95\x82";  // │
        const std::string TL = "\xe2\x95\xad";  // ╭
        const std::string TR = "\xe2\x95\xae";  // ╮
        const std::string BL = "\xe2\x95\xb0";  // ╰
        const std::string BR = "\xe2\x95\xaf";  // ╯
        const std::string ML = "\xe2\x95\x9c";  // ├
        const std::string MR = "\xe2\x95\xa4";  // ┤

        int boxWidth = 59;

        std::cout << "\n\n";
        // Top border
        std::cout << "  " << TL;
        for (int i = 0; i < boxWidth; i++) std::cout << H;
        std::cout << TR << "\n";
        // Empty
        std::cout << "  " << V << std::string(boxWidth, ' ') << V << "\n";
        // Title
        {
            std::string title = "                 A S T R A L   S U C C E S S               ";
            std::cout << "  " << V << title << std::string(boxWidth - title.length(), ' ') << V << "\n";
        }
        // Empty
        std::cout << "  " << V << std::string(boxWidth, ' ') << V << "\n";
        // Separator
        std::cout << "  " << ML;
        for (int i = 0; i < boxWidth; i++) std::cout << H;
        std::cout << MR << "\n";
        // Empty
        std::cout << "  " << V << std::string(boxWidth, ' ') << V << "\n";
        // Quote line 1
        std::cout << "  " << V << "   \"The cosmos favors those who dare to venture into the   " << V << "\n";
        // Quote line 2
        std::cout << "  " << V << "    unknown.\"                                              " << V << "\n";
        // Empty
        std::cout << "  " << V << std::string(boxWidth, ' ') << V << "\n";
        // Bottom border
        std::cout << "  " << BL;
        for (int i = 0; i < boxWidth; i++) std::cout << H;
        std::cout << BR << "\n";

        std::cout << "\n";
        std::cout << "    \tUSER:     " << splash_user << "\n";
        std::cout << "     STATUS:   UNDETECTED\n";
        std::cout << "     TIER:     PREMIUM\n\n";
        std::cout << "==================================================================\n";
        std::cout << " [ SYSTEM  ] : ASTRAL Engine is currently running.\n";
        std::cout << " [ SYSTEM  ] : Press RIGHTSHIFT in-game to toggle the menu.\n";
        std::cout << "==================================================================\n\n";

        // STEP 4 — Uptime Timer
        auto start_time = std::chrono::steady_clock::now();

        std::thread([start_time]() {
            while (g_running) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

                int hours = elapsed / 3600;
                int minutes = (elapsed % 3600) / 60;
                int seconds = elapsed % 60;

                std::cout << "\r  [+] Cheating:  " << std::setfill('0') << std::setw(2) << hours << ":"
                          << std::setfill('0') << std::setw(2) << minutes << ":"
                          << std::setfill('0') << std::setw(2) << seconds << "          " << std::flush;

                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();

        // Wait
        while (g_running) {
            Sleep(1000);
        }
    } else {
        std::cout << "  [ ERROR ] : Failed to initialize cryptographic provider." << std::endl;
        system("pause");
        if (g_dbgFile) fclose(g_dbgFile);
        return 1;
    }

    return 0;
}
