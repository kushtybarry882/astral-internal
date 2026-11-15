#pragma once

#include <imgui.h>

namespace theme {

void Apply();
void LoadFonts();

ImFont* TitleFont();
ImFont* SmallFont();
ImFont* CodeFont();
ImFont* BodyFont();

} // namespace theme
