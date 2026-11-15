#include "theme.h"

#include <windows.h>
#include <cmath>

namespace theme {

namespace {

ImFont* g_fontBody   = nullptr;
ImFont* g_fontTitle  = nullptr;
ImFont* g_fontSmall  = nullptr;
ImFont* g_fontCode   = nullptr;

ImU32 ToColor(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

} // namespace

void Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 12.0f;
    style.ScrollbarSize = 6.0f;
    style.GrabMinSize = 8.0f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    // LEGIONNAIRE v3.0 color palette — expanded for full UI theming
    constexpr ImVec4 kWindowBg       = ImVec4(0.12f, 0.14f, 0.18f, 0.95f);
    constexpr ImVec4 kChildBg    = ImVec4(0.12f, 0.14f, 0.18f, 0.95f);
    constexpr ImVec4 kFrameBg    = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    constexpr ImVec4 kBorder     = ImVec4(0.20f, 0.24f, 0.28f, 0.50f);
    constexpr ImVec4 kTeal       = ImVec4(0.20f, 0.85f, 0.85f, 1.00f);
    constexpr ImVec4 kTealActive    = ImVec4(0.15f, 0.75f, 0.75f, 1.00f);
    constexpr ImVec4 kOcean         = ImVec4(0.10f, 0.65f, 0.80f, 1.00f);
    constexpr ImVec4 kOceanHover    = ImVec4(0.15f, 0.75f, 0.85f, 1.00f);
    constexpr ImVec4 kLime          = ImVec4(0.25f, 0.95f, 0.35f, 1.00f);
    constexpr ImVec4 kLimeHover     = ImVec4(0.35f, 1.00f, 0.45f, 1.00f);
    constexpr ImVec4 kOrange        = ImVec4(1.00f, 0.75f, 0.20f, 1.00f);
    constexpr ImVec4 kOrangeHover   = ImVec4(1.10f, 0.85f, 0.30f, 1.00f);
    constexpr ImVec4 kPink          = ImVec4(1.00f, 0.40f, 0.70f, 1.00f);
    constexpr ImVec4 kPinkHover     = ImVec4(1.10f, 0.50f, 0.80f, 1.00f);
    constexpr ImVec4 kPurple        = ImVec4(0.70f, 0.30f, 0.90f, 1.00f);
    constexpr ImVec4 kPurpleHover   = ImVec4(0.80f, 0.40f, 1.00f, 1.00f);
    constexpr ImVec4 kYellow        = ImVec4(1.00f, 0.95f, 0.15f, 1.00f);
    constexpr ImVec4 kYellowHover   = ImVec4(1.10f, 1.05f, 0.25f, 1.00f);
    constexpr ImVec4 kSubtleBg      = ImVec4(0.14f, 0.16f, 0.20f, 0.80f);
    constexpr ImVec4 kTealHover  = ImVec4(0.25f, 0.95f, 0.95f, 1.00f);
    constexpr ImVec4 kText       = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    constexpr ImVec4 kTextMuted  = ImVec4(0.6f, 0.65f, 0.7f, 1.0f);
    constexpr ImVec4 kTextDim    = ImVec4(0.6f, 0.65f, 0.7f, 0.6f);

    style.Colors[ImGuiCol_Text]              = kText;
    style.Colors[ImGuiCol_TextDisabled]      = kTextMuted;
    style.Colors[ImGuiCol_WindowBg]          = kWindowBg;
    style.Colors[ImGuiCol_ChildBg]           = kChildBg;
    style.Colors[ImGuiCol_PopupBg]           = kChildBg;
    style.Colors[ImGuiCol_Border]            = kBorder;
    style.Colors[ImGuiCol_FrameBg]           = kFrameBg;
    style.Colors[ImGuiCol_FrameBgHovered]    = ImVec4(0.22f, 0.28f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive]     = ImVec4(0.20f, 0.55f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab]        = kTeal;
    style.Colors[ImGuiCol_SliderGrabActive]  = kTealHover;
    style.Colors[ImGuiCol_Button]            = kFrameBg;
    style.Colors[ImGuiCol_ButtonHovered]     = ImVec4(0.22f, 0.28f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive]      = ImVec4(0.20f, 0.55f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]         = kTeal;
    style.Colors[ImGuiCol_Header]            = ImVec4(0.20f, 0.55f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered]     = ImVec4(0.25f, 0.65f, 0.75f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive]      = kTealActive;
    style.Colors[ImGuiCol_Separator]         = kBorder;
    style.Colors[ImGuiCol_SeparatorHovered]  = kTealHover;
    style.Colors[ImGuiCol_SeparatorActive]   = kLime;
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ResizeGripHovered] = kTeal;
    style.Colors[ImGuiCol_ResizeGripActive]  = kTealHover;
    style.Colors[ImGuiCol_TitleBg]           = kSubtleBg;
    style.Colors[ImGuiCol_TitleBgActive]     = kSubtleBg;
    style.Colors[ImGuiCol_TitleBgCollapsed]  = kSubtleBg;
    style.Colors[ImGuiCol_TabHovered]        = kOcean;
    style.Colors[ImGuiCol_Tab]               = kChildBg;
    style.Colors[ImGuiCol_TabSelected]       = kTeal;
    style.Colors[ImGuiCol_TabDimmed]         = kChildBg;
    style.Colors[ImGuiCol_TabDimmedSelected] = kOcean;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF,
        0x2606, 0x2606,
        0xE000, 0xE100,
        0
    };

    g_fontBody = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f, nullptr, glyphRanges);
    g_fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 20.0f, nullptr, glyphRanges);
    g_fontSmall = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 12.0f, nullptr, glyphRanges);
    g_fontCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 12.0f, nullptr, glyphRanges);

    if (!g_fontBody)  g_fontBody  = io.Fonts->AddFontDefault();
    if (!g_fontTitle) g_fontTitle = g_fontBody;
    if (!g_fontSmall) g_fontSmall = g_fontBody;
    if (!g_fontCode)  g_fontCode  = g_fontBody;

    io.FontDefault = g_fontBody;
}

ImFont* TitleFont()  { return g_fontTitle; }
ImFont* SmallFont()  { return g_fontSmall; }
ImFont* CodeFont()   { return g_fontCode; }
ImFont* BodyFont()   { return g_fontBody; }

} // namespace theme
