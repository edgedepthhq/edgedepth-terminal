#include "ui/price_profile_renderer.h"
#include <cmath>
#include <algorithm>

namespace Profile {

void render_profile(
    ImDrawList* draw_list,
    const ImPlotRect& limits,
    const ImVec2& plot_pos,
    const ImVec2& plot_size,
    const std::vector<ProfileBand>& bands,
    const std::vector<PriceMarker>& markers,
    const ProfileConfig& config)
{
    render_profile_with_tooltip(draw_list, limits, plot_pos, plot_size,
                                bands, markers, config, nullptr, nullptr);
}

void render_profile_with_tooltip(
    ImDrawList* draw_list,
    const ImPlotRect& limits,
    const ImVec2& plot_pos,
    const ImVec2& plot_size,
    const std::vector<ProfileBand>& bands,
    const std::vector<PriceMarker>& markers,
    const ProfileConfig& config,
    TooltipCallback tooltip_cb,
    void* tooltip_user_data)
{    const float plot_left = plot_pos.x;
    const float plot_right = plot_pos.x + plot_size.x;
    const float max_bar_width = plot_size.x * config.max_bar_width_pct;

    ImPlot::PushPlotClipRect();

    // Track hovered band for tooltip
    int hovered_band = -1;
    const ImPlotPoint mouse = ImPlot::IsPlotHovered() ? ImPlot::GetPlotMousePos() : ImPlotPoint(0, 0);
    const bool check_hover = ImPlot::IsPlotHovered() && tooltip_cb != nullptr;

    // ── Render bands ───────────────────────────────────────────
    for (int i = 0; i < static_cast<int>(bands.size()); ++i) {
        const auto& band = bands[i];
        const double half_bw = band.band_width * 0.5;
        const double price_top = band.price_mid + half_bw;
        const double price_bot = band.price_mid - half_bw;

        // Cull bands outside visible price range
        if (price_bot > limits.Y.Max || price_top < limits.Y.Min) continue;
        if (band.primary_value < 0.001f) continue;

        // Convert price to pixel Y
        const ImVec2 top_px = ImPlot::PlotToPixels(0, price_top);
        const ImVec2 bot_px = ImPlot::PlotToPixels(0, price_bot);
        const float y_top = std::min(top_px.y, bot_px.y);
        const float y_bot = std::max(top_px.y, bot_px.y);

        const auto& pc = band.primary_color;

        // Layer 1: Background wash
        if (config.draw_wash && band.wash_alpha > 0.003f) {
            draw_list->AddRectFilled(
                ImVec2(plot_left, y_top),
                ImVec2(plot_right, y_bot),
                IM_COL32(pc.r, pc.g, pc.b, static_cast<uint8_t>(band.wash_alpha * 255.0f))
            );
        }

        // Layer 2: Primary bar
        const float bar_width = max_bar_width * band.primary_value;
        if (bar_width > 1.0f) {
            const float bar_left = config.right_aligned
                ? (plot_right - bar_width)
                : plot_left;
            const float bar_right = config.right_aligned
                ? plot_right
                : (plot_left + bar_width);

            draw_list->AddRectFilled(
                ImVec2(bar_left, y_top),
                ImVec2(bar_right, y_bot),
                IM_COL32(pc.r, pc.g, pc.b, static_cast<uint8_t>(band.primary_alpha * 255.0f))
            );

            // Layer 3: Secondary overlay bar
            if (band.secondary_value > 0.001f) {
                const float sec_width = bar_width * band.secondary_value;
                if (sec_width > 1.0f) {
                    const auto& sc = band.secondary_color;
                    const float sec_left = config.right_aligned
                        ? (plot_right - sec_width)
                        : plot_left;
                    const float sec_right = config.right_aligned
                        ? plot_right
                        : (plot_left + sec_width);

                    draw_list->AddRectFilled(
                        ImVec2(sec_left, y_top),
                        ImVec2(sec_right, y_bot),
                        IM_COL32(sc.r, sc.g, sc.b, static_cast<uint8_t>(band.secondary_alpha * 255.0f))
                    );
                }
            }

            // Layer 4: Edge highlight
            if (band.show_edge && band.edge_alpha > 0.01f) {
                const auto& ec = band.edge_color;
                draw_list->AddRect(
                    ImVec2(bar_left, y_top),
                    ImVec2(bar_right, y_bot),
                    IM_COL32(ec.r, ec.g, ec.b, static_cast<uint8_t>(band.edge_alpha * 255.0f)),
                    0.0f, 0, 1.0f
                );
            }
        }

        // Layer 5: Labels
        const float band_height_px = y_bot - y_top;
        if (band.label != nullptr && band_height_px > config.label_min_band_height) {
            const ImVec2 text_size = ImGui::CalcTextSize(band.label);
            if (text_size.y <= band_height_px * 0.95f) {
                float label_x, label_y;
                label_y = (y_top + y_bot) * 0.5f - text_size.y * 0.5f;
                if (config.right_aligned) {
                    label_x = plot_right - text_size.x - 6.0f;
                } else {
                    label_x = plot_left + 6.0f;
                }
                const float label_alpha = std::min(band.primary_value * 2.0f, 0.9f);
                // Shadow
                draw_list->AddText(ImVec2(label_x + 1, label_y + 1),
                    IM_COL32(0, 0, 0, static_cast<uint8_t>(label_alpha * 180)), band.label);
                // Text
                draw_list->AddText(ImVec2(label_x, label_y),
                    IM_COL32(255, 255, 255, static_cast<uint8_t>(label_alpha * 240)), band.label);
            }
        }

        // Check hover for tooltip
        if (check_hover && mouse.y >= price_bot && mouse.y <= price_top) {
            hovered_band = i;
        }
    }

    // ── Render price markers (POC, VAH, VAL, mark price) ──────
    for (const auto& marker : markers) {
        const ImVec2 marker_px = ImPlot::PlotToPixels(0, marker.price);
        if (marker_px.y < plot_pos.y || marker_px.y > plot_pos.y + plot_size.y) continue;

        if (marker.dashed) {
            const float dash_len = 6.0f;
            const float gap_len = 4.0f;
            float x = plot_left;
            while (x < plot_right) {
                const float x_end = std::min(x + dash_len, plot_right);
                draw_list->AddLine(
                    ImVec2(x, marker_px.y), ImVec2(x_end, marker_px.y),
                    marker.color, 1.0f
                );
                x += dash_len + gap_len;
            }
        } else {
            draw_list->AddLine(
                ImVec2(plot_left, marker_px.y), ImVec2(plot_right, marker_px.y),
                marker.color, 1.0f
            );
        }

        // Marker label
        if (marker.label != nullptr) {
            const ImVec2 text_size = ImGui::CalcTextSize(marker.label);
            draw_list->AddText(
                ImVec2(plot_right - text_size.x - 4, marker_px.y - text_size.y - 2),
                marker.color, marker.label
            );
        }
    }

    // ── Tooltip ────────────────────────────────────────────────
    if (hovered_band >= 0 && tooltip_cb != nullptr) {
        tooltip_cb(bands[hovered_band], hovered_band, tooltip_user_data);
    }

    ImPlot::PopPlotClipRect();
}

} // namespace Profile