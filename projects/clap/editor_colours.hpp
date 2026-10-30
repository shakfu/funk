#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

// Draws text in the colours of tokens, a Token (editor_logic.hpp) per byte, over the multiline
// field id, which the current window has just drawn with its text transparent. It repeats
// InputTextEx's call of ImFont::RenderText (imgui_widgets.cpp, imgui_draw.cpp, ImGui 1.93) with a
// colour per byte, so each glyph lands where ImGui would have drawn it: test_editor_colours checks it.
void DrawTokens(ImGuiID id, const std::string& text, const std::vector<uint8_t>& tokens, bool wrap);
