#include "indicator_manager.h"
#include "ui/chart_widget.h"
#include "ui/custom_implot.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cstdio>

namespace Indicators {

    void IndicatorManager::add_indicator(std::unique_ptr<IndicatorBase> indicator) {
        indicators.emplace_back(std::move(indicator));
        // Stacked by default (James, 2026-07-04): every indicator gets its
        // own stacked plot. Right-click → Unstack tabs it into the shared
        // active-tab slot (the previous default). Drag-to-merge tab GROUPS
        // is the S4/F1 follow-up — needs a multi-tab-group pane model.
        pinned_.push_back(true);
        active_tab_ = static_cast<int>(indicators.size()) - 1;  // focus the new tab
        pane_hidden_ = false;                                   // re-show on add
    }

    void IndicatorManager::remove_indicator(size_t index) {
        if (index < indicators.size()) {
            indicators.erase(indicators.begin() + index);
            if (index < pinned_.size()) pinned_.erase(pinned_.begin() + index);
        }
    }

    // Stacked plots this frame: every pinned indicator, plus the active tab if
    // it isn't already pinned. Active goes last so it owns the time axis.
    std::vector<int> IndicatorManager::stacked_plot_indices() const {
        std::vector<int> out;
        out.reserve(indicators.size());
        for (int i = 0; i < static_cast<int>(indicators.size()); ++i) {
            if (i < static_cast<int>(pinned_.size()) && pinned_[i] && i != active_tab_)
                out.push_back(i);
        }
        if (active_tab_ >= 0 && active_tab_ < static_cast<int>(indicators.size()))
            out.push_back(active_tab_);
        return out;
    }

    // 5px hit strip between stacked plots for the height drag.
    static constexpr float kIndiSplitterH = 5.0f;

    float IndicatorManager::pane_height() const {
        if (indicators.empty() || pane_hidden_) return 0.0f;
        if (collapsed_) return Theme::Layout::PANEL_HEADER_H;
        // Per-indicator heights (drag-resizable) instead of a fixed slice
        // per plot; splitter strips sit between stacked plots.
        const std::vector<int> idx = stacked_plot_indices();
        float h = Theme::Layout::PANEL_HEADER_H;
        for (int i : idx) h += indicators[i]->get_height_pixels();
        if (idx.size() > 1)
            h += kIndiSplitterH * static_cast<float>(idx.size() - 1);
        if (idx.empty())
            h += Theme::Layout::INDI_PANE_H - Theme::Layout::PANEL_HEADER_H;
        return h;
    }

