// The settings menu's colours.
// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/app/panel_colors.h (the F10 panel's
// colours, the HUD's sage green there); here in Far Cry Instincts' jungle green and sand.
#pragma once
#include <utility>

#include <imgui.h>

namespace app {

inline constexpr ImVec4 kPanelBg{0.035f, 0.045f, 0.035f, 0.94f};
inline constexpr ImVec4 kBorder{0.66f, 0.62f, 0.45f, 0.85f};
inline constexpr ImVec4 kText{0.91f, 0.90f, 0.84f, 1.0f};
inline constexpr ImVec4 kHighlight{0.22f, 0.30f, 0.17f, 1.0f};
inline constexpr ImVec4 kHeading{0.80f, 0.70f, 0.42f, 1.0f};
inline constexpr ImVec4 kDim{0.62f, 0.62f, 0.55f, 1.0f};
inline constexpr ImVec4 kRestart{0.90f, 0.62f, 0.30f, 1.0f};  // a change that waits for a restart
inline constexpr ImVec4 kTab{0.10f, 0.13f, 0.09f, 1.0f};
inline constexpr ImVec4 kTabHovered{0.17f, 0.23f, 0.14f, 1.0f};
inline constexpr ImVec4 kButton{0.14f, 0.18f, 0.12f, 1.0f};
inline constexpr ImVec4 kButtonHovered{0.31f, 0.40f, 0.24f, 1.0f};  // lighter than the highlight, so it shows on a selected row
inline constexpr ImVec4 kLine{0.80f, 0.70f, 0.42f, 0.35f};

inline constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
    {ImGuiCol_TextSelectedBg, kButtonHovered},
};

}  // namespace app
