// Syntax colours over the editor's code field: editor_colours.hpp.

#include "editor_colours.hpp"
#include "editor_logic.hpp"

#include <imgui_internal.h>

// Token's colours (editor_logic.hpp); kPlain is the style's text colour.
static const ImU32 kTokenColours[kTokenCount] = {
	0,
	IM_COL32(128, 140, 128, 255), // comment
	IM_COL32(204, 140, 242, 255), // keyword
	IM_COL32(152, 216, 140, 255), // string
	IM_COL32(242, 178, 115, 255), // number
	IM_COL32(115, 204, 242, 255), // type
	IM_COL32(242, 216, 115, 255), // constructor
	IM_COL32(140, 178, 255, 255), // function
	IM_COL32(230, 153, 153, 255), // operator
	IM_COL32(166, 191, 242, 255), // module
	IM_COL32(178, 178, 178, 255), // punctuation
	IM_COL32(216, 191, 153, 255), // field
};

void DrawTokens(ImGuiID id, const std::string& text, const std::vector<uint8_t>& tokens, bool wrap)
{
	ImGuiContext& g = *GImGui;
	ImGuiWindow* w = nullptr;
	for (ImGuiWindow* child : ImGui::GetCurrentWindow()->DC.ChildWindows)
		if (child->ChildId == id) w = child;
	if (!w || text.empty() || tokens.size() != text.size()) return;
	ImGuiInputTextState* state = ImGui::GetInputTextState(id);
	const bool active = state && g.ActiveId == id;
	// draw_pos: the child's content start at its current scroll, which InputTextEx may have just
	// changed, plus the frame padding it adds; clip_rect: the frame less the scrollbar, within the child
	const ImVec2 start((float)((double)w->Pos.x + w->WindowPadding.x - (double)w->Scroll.x + w->DecoOuterSizeX1 + w->DC.ColumnsOffset.x)
	                       + g.Style.FramePadding.x,
	                   (float)((double)w->Pos.y + w->WindowPadding.y - (double)w->Scroll.y + w->DecoOuterSizeY1) + g.Style.FramePadding.y);
	ImRect frame(w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x - w->ScrollbarSizes.x, w->Pos.y + w->Size.y);
	frame.ClipWith(w->InnerClipRect);
	const ImVec4 clip = frame.ToVec4();
	ImFont* font = g.Font;
	const float size = g.FontSize;
	const ImDrawTextFlags flags = ImDrawTextFlags_WrapKeepBlanks | ImDrawTextFlags_CpuFineClip;
	const float wrapWidth = !wrap ? 0.f : active ? state->WrapWidth
	                      : ImMax(1.f, w->ContentRegionRect.Max.x - start.x + (w->ScrollbarY ? 0.f : -g.Style.ScrollbarSize));

	// The first visible line, as CalcClipRectVisibleItemsY and InputTextLineIndexBuild find it
	const char* const begin = text.data();
	const char* const end = begin + text.size();
	const int first = ImMax((int)((clip.y - start.y) / size), 0);
	const char* s = begin;
	for (int line = 0; line < first && s < end; line++) {
		s = wrap ? ImFontCalcWordWrapPositionEx(font, size, s, end, wrapWidth, ImDrawTextFlags_WrapKeepBlanks)
		         : (const char*)ImMemchr(s, '\n', end - s);
		if (!s) s = end;
		if (s < end && *s == '\n') s++;
	}
	if (s >= end) return;

	ImDrawList* dl = w->DrawList;
	float x = start.x - (active ? state->Scroll.x : 0.f);
	float y = start.y + first * size;
	if (((flags | dl->Flags) & ImDrawFlags_TextNoPixelSnap) == 0) {
		x = IM_TRUNC(x);
		y = IM_TRUNC(y);
	}
	ImFontBaked* baked = font->GetFontBaked(size);
	const float scale = size / baked->Size;
	const float originX = x;
	const ImU32 plain = ImGui::GetColorU32(ImGuiCol_Text);
	const char* eol = nullptr;
	dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w));
	while (s < end) {
		if (wrap) {
			if (!eol) eol = ImFontCalcWordWrapPositionEx(font, size, s, end, wrapWidth - (x - originX), flags);
			if (s >= eol) {
				x = originX;
				y += size;
				if (y > clip.w) break;
				eol = nullptr;
				s = ImTextCalcWordWrapNextLineStart(s, end, flags);
				continue;
			}
		}
		const uint8_t token = tokens[s - begin];
		unsigned int c = (unsigned char)*s;
		if (c < 0x80) s += 1;
		else s += ImTextCharFromUtf8(&c, s, end);
		if (c < 32) {
			if (c == '\n') {
				x = originX;
				y += size;
				if (y > clip.w) break;
				continue;
			}
			if (c == '\r') continue;
		}
		const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)c);
		const float advance = glyph->AdvanceX * scale;
		if (glyph->Visible) {
			float x1 = x + glyph->X0 * scale, x2 = x + glyph->X1 * scale;
			float y1 = y + glyph->Y0 * scale, y2 = y + glyph->Y1 * scale;
			if (x1 <= clip.z && x2 >= clip.x) {
				float u1 = glyph->U0, v1 = glyph->V0, u2 = glyph->U1, v2 = glyph->V1;
				if (x1 < clip.x) { u1 = u1 + (1.f - (x2 - clip.x) / (x2 - x1)) * (u2 - u1); x1 = clip.x; }
				if (y1 < clip.y) { v1 = v1 + (1.f - (y2 - clip.y) / (y2 - y1)) * (v2 - v1); y1 = clip.y; }
				if (x2 > clip.z) { u2 = u1 + ((clip.z - x1) / (x2 - x1)) * (u2 - u1); x2 = clip.z; }
				if (y2 > clip.w) { v2 = v1 + ((clip.w - y1) / (y2 - y1)) * (v2 - v1); y2 = clip.w; }
				if (y1 < y2) {
					ImU32 col = token != kPlain && token < kTokenCount ? kTokenColours[token] : plain;
					if (glyph->Colored) col |= ~IM_COL32_A_MASK;
					dl->PrimReserve(6, 4);
					dl->PrimRectUV(ImVec2(x1, y1), ImVec2(x2, y2), ImVec2(u1, v1), ImVec2(u2, v2), col);
				}
			}
		}
		x += advance;
	}
	dl->PopClipRect();
}