    bool IndicatorManager::pane_expanded() const {
        return !indicators.empty() && !pane_hidden_ && !collapsed_;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Tabbed indicator pane (.indi-pane)
    //   31px header: tab pills · live value (right) · collapse · close
    //   below: single ImPlot of the active indicator, x-linked to the chart
    // ═══════════════════════════════════════════════════════════════════════

    void IndicatorManager::render_tabbed(double x_min, double x_max,
                                         void* chart_widget,
                                         void* crosshair_state_ptr)
    {
        if (indicators.empty() || pane_hidden_) return;

        auto* chart = static_cast<ChartWidget*>(chart_widget);
        auto* crosshair_state = static_cast<ChartWidget::CrosshairState*>(crosshair_state_ptr);

        if (active_tab_ >= static_cast<int>(indicators.size()))
            active_tab_ = static_cast<int>(indicators.size()) - 1;
        if (active_tab_ < 0) active_tab_ = 0;
        IndicatorBase* active = indicators[active_tab_].get();

        const float header_h = Theme::Layout::PANEL_HEADER_H;
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const ImVec2 h0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ── Header strip — ELEV fill, BD2 top hairline, BD1 bottom ──
        dl->AddRectFilled(h0, ImVec2(h0.x + avail_w, h0.y + header_h),
                          Theme::u32(Theme::Tokens::ELEV));
        dl->AddLine(ImVec2(h0.x, h0.y + 0.5f),
                    ImVec2(h0.x + avail_w, h0.y + 0.5f), Theme::u32(Theme::Tokens::BD2));
        dl->AddLine(ImVec2(h0.x, h0.y + header_h - 0.5f),
                    ImVec2(h0.x + avail_w, h0.y + header_h - 0.5f), Theme::u32(Theme::Tokens::BD1));

        int pending_delete = -1;

        // ── Tab pills (left) — small tabs, active = brand-soft / brand text ──
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 2.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(h0.x + 8.0f, h0.y + (header_h - 21.0f) * 0.5f));

        for (int i = 0; i < static_cast<int>(indicators.size()); ++i) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            const bool on = (i == active_tab_);
            const bool pin = (i < static_cast<int>(pinned_.size()) && pinned_[i]);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button,        Theme::Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Tokens::BRAND_SOFT);
                ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Tokens::BRAND);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      pin ? Theme::Tokens::TX2 : Theme::Tokens::TX3);
            }
            // Pinned tabs get a leading dot so stacked-but-inactive ones read as "kept"
            char tab_label[80];
            snprintf(tab_label, sizeof(tab_label), "%s%s",
                     pin ? "\xe2\x80\xa2 " : "", indicators[i]->get_name());
            if (ImGui::SmallButton(tab_label)) {
                active_tab_ = i;
                collapsed_ = false;
            }
            // Right-click → tab context menu (F2 pilot): stack/unstack,
            // per-indicator Settings… (when declared), remove. Replaces the
            // old immediate pin toggle so settings have a discoverable home.
            if (ImGui::BeginPopupContextItem("indi_tab_ctx")) {
                if (ImGui::MenuItem(pin ? "Unstack" : "Stack")) {
                    if (i < static_cast<int>(pinned_.size())) pinned_[i] = !pinned_[i];
                }
                if (indicators[i]->has_settings() &&
                    ImGui::MenuItem("Settings...")) {
                    settings_open_idx_ = i;
                    settings_popup_pending_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove")) pending_delete = i;
                ImGui::EndPopup();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) ||
                (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl)) {
                pending_delete = i;
            }
            if (ImGui::IsItemHovered())
                Theme::tooltip("%s\nright-click: menu · middle-click: remove",
                                  indicators[i]->get_name());
            ImGui::PopStyleColor(4);
            ImGui::PopID();
        }
        ImGui::PopStyleVar(2);

        // ── Right cluster: live value · collapse · close ──
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::HOVER);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Theme::Tokens::ACTIVE);
            ImGui::PushStyleColor(ImGuiCol_Text,          Theme::Tokens::TX3);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 2.5f));

            const float btn_y = h0.y + (header_h - 21.0f) * 0.5f;
            float rx = h0.x + avail_w - 8.0f;

            // close (hides the pane; re-adding an indicator re-shows it)
            rx -= ImGui::CalcTextSize("x").x + 10.0f;
            ImGui::SetCursorScreenPos(ImVec2(rx, btn_y));
            if (ImGui::SmallButton("x##indi_close")) pane_hidden_ = true;
            if (ImGui::IsItemHovered()) Theme::tooltip("Close pane");

            // collapse / expand chevron
            const char* chev = collapsed_ ? "^##indi_col" : "v##indi_col";
            rx -= ImGui::CalcTextSize("v").x + 14.0f;
            ImGui::SetCursorScreenPos(ImVec2(rx, btn_y));
            if (ImGui::SmallButton(chev)) collapsed_ = !collapsed_;
            if (ImGui::IsItemHovered())
                Theme::tooltip(collapsed_ ? "Expand" : "Collapse");

            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);

            // live value of the active indicator — mono; explicit tag color
            // (SPEC §0.2 — e.g. Toxicity regime) wins over direction coloring
            double latest = 0.0;
            if (active && active->get_latest_value(latest)) {
                char vbuf[32];
                active->format_latest_value(latest, vbuf, sizeof(vbuf));
                ImU32 vcol;
                if (ImU32 tag_col = 0; active->get_latest_tag_color(tag_col)) {
                    vcol = tag_col;
                } else {
                    const int dir = active->get_latest_direction();
                    const ImVec4& vc = dir > 0 ? Theme::Tokens::UP
                                     : dir < 0 ? Theme::Tokens::DOWN
                                     :           Theme::Tokens::TX1;
                    vcol = Theme::u32(vc);
                }
                ImGui::PushFont(Theme::Fonts::mono_sm());
                const float tw = ImGui::CalcTextSize(vbuf).x;
                dl->AddText(ImVec2(rx - tw - 12.0f,
                                   h0.y + (header_h - ImGui::GetFontSize()) * 0.5f),
                            vcol, vbuf);
                ImGui::PopFont();
            }
        }

        // ── F2 pilot: per-indicator settings popover ──
        if (settings_open_idx_ >= 0) {
            if (settings_open_idx_ < static_cast<int>(indicators.size()) &&
                indicators[settings_open_idx_]->has_settings()) {
                if (settings_popup_pending_) {
                    ImGui::OpenPopup("indi_settings_popover");
                    settings_popup_pending_ = false;
                }
                if (ImGui::BeginPopup("indi_settings_popover")) {
                    ImGui::PushFont(Theme::Fonts::label());
                    ImGui::TextDisabled("%s", indicators[settings_open_idx_]->get_name());
                    ImGui::PopFont();
                    ImGui::Separator();
                    indicators[settings_open_idx_]->render_settings();
                    ImGui::EndPopup();
                } else {
                    settings_open_idx_ = -1;  // dismissed by outside click
                }
            } else {
                settings_open_idx_ = -1;      // indicator removed under us
                settings_popup_pending_ = false;
            }
        }

        // advance layout past the header
        ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + header_h));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        if (pending_delete >= 0) {
            indicators.erase(indicators.begin() + pending_delete);
            if (pending_delete < static_cast<int>(pinned_.size()))
                pinned_.erase(pinned_.begin() + pending_delete);
            if (indicators.empty()) return;
            if (active_tab_ >= static_cast<int>(indicators.size()))
                active_tab_ = static_cast<int>(indicators.size()) - 1;
        }

        if (collapsed_) return;

        // ── Stacked indicator plots — one per pinned indicator + the active
        //    tab; the last one owns the time axis. All x-linked to the chart.
        //    Heights are per-indicator (drag the splitter strip between two
        //    plots to resize the one above; the chart reallocates next frame
        //    via pane_height()). ──
        const std::vector<int> plot_idx = stacked_plot_indices();
        const float total_h = ImGui::GetContentRegionAvail().y;
        if (total_h < 30.0f || plot_idx.empty()) return;

        const int n = static_cast<int>(plot_idx.size());

        for (int slot = 0; slot < n; ++slot) {
            IndicatorBase* ind = indicators[plot_idx[slot]].get();
            const bool is_last = (slot == n - 1);
            const float plot_h = is_last ? ImGui::GetContentRegionAvail().y
                                         : ind->get_height_pixels();
            if (plot_h < 20.0f) break;

            char plot_id[48];
            snprintf(plot_id, sizeof(plot_id), "##indi_plot_%d", plot_idx[slot]);

            ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10, 6));
            if (ImPlot::BeginPlot(plot_id, ImVec2(-1, plot_h),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                // Only the bottom plot shows x tick labels (others share the axis)
                const ImPlotAxisFlags x_flags = is_last ? ImPlotAxisFlags_None
                                                        : ImPlotAxisFlags_NoTickLabels;
                ImPlot::SetupAxis(ImAxis_X1, nullptr, x_flags);
                ImPlot::SetupAxis(ImAxis_Y1, nullptr,
                                  ImPlotAxisFlags_Opposite | ImPlotAxisFlags_AutoFit);

                double y_min, y_max;
                ind->get_y_limits(x_min, x_max, y_min, y_max);
                ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImGuiCond_Always);

                if (is_last) chart->setup_time_axis_ticks(x_min, x_max);

                auto fmt_cb = ind->get_y_formatter();
                if (fmt_cb) ImPlot::SetupAxisFormat(ImAxis_Y1, fmt_cb, nullptr);
                else        ImPlot::SetupAxisFormat(ImAxis_Y1, ind->get_y_format());

                ImPlot::SetupAxisLinks(ImAxis_X1, nullptr, nullptr);
                ind->render_content(x_min, x_max);

                // Indicator name tag, top-left of each stacked plot
                {
                    const ImVec2 pp = ImPlot::GetPlotPos();
                    ImGui::PushFont(Theme::Fonts::label());
                    dl->AddText(ImVec2(pp.x + 6.0f, pp.y + 4.0f),
                                Theme::u32(Theme::Tokens::TX3), ind->get_name());
                    ImGui::PopFont();
                }

                // Latest-value axis tag — explicit tag color (regime etc.)
                // wins; otherwise direction-colored as before
                {
                    double latest_val = 0.0;
                    if (ind->get_latest_value(latest_val)) {
                        ImU32 tag_col;
                        if (ImU32 c = 0; ind->get_latest_tag_color(c)) {
                            tag_col = c;
                        } else {
                            const int dir = ind->get_latest_direction();
                            tag_col =
                                dir > 0 ? Theme::get_buy_color_u32(255)
                              : dir < 0 ? Theme::get_sell_color_u32(255)
                              :           Theme::u32(Theme::Tokens::TX3);
                        }
                        char tag_buf[32];
                        ind->format_latest_value(latest_val, tag_buf, sizeof(tag_buf));
                        CustomImPlot::DrawAxisValueTag(latest_val, tag_buf, tag_col, true);
                    }
                }

                const ImVec2 plot_pos = ImPlot::GetPlotPos();
                const ImVec2 plot_size = ImPlot::GetPlotSize();

                if (ImPlot::IsPlotHovered()) {
                    crosshair_state->is_active = true;
                    crosshair_state->plot_pos = ImPlot::GetPlotMousePos();
                    crosshair_state->chart_hovered = false;
                    crosshair_state->indicator_hovered = true;
                    crosshair_state->indicator_y_formatter = ind->get_y_formatter();
                    crosshair_state->hovered_plot_min = plot_pos;
                    crosshair_state->hovered_plot_max = ImVec2(plot_pos.x + plot_size.x,
                                                               plot_pos.y + plot_size.y);
                    const ImPlotRect limits = ImPlot::GetPlotLimits();
                    crosshair_state->hovered_y_min = limits.Y.Min;
                    crosshair_state->hovered_y_max = limits.Y.Max;
                }
                if (is_last) {
                    crosshair_state->last_plot_max = ImVec2(plot_pos.x + plot_size.x,
                                                            plot_pos.y + plot_size.y);
                }

                ImPlot::EndPlot();
            }
            ImPlot::PopStyleVar();

            // Splitter strip between stacked plots: drag resizes the plot
            // ABOVE (clamped in set_height_pixels); the pane total follows
            // via pane_height() on the next frame.
            if (!is_last) {
                ImGui::PushID(plot_idx[slot]);
                ImGui::InvisibleButton("##indi_splitter",
                                       ImVec2(-1.0f, kIndiSplitterH));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0f)
                    ind->set_height_pixels(ind->get_height_pixels() +
                                           ImGui::GetIO().MouseDelta.y);
                // subtle affordance: 1px BD1 hairline, brand tint while active
                {
                    const ImVec2 smin = ImGui::GetItemRectMin();
                    const ImVec2 smax = ImGui::GetItemRectMax();
                    const float  sy   = (smin.y + smax.y) * 0.5f;
                    dl->AddLine(ImVec2(smin.x, sy), ImVec2(smax.x, sy),
                                ImGui::IsItemActive()
                                    ? Theme::u32(Theme::Tokens::BRAND)
                                    : Theme::u32(Theme::Tokens::BD1));
                }
                ImGui::PopID();
            }
        }
    }

    void IndicatorManager::update_all() {
        for (auto& indicator : indicators) {
            indicator->update();
        }
    }

    void IndicatorManager::clear_all() {
        for (const auto& indicator : indicators) {
            indicator->clear();
        }
    }

    IndicatorBase* IndicatorManager::get_indicator(size_t index) {
        return index < indicators.size() ? indicators[index].get() : nullptr;
    }

    float IndicatorManager::get_total_height_ratio() const {
        float total = 0.0f;
        for (const auto& indicator : indicators) {
            if (indicator->is_visible()) {
                total += indicator->get_height_ratio();
            }
        }
        return total;
    }
}
