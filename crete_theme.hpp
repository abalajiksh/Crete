#pragma once
// ============================================================================
// crete_theme.hpp — Modernist theme + reusable widgets for the crête GUI.
//
// Flat, architectural, near-mono red-on-white (or ink-on-dark). Zero corner
// radius, strong rules, flush-left labels, Archivo type. Derived from the
// Modernist design-system tokens (styles.css).
//
//   #include "crete_theme.hpp"
//   theme::apply(app.dark_mode);   // after ImGui::CreateContext()
//   theme::load_fonts();           // replaces the old load_fonts()
//
// Fonts: drop Archivo TTFs under  fonts/Archivo/  next to the binary:
//   fonts/Archivo/Archivo-Regular.ttf
//   fonts/Archivo/Archivo-SemiBold.ttf
//   fonts/Archivo/Archivo-ExtraBold.ttf
// (SIL Open Font License — https://fonts.google.com/specimen/Archivo)
// Falls back to the bundled Sora, then the ImGui default, if absent.
// ============================================================================

#include "imgui.h"
#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

namespace theme {

// ── Palette (all Modernist tokens as ImVec4) ────────────────────────────────
struct Palette {
    ImVec4 bg, surface, panel, field, titlebar;
    ImVec4 ink, muted, faint, line, lineStrong;
    ImVec4 accent, accentHover, accentActive, accentSoft, accentInk, onAccent, sel;
};

// hex 0xRRGGBB -> ImVec4
inline ImVec4 hx(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

inline const Palette& light() {
    static const Palette p{
        /*bg*/         hx(0xf3f2f2), /*surface*/ hx(0xeae9e9), /*panel*/ hx(0xf8f4f4),
        /*field*/      hx(0xffffff), /*titlebar*/hx(0xe2e0e0),
        /*ink*/        hx(0x201e1d), /*muted*/   hx(0x201e1d, 0.55f),
        /*faint*/      hx(0x201e1d, 0.38f), /*line*/ hx(0x201e1d, 0.16f),
        /*lineStrong*/ hx(0x201e1d, 0.42f),
        /*accent*/     hx(0xec3013), /*accentHover*/ hx(0xdd2b0f), /*accentActive*/ hx(0xae1800),
        /*accentSoft*/ hx(0xffe0d9), /*accentInk*/ hx(0x7c1405),
        /*onAccent*/   hx(0xfbf4f2), /*sel*/ hx(0xfff2ef)
    };
    return p;
}

inline const Palette& dark() {
    static const Palette p{
        /*bg*/         hx(0x1a1715), /*surface*/ hx(0x221f1d), /*panel*/ hx(0x2a2624),
        /*field*/      hx(0x17140f), /*titlebar*/hx(0x100e0d),
        /*ink*/        hx(0xf3f0ee), /*muted*/   hx(0xf3f0ee, 0.55f),
        /*faint*/      hx(0xf3f0ee, 0.32f), /*line*/ hx(0xf3f0ee, 0.15f),
        /*lineStrong*/ hx(0xf3f0ee, 0.34f),
        /*accent*/     hx(0xff563c), /*accentHover*/ hx(0xff7a63), /*accentActive*/ hx(0xdd2b0f),
        /*accentSoft*/ ImVec4(1.0f, 0.34f, 0.235f, 0.16f), /*accentInk*/ hx(0xffb4a4),
        /*onAccent*/   hx(0x2a120c), /*sel*/ ImVec4(1.0f, 0.34f, 0.235f, 0.12f)
    };
    return p;
}

// Current palette (set by apply()). Widgets read through this.
inline const Palette*& current() { static const Palette* p = &light(); return p; }
inline const Palette& P() { return *current(); }
inline ImU32 u32(const ImVec4& c) { return ImGui::GetColorU32(c); }

// ── Fonts ───────────────────────────────────────────────────────────────────
inline ImFont* g_body  = nullptr;  // Archivo Regular  16
inline ImFont* g_head  = nullptr;  // Archivo ExtraBold 18  (labels, table DR)
inline ImFont* g_brand = nullptr;  // Archivo ExtraBold 26  (wordmark)
inline ImFont* g_big   = nullptr;  // Archivo ExtraBold 42  (hero DR)

inline ImFont* try_load(ImGuiIO& io, const std::string& base,
                        const char* const* candidates, int n, float size,
                        const ImWchar* ranges) {
    for (int i = 0; i < n; ++i) {
        std::string full = base + candidates[i];
        std::ifstream test(full, std::ios::binary);
        if (test.good()) {
            test.close();
            ImFont* f = io.Fonts->AddFontFromFileTTF(full.c_str(), size, nullptr, ranges);
            if (f) return f;
        }
    }
    return nullptr;
}

inline void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x22FF, 0 };

