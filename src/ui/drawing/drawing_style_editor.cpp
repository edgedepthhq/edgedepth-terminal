#include "ui/drawing/drawing_style_editor.h"

#include "core/app_context.h"
#include "core/drawing_manager.h"
#include "education/chart_projection.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"

#include "imgui.h"

#include <cstdio>

namespace drawing {

namespace {

// Swatch palette: design-system hues only. BRAND appears here as a DATA color
// choice; the drawing chrome itself uses neutral white/black on-states (no
// accent cyan anywhere in the drawing UI - James's 2026-08-06 call).
const ImVec4* palette_color(int i) {
    using namespace Theme;
    switch (i) {
        case 0: return &Tokens::TX1;
        case 1: return &Tokens::BRAND;
        case 2: return &Tokens::UP;
        case 3: return &Tokens::DOWN;
        case 4: return &Tokens::WARN;
        case 5: return &Tokens::REST;
        case 6: return &Tokens::ICEBERG_VIOLET;
        case 7: return &Tokens::TX3;
        default: return &Tokens::TX1;
    }
}
constexpr int kPaletteCount = 8;

// 28px icon button reusing the rail's visual language.
bool small_icon_btn(const char* id, UiIcon icon, bool active, const char* tip) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sz = 24.0f;
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
                          Theme::u32(Theme::Tokens::TX1), Theme::Radius::R1);
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
                          Theme::u32(Theme::Tokens::ELEV), Theme::Radius::R1);
    const ImU32 col = active  ? Theme::u32(Theme::Tokens::BASE)
                    : hovered ? Theme::u32(Theme::Tokens::TX1)
                              : Theme::u32(Theme::Tokens::TX2);
    draw_ui_icon(dl, icon, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), 6.0f, col, 1.4f);
    if (hovered && tip) Theme::tooltip("%s", tip);
    return clicked;
}

}  // namespace

