// language: C++, file: mmap_self.cpp, target: Windows x64
// Faithful manual-map probe for CS2Internal.dll: relocated copy + import
// resolution + TLS callback dispatch + DllMain on one thread, identical in
// spirit to ASTRAL_Loader's SafeManualMap. Exits 0 only if the entry bootstrap
// survives (no STATUS_STACK_BUFFER_OVERRUN / access violation). Worker threads
// spawned by DllMain idle without client.dll, so they are harmless here.
#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstdint>
#include <cstring>

static bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    bool ok = ReadFile(f, out.data(), (DWORD)sz.QuadPart, &rd, nullptr) && rd == (DWORD)sz.QuadPart;
    CloseHandle(f);
    return ok;
}

static DWORD RvaToRaw(const uint8_t* img, IMAGE_NT_HEADERS* nt, DWORD rva) {
    auto* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (rva >= s[i].VirtualAddress &&
            rva < s[i].VirtualAddress + (s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData))
            return s[i].PointerToRawData + (rva - s[i].VirtualAddress);
    }
    return rva;
}

int main(int argc, char** argv) {
    const char* dllpath = (argc > 1) ? argv[1] : "CS2Internal.dll";
    std::vector<uint8_t> dll;
    if (!ReadFile(dllpath, dll)) { std::printf("read dll failed\n"); return 1; }

    auto* dos = (IMAGE_DOS_HEADER*)dll.data();
    auto* nt = (IMAGE_NT_HEADERS*)(dll.data() + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint64_t imageBasePref = nt->OptionalHeader.ImageBase;
    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;

    // Pull import dependencies into this process so GetProcAddress resolves them.
    const char* deps[] = {"kernel32.dll","user32.dll","d3d11.dll","dxgi.dll",
        "d3dcompiler_47.dll","gdi32.dll","shell32.dll","imm32.dll","ntdll.dll"};
    for (auto* d : deps) LoadLibraryA(d);

    void* base = VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base) { std::printf("alloc failed\n"); return 1; }
    uintptr_t remoteLo = (uintptr_t)base;
    uintptr_t delta = remoteLo - (uintptr_t)imageBasePref;
    std::printf("mapped at %p (delta %p)\n", base, (void*)delta);

    memcpy(base, dll.data(), nt->OptionalHeader.SizeOfHeaders);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (sec[i].SizeOfRawData)
            memcpy((void*)(remoteLo + sec[i].VirtualAddress),
                dll.data() + sec[i].PointerToRawData, sec[i].SizeOfRawData);

    // Relocations (x64: IMAGE_REL_BASED_DIR64)
    auto& rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (rel.VirtualAddress && rel.Size) {
        auto* rb = (IMAGE_BASE_RELOCATION*)(dll.data() + RvaToRaw(dll.data(), nt, rel.VirtualAddress));
        uint8_t* relend = (uint8_t*)rb + rel.Size;
        while ((uint8_t*)rb < relend && rb->SizeOfBlock) {
            DWORD cnt = (rb->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* types = (WORD*)((uint8_t*)rb + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD j = 0; j < cnt; ++j)
                if ((types[j] >> 12) == IMAGE_REL_BASED_DIR64)
                    *(uintptr_t*)(remoteLo + rb->VirtualAddress + (types[j] & 0xFFF)) += delta;
            rb = (IMAGE_BASE_RELOCATION*)((uint8_t*)rb + rb->SizeOfBlock);
        }
    }

    // Import resolution
    auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress) {
        auto* id = (IMAGE_IMPORT_DESCRIPTOR*)(dll.data() + RvaToRaw(dll.data(), nt, imp.VirtualAddress));
        while (id->Name) {
            const char* name = (const char*)(dll.data() + RvaToRaw(dll.data(), nt, id->Name));
            HMODULE hm = LoadLibraryA(name);
            if (hm) {
                auto* thunk = (IMAGE_THUNK_DATA*)(dll.data() + RvaToRaw(dll.data(), nt, id->FirstThunk));
                BYTE* iatRemote = (BYTE*)(remoteLo + id->FirstThunk);
                while (thunk->u1.AddressOfData) {
                    void* fn = nullptr;
                    if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                        fn = GetProcAddress(hm, (LPCSTR)(thunk->u1.Ordinal & 0xFFFF));
                    else {
                        auto* ibn = (IMAGE_IMPORT_BY_NAME*)(dll.data() + RvaToRaw(dll.data(), nt, (DWORD)thunk->u1.AddressOfData));
                        fn = GetProcAddress(hm, (LPCSTR)ibn->Name);
                    }
                    size_t off = (BYTE*)thunk - (dll.data() + RvaToRaw(dll.data(), nt, id->FirstThunk));
                    if (fn) *(void**)(iatRemote + off) = fn;
                    ++thunk;
                }
            }
            ++id;
        }
    }

    // Apply section protections (code must be executable)
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        DWORD prot = PAGE_READONLY;
        bool ex = (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool wr = (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        if (ex && wr) prot = PAGE_EXECUTE_READWRITE;
        else if (ex) prot = PAGE_EXECUTE_READ;
        else if (wr) prot = PAGE_READWRITE;
        SIZE_T sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (sz) { DWORD op; VirtualProtect((void*)(remoteLo + sec[i].VirtualAddress), sz, prot, &op); }
    }

    // TLS callbacks (normalize VA/RVA/relocated, same logic as the loader)
    std::vector<uintptr_t> cbs;
    {
        auto& tlsd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        uintptr_t ib = (uintptr_t)imageBasePref, hi = remoteLo + imageSize;
        auto norm = [&](uint64_t f) -> uintptr_t {
            if (f >= remoteLo && f < hi) return (uintptr_t)f;
            if (f >= ib && f < ib + imageSize) return remoteLo + (uintptr_t)(f - ib);
            if (f < (uint64_t)imageSize) return remoteLo + (uintptr_t)f;
            return 0;
        };
        if (tlsd.VirtualAddress && tlsd.Size) {
            IMAGE_TLS_DIRECTORY64 tr{};
            memcpy(&tr, (void*)(remoteLo + tlsd.VirtualAddress), sizeof(tr));
            if (tr.AddressOfCallBacks) {
                uintptr_t cba = norm(tr.AddressOfCallBacks);
                if (cba) {
                    for (int g = 0; g < 64; ++g) {
                        uint64_t e = 0;
                        memcpy(&e, (void*)cba, sizeof(e));
                        if (!e) break;
                        uintptr_t a = norm(e);
                        if (a) cbs.push_back(a);
                        cba += sizeof(uint64_t);
                    }
                }
            }
        }
    }

    std::printf("TLS callbacks resolved: %u\n", (unsigned)cbs.size());
    for (uintptr_t a : cbs) {
        std::printf("  running cb %p ... ", (void*)a);
        fflush(stdout);
        ((void(*)(PVOID, DWORD, PVOID))a)((PVOID)remoteLo, DLL_PROCESS_ATTACH, nullptr);
        std::printf("ok\n");
    }

    uintptr_t entry = remoteLo + nt->OptionalHeader.AddressOfEntryPoint;
    std::printf("calling DllMain @ %p ... ", (void*)entry); fflush(stdout);
    BOOL r = ((BOOL(*)(HMODULE, DWORD, LPVOID))entry)((HMODULE)remoteLo, DLL_PROCESS_ATTACH, nullptr);
    std::printf("returned %d\n", (int)r);

    Sleep(2000);
    if (r) std::printf("RESULT=SURVIVED\n"); else std::printf("RESULT=FAILED\n");
    return r ? 0 : 1;
}