    std::string base;
    if (char* b = SDL_GetBasePath()) { base = b; SDL_free(b); }

    const char* reg[]  = { "fonts/Archivo/Archivo-Regular.ttf",
                           "fonts/Archivo/static/Archivo-Regular.ttf",
                           "fonts/Sora/static/Sora-Regular.ttf" };
    const char* bold[] = { "fonts/Archivo/Archivo-ExtraBold.ttf",
                           "fonts/Archivo/static/Archivo-ExtraBold.ttf",
                           "fonts/Archivo/Archivo-Bold.ttf",
                           "fonts/Sora/static/Sora-Bold.ttf" };

    g_body  = try_load(io, base, reg,  3, 16.0f, ranges);
    g_head  = try_load(io, base, bold, 4, 18.0f, ranges);
    g_brand = try_load(io, base, bold, 4, 26.0f, ranges);
    g_big   = try_load(io, base, bold, 4, 42.0f, ranges);

    if (!g_body) { ImFontConfig cfg; cfg.SizePixels = 16.0f; g_body = io.Fonts->AddFontDefault(&cfg); }
    if (!g_head)  g_head  = g_body;
    if (!g_brand) g_brand = g_head;
    if (!g_big)   g_big   = g_brand;
    io.FontDefault = g_body;
}

// ── Apply the theme to ImGui's global style ─────────────────────────────────
inline void apply(bool dark_mode) {
    current() = dark_mode ? &dark() : &light();
    const Palette& p = P();

    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();  // baseline; we overwrite the roles we use

    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
    s.GrabRounding = s.ScrollbarRounding = s.TabRounding = s.PopupRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;   // Modernist inputs are ruled boxes
    s.ChildBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    s.WindowPadding = ImVec2(20, 16);
    s.FramePadding  = ImVec2(11, 7);
    s.ItemSpacing   = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(7, 6);
    s.CellPadding   = ImVec2(10, 8);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize   = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = p.bg;
    c[ImGuiCol_ChildBg]             = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]             = p.surface;
    c[ImGuiCol_Text]                = p.ink;
    c[ImGuiCol_TextDisabled]        = p.muted;
    c[ImGuiCol_Border]              = p.line;
    c[ImGuiCol_Separator]           = p.lineStrong;
    c[ImGuiCol_SeparatorHovered]    = p.accent;
    c[ImGuiCol_SeparatorActive]     = p.accent;
    c[ImGuiCol_FrameBg]             = p.field;
    c[ImGuiCol_FrameBgHovered]      = p.accentSoft;
    c[ImGuiCol_FrameBgActive]       = p.accentSoft;
    c[ImGuiCol_Button]              = p.surface;
    c[ImGuiCol_ButtonHovered]       = p.accentSoft;
    c[ImGuiCol_ButtonActive]        = p.accentSoft;
    c[ImGuiCol_Header]              = p.sel;
    c[ImGuiCol_HeaderHovered]       = p.accentSoft;
    c[ImGuiCol_HeaderActive]        = p.sel;
    c[ImGuiCol_CheckMark]           = p.accent;
    c[ImGuiCol_SliderGrab]          = p.accent;
    c[ImGuiCol_SliderGrabActive]    = p.accentActive;
    c[ImGuiCol_PlotHistogram]       = p.accent;   // ProgressBar fill
    c[ImGuiCol_PlotHistogramHovered]= p.accentHover;
    c[ImGuiCol_TableHeaderBg]       = p.panel;
    c[ImGuiCol_TableBorderStrong]   = p.lineStrong;
    c[ImGuiCol_TableBorderLight]    = p.line;
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = p.faint;
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]       = p.line;
    c[ImGuiCol_ScrollbarGrabHovered]= p.lineStrong;
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_NavHighlight]        = p.accent;
    c[ImGuiCol_DragDropTarget]      = p.accent;
    c[ImGuiCol_ChildBg]             = ImVec4(0, 0, 0, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Widgets
// ═══════════════════════════════════════════════════════════════════════════

// A full-width horizontal rule of `thick` px at the current cursor Y.
inline void Rule(float thick = 2.0f, float pad_y = 6.0f) {
    ImVec2 pmin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    pmin.y += pad_y;
    ImGui::GetWindowDrawList()->AddRectFilled(
        pmin, ImVec2(pmin.x + w, pmin.y + thick), u32(P().lineStrong));
    ImGui::Dummy(ImVec2(w, thick + pad_y * 2));
}

// Small uppercase tracked label (Modernist "kicker").
inline void Kicker(const char* text, const ImVec4* col = nullptr) {
    ImGui::PushFont(g_body);
    ImGui::PushStyleColor(ImGuiCol_Text, col ? *col : P().muted);
    // fake letter-spacing with a scaled small font
    float os = ImGui::GetFont()->Scale;
    ImGui::SetWindowFontScale(0.72f);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(os);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Section header inside the detail panel: kicker + 2px rule.
inline void SectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    Kicker(text);
    Rule(2.0f, 3.0f);
}

// A label / right-aligned value row with a hairline under it.
inline void KeyValue(const char* label, const char* value,
                     const ImVec4* value_col = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, P().muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    float w = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    ImGui::PushFont(g_head);
    ImGui::PushStyleColor(ImGuiCol_Text, value_col ? *value_col : P().ink);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 1));
}

