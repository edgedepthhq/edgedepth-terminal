#include "ui/drawing/drawing_toolbar.h"

#include "core/drawing_manager.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"

#include "imgui.h"

#include <cstdio>

namespace drawing {

namespace {

constexpr float kRailW          = 40.0f;
constexpr float kRailCollapsedW = 14.0f;
constexpr float kBtnSize        = 28.0f;
constexpr float kBtnGap         = 2.0f;
constexpr float kIconHalf       = 7.0f;

// Rail order — grouped like the reference toolbar; nullptr-terminated groups
// separated by hairlines.
constexpr Tool kGroupPointer[] = {Tool::Cursor};
constexpr Tool kGroupLines[]   = {Tool::Trendline, Tool::Arrow, Tool::Ray,
                                  Tool::ExtendedLine, Tool::HLine, Tool::HRay,
                                  Tool::VLine, Tool::CrossLine};
constexpr Tool kGroupShapes[]  = {Tool::Rectangle, Tool::Channel, Tool::Polyline,
                                  Tool::Brush};
constexpr Tool kGroupAnalysis[] = {Tool::Fib, Tool::LongPosition,
                                   Tool::ShortPosition};
constexpr Tool kGroupNotes[]   = {Tool::Text, Tool::Measure, Tool::PriceRange,
                                  Tool::DateRange};

struct ToolGroup { const Tool* tools; int count; };
constexpr ToolGroup kGroups[] = {
    {kGroupPointer,  1},
    {kGroupLines,    8},
    {kGroupShapes,   4},
    {kGroupAnalysis, 3},
    {kGroupNotes,    4},
};

// One 28px icon button, centered in the rail column. Returns true on click.
// `active` renders the neutral on-state (white chip + near-black glyph) -
// James's 2026-08-06 call: no accent cyan anywhere in the drawing chrome.
bool icon_button(const char* id, bool active, const char* tip,
                 void (*draw)(ImDrawList*, ImVec2, float, ImU32, float)) {
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - kBtnSize) * 0.5f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(kBtnSize, kBtnSize));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + kBtnSize * 0.5f, p.y + kBtnSize * 0.5f);
    if (active) {
        dl->AddRectFilled(p, ImVec2(p.x + kBtnSize, p.y + kBtnSize),
                          Theme::u32(Theme::Tokens::TX1), Theme::Radius::R2);
    } else if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + kBtnSize, p.y + kBtnSize),
                          Theme::u32(Theme::Tokens::ELEV), Theme::Radius::R2);
    }
    const ImU32 col = active  ? Theme::u32(Theme::Tokens::BASE)
                    : hovered ? Theme::u32(Theme::Tokens::TX1)
                              : Theme::u32(Theme::Tokens::TX2);
    draw(dl, c, kIconHalf, col, 1.5f);

    if (hovered && tip && tip[0] != '\0') Theme::tooltip("%s", tip);
    return clicked;
}

// Adapters so icon_button can take either glyph table through one signature.
Tool   g_tool_to_draw = Tool::Cursor;
UiIcon g_icon_to_draw = UiIcon::Close;
void draw_tool_adapter(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    draw_tool_icon(dl, g_tool_to_draw, c, s, col, th);
}
void draw_icon_adapter(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float th) {
    draw_ui_icon(dl, g_icon_to_draw, c, s, col, th);
}

bool tool_button(DrawingManager& mgr, Tool t) {
    g_tool_to_draw = t;
    char id[24];
    snprintf(id, sizeof(id), "##dtool_%d", static_cast<int>(t));
    const bool active = mgr.armed() == t;
    if (icon_button(id, active, tool_name(t), draw_tool_adapter)) {
        // Re-clicking the armed tool disarms back to Cursor.
        mgr.arm(active ? Tool::Cursor : t);
        return true;
    }
    return false;
}

bool ui_button(const char* id, UiIcon icon, bool active, const char* tip) {
    g_icon_to_draw = icon;
    return icon_button(id, active, tip, draw_icon_adapter);
}

void group_separator() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, 5.0f));
    dl->AddLine(ImVec2(p.x + 7.0f, p.y + 2.5f), ImVec2(p.x + w - 7.0f, p.y + 2.5f),
                Theme::u32(Theme::Tokens::BD1), 1.0f);
}

}  // namespace

float rail_width(const DrawingManager& mgr) {
    return mgr.rail_collapsed() ? kRailCollapsedW : kRailW;
}

