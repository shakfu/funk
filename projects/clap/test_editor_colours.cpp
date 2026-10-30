// DrawTokens (editor_colours.cpp) against ImGui itself, headless: the code field drawn by ImGui,
// then with its text transparent and DrawTokens over it, must give the same glyph vertices,
// position, texture and colour, wrapped or not, scrolled, active, and at fractional font sizes.
// Prints each case; exits 1 if any differs.

#include "editor_colours.hpp"
#include "editor_logic.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Case {
	const char* name;
	std::string text;
	bool wrap;
	float scale;   // style.FontScaleMain
	float scrollY; // the field's, set after the first frame; 0 to leave it
	bool active;   // keyboard focus in the field, the caret at the end
	bool scrollX;  // the field must have scrolled horizontally
};

float gScrollX; // the field's horizontal scroll at the last frame of Glyphs

std::string Lines(int n, const char* line)
{
	std::string s;
	for (int i = 0; i < n; i++) s += std::to_string(i) + " " + line + "\n";
	return s;
}

// The glyph vertices of the field's child window: those not using the white pixel, which solid
// fills (frame, selection, caret, scrollbar) do.
// A fresh context for each, so that one case's active field does not carry into the next.
std::vector<ImDrawVert> Glyphs(const Case& c, bool overlay)
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(720, 480);
	io.DeltaTime = 1.f / 30.f;
	io.Fonts->AddFontDefault();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	ImGui::GetStyle().FontScaleMain = c.scale;
	std::string text = c.text;
	ImGuiWindow* child = nullptr;
	for (int frame = 0; frame < 6; frame++) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::Begin("editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		const ImGuiID id = ImGui::GetID("##code");
		if (c.active && frame == 0) ImGui::SetKeyboardFocusHere();
		if (c.active && frame == 2) ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true), ImGui::GetIO().AddKeyEvent(ImGuiKey_End, true);
		if (c.active && frame == 3) ImGui::GetIO().AddKeyEvent(ImGuiKey_End, false), ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | (c.wrap ? ImGuiInputTextFlags_WordWrap : 0);
		if (overlay) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
		ImGui::InputTextMultiline("##code", &text, ImVec2(-1, 300), flags);
		if (overlay) {
			ImGui::PopStyleColor();
			DrawTokens(id, text, std::vector<uint8_t>(text.size(), kPlain), c.wrap);
		}
		child = nullptr;
		for (ImGuiWindow* w : ImGui::GetCurrentWindow()->DC.ChildWindows)
			if (w->ChildId == id) child = w;
		if (frame == 0 && child && c.scrollY > 0) ImGui::SetScrollY(child, c.scrollY);
		ImGuiInputTextState* state = ImGui::GetInputTextState(id);
		gScrollX = state && GImGui->ActiveId == id ? state->Scroll.x : 0.f;
		ImGui::End();
		ImGui::Render();
	}
	std::vector<ImDrawVert> out;
	const ImVec2 white = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
	if (child)
		for (const ImDrawVert& v : child->DrawList->VtxBuffer)
			if (v.uv.x != white.x || v.uv.y != white.y) out.push_back(v);
	ImGui::DestroyContext();
	return out;
}

bool Same(const ImDrawVert& a, const ImDrawVert& b)
{
	return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.col == b.col;
}

} // namespace

int main()
{
	const std::string prose = "a long line of words that wraps in a field of 720 pixels, at least once and maybe twice, "
	                          "since it goes on and on well past the right edge of the editor";
	const Case cases[] = {
		{"short lines", "f :: Int ->> ()\nf n = n + 1\n\n-- a comment\n", true, 1.f, 0.f, false, false},
		{"wrapped", prose + "\n" + prose, true, 1.f, 0.f, false, false},
		{"wrapped, font x1.3", prose + "\n" + prose, true, 1.3f, 0.f, false, false},
		{"not wrapped, clipped", prose + "\n" + prose, false, 1.f, 0.f, false, false},
		{"scrolled, font x1.15", Lines(200, "p.fastcat [p.atom 60, p.silence]"), true, 1.15f, 1000.f, false, false},
		{"scrolled, not wrapped", Lines(200, prose.c_str()), false, 1.f, 777.f, false, false},
		{"active", prose + "\nsecond line\n", true, 1.f, 0.f, true, false},
		{"active, scrolled right", prose + prose, false, 1.f, 0.f, true, true},
		{"tabs and UTF-8", "\tx = \"caf\xc3\xa9\"\n\t\ty = 1\r\n", true, 1.f, 0.f, false, false},
	};
	int failed = 0;
	for (const Case& c : cases) {
		const std::vector<ImDrawVert> imgui = Glyphs(c, false), ours = Glyphs(c, true);
		size_t differ = imgui.size() == ours.size() ? imgui.size() : 0;
		for (size_t i = 0; i < imgui.size() && i < ours.size(); i++)
			if (!Same(imgui[i], ours[i])) {
				differ = i;
				break;
			}
		const float scrollX = gScrollX;
		const bool ok = imgui.size() == ours.size() && differ == imgui.size() && !imgui.empty() && (!c.scrollX || scrollX > 0.f);
		failed += !ok;
		if (ok) printf("PASS: %s: %zu glyph vertices, scrolled %g right\n", c.name, imgui.size(), scrollX);
		else if (c.scrollX && scrollX <= 0.f) printf("FAIL: %s: the field did not scroll right\n", c.name);
		else if (imgui.empty()) printf("FAIL: %s: ImGui drew no glyphs\n", c.name);
		else printf("FAIL: %s: %zu vertices against ImGui's %zu, first difference at %zu\n", c.name, ours.size(), imgui.size(), differ);
		if (!ok && getenv("DEBUG") && differ < imgui.size() && differ < ours.size())
			printf("  imgui (%g, %g) uv (%g, %g) col %08x; ours (%g, %g) uv (%g, %g) col %08x\n", imgui[differ].pos.x, imgui[differ].pos.y,
			       imgui[differ].uv.x, imgui[differ].uv.y, imgui[differ].col, ours[differ].pos.x, ours[differ].pos.y, ours[differ].uv.x,
			       ours[differ].uv.y, ours[differ].col);
	}
	return failed ? 1 : 0;
}