void render_style_editor(const AppContext& ctx) {
    DrawingManager& mgr = ctx.drawing_mgr();
    Drawing* d = mgr.find(mgr.selected());
    if (!d) return;

    // Anchor the bar near the top of the chart plot (stable, non-occluding).
    const edu::ChartProjection& proj = edu::chart_projection();
    ImVec2 pos;
    if (proj.valid) {
        pos = ImVec2(proj.plot_min.x + 12.0f, proj.plot_min.y + 12.0f);
    } else {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        pos = ImVec2(vp->Pos.x + LayoutManager::left_reserve + 12.0f,
                     vp->Pos.y + LayoutManager::top_reserve + 60.0f);
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##drawing_style_editor", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ── Color swatches ──────────────────────────────────────────────────
        for (int i = 0; i < kPaletteCount; ++i) {
            if (i) ImGui::SameLine();
            const ImVec4& c = *palette_color(i);
            const uint32_t packed = Theme::u32(c);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            char id[16];
            snprintf(id, sizeof(id), "##sw%d", i);
            if (ImGui::InvisibleButton(id, ImVec2(18.0f, 18.0f)) &&
                d->style.color != packed) {
                mgr.begin_modify(d->id);
                d->style.color = packed;
                d->style.fill = 0;  // re-derive area fills from the new color
                mgr.end_modify(d->id);
            }
            dl->AddRectFilled(p, ImVec2(p.x + 18.0f, p.y + 18.0f), packed,
                              Theme::Radius::R1);
            // White ring with a panel-ground gap so it stays visible around
            // the white swatch too.
            if (d->style.color == packed)
                dl->AddRect(ImVec2(p.x - 2.5f, p.y - 2.5f),
                            ImVec2(p.x + 20.5f, p.y + 20.5f),
                            Theme::u32(Theme::Tokens::TX1), Theme::Radius::R1,
                            0, 1.2f);
        }

        // ── Width + pattern ─────────────────────────────────────────────────
        // Both rows draw stroke previews instead of text labels: numeric
        // labels clipped at the fixed button width ("1.5" -> "1.") and the
        // dash/dot glyph labels clipped too.
        ImGui::SameLine(0.0f, 10.0f);
        const float widths[4] = {1.0f, 1.5f, 2.0f, 3.0f};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            const bool on = d->style.width == widths[i];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            char id[16];
            snprintf(id, sizeof(id), "##w%d", i);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(26.0f, 24.0f));
            const bool hovered = ImGui::IsItemHovered();
            if (on)
                dl->AddRectFilled(p, ImVec2(p.x + 26.0f, p.y + 24.0f),
                                  Theme::u32(Theme::Tokens::TX1),
                                  Theme::Radius::R1);
            else if (hovered)
                dl->AddRectFilled(p, ImVec2(p.x + 26.0f, p.y + 24.0f),
                                  Theme::u32(Theme::Tokens::ELEV),
                                  Theme::Radius::R1);
            const ImU32 col = on      ? Theme::u32(Theme::Tokens::BASE)
                              : hovered ? Theme::u32(Theme::Tokens::TX1)
                                        : Theme::u32(Theme::Tokens::TX2);
            const float cy = p.y + 12.0f;
            dl->AddLine(ImVec2(p.x + 5.0f, cy), ImVec2(p.x + 21.0f, cy), col,
                        widths[i]);
            if (hovered) Theme::tooltip("%g px", widths[i]);
            if (clicked && !on) {
                mgr.begin_modify(d->id);
                d->style.width = widths[i];
                mgr.end_modify(d->id);
            }
        }

        ImGui::SameLine(0.0f, 10.0f);
        const char* ptip[3] = {"Solid", "Dashed", "Dotted"};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(d->style.pattern) == i;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            char id[16];
            snprintf(id, sizeof(id), "##p%d", i);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(32.0f, 24.0f));
            const bool hovered = ImGui::IsItemHovered();
            if (on)
                dl->AddRectFilled(p, ImVec2(p.x + 32.0f, p.y + 24.0f),
                                  Theme::u32(Theme::Tokens::TX1),
                                  Theme::Radius::R1);
            else if (hovered)
                dl->AddRectFilled(p, ImVec2(p.x + 32.0f, p.y + 24.0f),
                                  Theme::u32(Theme::Tokens::ELEV),
                                  Theme::Radius::R1);
            const ImU32 col = on      ? Theme::u32(Theme::Tokens::BASE)
                              : hovered ? Theme::u32(Theme::Tokens::TX1)
                                        : Theme::u32(Theme::Tokens::TX2);
            const float cy = p.y + 12.0f;
            const float x0 = p.x + 5.0f, x1 = p.x + 27.0f;
            if (i == 0) {
                dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), col, 1.5f);
            } else if (i == 1) {
                for (float x = x0; x + 4.0f <= x1 + 0.5f; x += 7.0f)
                    dl->AddLine(ImVec2(x, cy), ImVec2(x + 4.0f, cy), col, 1.5f);
            } else {
                for (float x = x0 + 1.0f; x <= x1; x += 5.0f)
                    dl->AddCircleFilled(ImVec2(x, cy), 1.3f, col);
            }
            if (hovered) Theme::tooltip("%s", ptip[i]);
            if (clicked && !on) {
                mgr.begin_modify(d->id);
                d->style.pattern = static_cast<LinePattern>(i);
                mgr.end_modify(d->id);
            }
        }

        // ── Visibility / lock / delete ──────────────────────────────────────
        ImGui::SameLine(0.0f, 10.0f);
        if (small_icon_btn("##se_eye", d->hidden ? UiIcon::EyeOff : UiIcon::Eye,
                           d->hidden, d->hidden ? "Show" : "Hide")) {
            mgr.begin_modify(d->id);
            d->hidden = !d->hidden;
            mgr.end_modify(d->id);
        }
        ImGui::SameLine();
        if (small_icon_btn("##se_lock", d->locked ? UiIcon::Lock : UiIcon::Unlock,
                           d->locked, d->locked ? "Unlock" : "Lock")) {
            mgr.begin_modify(d->id);
            d->locked = !d->locked;
            mgr.end_modify(d->id);
        }
        ImGui::SameLine();
        if (small_icon_btn("##se_trash", UiIcon::Trash, false, "Delete")) {
            mgr.remove(d->id);
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
            return;
        }
        ImGui::SameLine();
        if (small_icon_btn("##se_close", UiIcon::Close, false, "Deselect"))
            mgr.select(0);

        // ── Per-tool extras ─────────────────────────────────────────────────
        if (d->tool == Tool::Fib) {
            ImGui::PushFont(Theme::Fonts::mono_sm());
            for (int i = 0; i < kFibLevelCount; ++i) {
                if (i % 6 != 0) ImGui::SameLine();
                bool on = (d->fib_mask & (1u << i)) != 0;
                char lbl[24];
                snprintf(lbl, sizeof(lbl), "%g##fib%d", kFibLevels[i], i);
                if (ImGui::Checkbox(lbl, &on)) {
                    mgr.begin_modify(d->id);
                    d->fib_mask =
                        on ? (d->fib_mask | (1u << i)) : (d->fib_mask & ~(1u << i));
                    mgr.end_modify(d->id);
                }
            }
            ImGui::PopFont();
        } else if (d->tool == Tool::Text) {
            ImGui::PushFont(Theme::Fonts::mono_sm());
            const float sizes[3] = {12.0f, 14.0f, 18.0f};
            const char* slbl[3] = {"S", "M", "L"};
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine();
                const bool on = d->font_size == sizes[i];
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      on ? Theme::Tokens::TX1
                                         : ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      on ? Theme::Tokens::TX1
                                         : Theme::Tokens::ELEV);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      on ? Theme::Tokens::TX1
                                         : Theme::Tokens::ELEV);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      on ? Theme::Tokens::BASE
                                         : Theme::Tokens::TX2);
                char id[12];
                snprintf(id, sizeof(id), "%s##fs%d", slbl[i], i);
                if (ImGui::Button(id, ImVec2(26.0f, 22.0f)) && !on) {
                    mgr.begin_modify(d->id);
                    d->font_size = sizes[i];
                    mgr.end_modify(d->id);
                }
                ImGui::PopStyleColor(4);
            }
            ImGui::PopFont();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    // Selected-drawing edits double as the new-drawing default.
    if (Drawing* still = mgr.find(mgr.selected()))
        mgr.default_style() = still->style;
}

}  // namespace drawing