void render_chart_rail(DrawingManager& mgr, float height) {
    const float w = rail_width(mgr);
    if (height <= 0.0f) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Tokens::PANEL);
    ImGui::BeginChild("##drawing_rail", ImVec2(w, height), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

    // Right-edge hairline — the only separation per the design discipline.
    const ImVec2 p0 = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p0.x + w - 0.5f, p0.y), ImVec2(p0.x + w - 0.5f, p0.y + height),
                Theme::u32(Theme::Tokens::BD1), 1.0f);

    ImGui::Dummy(ImVec2(1.0f, 4.0f));  // top inset (child has zero padding)

    if (mgr.rail_collapsed()) {
        // Slim strip: a single expand chevron at the top.
        if (ui_button("##drail_expand", UiIcon::ChevronRight, false,
                      "Show drawing tools"))
            mgr.set_rail_collapsed(false);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // Bottom utility cluster is pinned; tools scroll (wheel) if the chart is
    // short. 4 buttons + separator.
    const float cluster_h = 4.0f * (kBtnSize + kBtnGap) + 7.0f;
    ImGui::BeginChild("##drail_tools", ImVec2(w, height - cluster_h - 12.0f),
                      false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kBtnGap));
    for (size_t gi = 0; gi < sizeof(kGroups) / sizeof(kGroups[0]); ++gi) {
        if (gi > 0) group_separator();
        for (int ti = 0; ti < kGroups[gi].count; ++ti)
            tool_button(mgr, kGroups[gi].tools[ti]);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // ── Utility cluster ──────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kBtnGap));
    group_separator();
    if (ui_button("##drail_magnet", UiIcon::Magnet, mgr.magnet(),
                  "Magnet (snap to OHLC)"))
        mgr.set_magnet(!mgr.magnet());
    if (ui_button("##drail_eye", mgr.hidden_all() ? UiIcon::EyeOff : UiIcon::Eye,
                  mgr.hidden_all(), mgr.hidden_all() ? "Show drawings"
                                                     : "Hide all drawings"))
        mgr.set_hidden_all(!mgr.hidden_all());
    if (ui_button("##drail_trash", UiIcon::Trash, false, "Remove all drawings"))
        ImGui::OpenPopup("##drail_clear_confirm");
    if (ui_button("##drail_collapse", UiIcon::ChevronLeft, false,
                  "Hide drawing toolbar"))
        mgr.set_rail_collapsed(true);
    ImGui::PopStyleVar();

    // Remove-all confirm — floating chrome: R3 + PANEL + BD2 border.
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    if (ImGui::BeginPopup("##drail_clear_confirm")) {
        ImGui::PushFont(Theme::Fonts::ui());
        ImGui::TextColored(Theme::Tokens::TX1, "Remove all drawings?");
        ImGui::Spacing();
        if (ImGui::Button("Remove", ImVec2(74.0f, 0.0f))) {
            mgr.clear_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(74.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void render_tool_menu_rows(DrawingManager& mgr) {
    ImGui::PushFont(Theme::Fonts::ui());
    for (size_t gi = 0; gi < sizeof(kGroups) / sizeof(kGroups[0]); ++gi) {
        if (gi > 0) {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(1.0f, 5.0f));
            pdl->AddLine(ImVec2(sp.x + 8.0f, sp.y + 2.5f),
                         ImVec2(sp.x + 192.0f, sp.y + 2.5f),
                         Theme::u32(Theme::Tokens::BD1), 1.0f);
        }
        for (int ti = 0; ti < kGroups[gi].count; ++ti) {
            const Tool t = kGroups[gi].tools[ti];
            const bool active = mgr.armed() == t;
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            char id[24];
            snprintf(id, sizeof(id), "##dd_%d", static_cast<int>(t));
            const bool row = ImGui::InvisibleButton(id, ImVec2(200.0f, 26.0f));
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            if (hov)
                pdl->AddRectFilled(rp, ImVec2(rp.x + 200.0f, rp.y + 26.0f),
                                   Theme::u32(Theme::Tokens::ELEV));
            const ImU32 icol = (active || hov)
                                   ? Theme::u32(Theme::Tokens::TX1)
                                   : Theme::u32(Theme::Tokens::TX2);
            draw_tool_icon(pdl, t, ImVec2(rp.x + 18.0f, rp.y + 13.0f), 6.5f,
                           icol, 1.4f);
            pdl->AddText(ImVec2(rp.x + 36.0f, rp.y + 5.0f),
                         Theme::u32(Theme::Tokens::TX1), tool_name(t));
            if (active)
                pdl->AddCircleFilled(ImVec2(rp.x + 189.0f, rp.y + 13.0f), 2.5f,
                                     Theme::u32(Theme::Tokens::TX1));
            if (row) {
                mgr.arm(active ? Tool::Cursor : t);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::PopFont();
}

void render_topbar_button(DrawingManager& mgr) {
    // Transparent 30px icon button (AppShell ico_btn look) + tool dropdown.
    // The caller (AppShell topbar cluster) has already set the cursor pos.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::ACTIVE);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R1);
    const bool clicked = ImGui::Button("##tb_draw", ImVec2(30.0f, 30.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool armed = mgr.armed() != Tool::Cursor;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (armed)
        dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::TX1), Theme::Radius::R1);
    draw_ui_icon(dl, UiIcon::Pencil,
                 ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 7.0f,
                 armed ? Theme::u32(Theme::Tokens::BASE)
                       : Theme::u32(Theme::Tokens::TX2),
                 1.4f);
    if (ImGui::IsItemHovered()) Theme::tooltip("Drawing tools");
    if (clicked) ImGui::OpenPopup("##tb_draw_tools");

    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    if (ImGui::BeginPopup("##tb_draw_tools")) {
        render_tool_menu_rows(mgr);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

}  // namespace drawing
