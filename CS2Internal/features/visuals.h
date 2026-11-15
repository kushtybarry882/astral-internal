// AUDIT: P0-6, P1-12, P3-9, P3-10
// UPGRADE: V-1, V-2, V-3, V-4, V-5, P-3
#pragma once

#include "../core/sdk.h"
#include <imgui.h>

namespace Visuals {

void Draw();
void AddBulletTracer(const sdk::Vector& start, const sdk::Vector& end);

// Drawing helpers accessible from external hooks
void DrawHealthBar(ImDrawList* drawList, ImVec2 topLeft, float height, int health);
void DrawArmorBar(ImDrawList* drawList, ImVec2 topLeft, float height, int armor);
void DrawCornerBox(ImDrawList* drawList, ImVec2 topLeft, ImVec2 bottomRight, ImU32 color, float thickness);

} // namespace Visuals