// The DR meter-bar table cell: big "DRnn" + a fill bar (accent when < DR8).
inline void DrMeterBar(int dr, float bar_w = 54.0f) {
    ImGui::PushFont(g_head);
    ImGui::Text("DR%d", dr);
    ImGui::PopFont();
    ImGui::SameLine();
    float frac = std::min(dr / 18.0f, 1.0f);
    float h = 5.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.y += (ImGui::GetTextLineHeight() - h) * 0.5f + 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 fill = u32(dr < 8 ? P().accent : P().ink);
    dl->AddRectFilled(pos, ImVec2(pos.x + bar_w, pos.y + h), u32(P().line));
    dl->AddRectFilled(pos, ImVec2(pos.x + bar_w * frac, pos.y + h), fill);
    ImGui::Dummy(ImVec2(bar_w, ImGui::GetTextLineHeight()));
}

// Flush segmented control (buttons butted together; active = accent fill).
// `disabled_mask` bit i disables option i. Returns true if selection changed.
inline bool Segmented(const char* id, const char* const* opts, int n, int* cur,
                      unsigned disabled_mask = 0) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11, 6));
    for (int i = 0; i < n; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (*cur == i);
        bool dis = (disabled_mask >> i) & 1u;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button,        P().accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P().accentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  P().accentActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          P().onAccent);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P().accentSoft);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  P().accentSoft);
            ImGui::PushStyleColor(ImGuiCol_Text,          P().muted);
        }
        if (dis) ImGui::BeginDisabled();
        if (ImGui::Button(opts[i]) && !sel) { *cur = i; changed = true; }
        if (dis) ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

// A solid accent primary button (flush-left label per Modernist).
inline bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        P().accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P().accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  P().accentActive);
    ImGui::PushStyleColor(ImGuiCol_Text,          P().onAccent);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// A ghost (accent-text, no fill) button — used in the header.
inline bool GhostButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P().accentSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  P().accentSoft);
    ImGui::PushStyleColor(ImGuiCol_Text,          P().accent);
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return r;
}

// A small tinted tag with explicit fill / text colors.
inline void Tag(const char* text, const ImVec4& fill, const ImVec4& txt) {
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 pad(9, 4);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad.x * 2, p.y + sz.y + pad.y * 2),
                      u32(fill));
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), u32(txt), text);
    ImGui::Dummy(ImVec2(sz.x + pad.x * 2, sz.y + pad.y * 2));
}

// A small tinted tag (accentSoft fill, accentInk text).
inline void Tag(const char* text) { Tag(text, P().accentSoft, P().accentInk); }

} // namespace theme
