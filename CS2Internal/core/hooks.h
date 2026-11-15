#pragma once

#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "sdk.h"

namespace hooks {

using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, const void*);
// CS2 CCSGOInput::CreateMove is a VIRTUAL at vtable index 5 and takes exactly
// (CCSGOInput* this, int slot, bool active). The CUserCmd is NOT an argument —
// it is fetched from the CCSGOInput command ring after the original returns.
using CreateMove_t = bool(__fastcall*)(void*, int, bool);
// SerializeUserCmd (include/patterns/patterns.hpp, dump RVA 0x962660): turns the
// live CUserCmd into the bytes sent to the server. The dump pattern reads
// [rdx+0x20] (CCSGOUserCmdPB has_bits) and [rdx+0x30] (input-history int), so rdx
// carries the CUserCmd. Signature tail (extra args) is passed through untouched.
using SerializeUserCmd_t = int(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

class HookManager {
public:
    static HookManager& Get();

    bool Initialize();
    void Shutdown();

    static HRESULT WINAPI hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
    static HRESULT WINAPI hkPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, const void* params);
    static bool __fastcall hkCreateMove(void* pThis, int slot, bool active);
    static int __fastcall hkSerializeUserCmd(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);

    bool IsInitialized() const { return initialized; }
    bool IsImGuiInitialized() const { return imguiInitialized; }
    ID3D11Device* GetDevice() { return device; }
    ID3D11DeviceContext* GetContext() { return context; }
    ID3D11RenderTargetView* GetRenderTargetView() { return renderTargetView; }
    HWND GetHWND() { return hwnd; }
    WNDPROC GetOrigWndProc() { return origWndProc; }
    void SetOrigWndProc(WNDPROC proc) { origWndProc = proc; }
    void SetHWND(HWND h) { hwnd = h; }
    void SetImGuiInitialized(bool val) { imguiInitialized = val; }
    Present_t GetOrigPresent() { return origPresent; }
    void SetOrigPresent(Present_t orig) { origPresent = orig; }

private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    bool InitializeImGui(IDXGISwapChain* swapChain);
    bool EnsureRenderTarget(IDXGISwapChain* swapChain);
    static void RenderImGuiPass(HookManager* self, IDXGISwapChain* swapChain);
    void RenderFrame(IDXGISwapChain* swapChain);
    bool InstallCreateMove();
    bool InstallSerializeUserCmd();

    bool initialized = false;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* renderTargetView = nullptr;
    UINT renderTargetWidth = 0;
    UINT renderTargetHeight = 0;
    Present_t origPresent = nullptr;
    Present1_t origPresent1 = nullptr;
    CreateMove_t origCreateMove = nullptr;
    SerializeUserCmd_t origSerializeUserCmd = nullptr;
    void* serializeTarget = nullptr;
    void* createMoveTarget = nullptr;
    void* presentTarget = nullptr;
    void* present1Target = nullptr;
    HWND hwnd = nullptr;
    WNDPROC origWndProc = nullptr;
    bool imguiInitialized = false;
};

} // namespace hooks
