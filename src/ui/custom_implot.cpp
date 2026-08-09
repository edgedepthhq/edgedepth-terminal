#include "custom_implot.h"
#include "implot_internal.h"
#include <algorithm>

// Helper functions
namespace CustomImPlot {
    namespace {
        struct CandleGeometry {
            double x;
            double open, close, low, high;
            double half_width;
            bool is_bullish;

            ImVec2 high_px, low_px, open_px, close_px;
            ImVec2 body_min, body_max;
            ImU32 color;
        };

        double calculate_half_width(const double* xs, int count, float width_param) {
            if (count > 1) {
                const double spacing = xs[1] - xs[0];
                return spacing * static_cast<double>(width_param) * 0.5;
            }
            return static_cast<double>(width_param);
        }

        CandleGeometry prepare_candle(
            double x, double open, double close, double low, double high,
            double half_width, ImVec4 bullCol, ImVec4 bearCol
        ) {
            CandleGeometry geom;
            geom.x = x;
            geom.open = open;
            geom.close = close;
            geom.low = low;
            geom.high = high;
            geom.half_width = half_width;
            geom.is_bullish = close >= open;
            // Convert to pixel coordinates
            geom.high_px = ImPlot::PlotToPixels(x, high);
            geom.low_px = ImPlot::PlotToPixels(x, low);
            geom.open_px = ImPlot::PlotToPixels(x, open);
            geom.close_px = ImPlot::PlotToPixels(x, close);
            ImVec2 left_px = ImPlot::PlotToPixels(x - half_width, open);
            ImVec2 right_px = ImPlot::PlotToPixels(x + half_width, close);
            // Calculate body rectangle
            float body_top = std::min(geom.open_px.y, geom.close_px.y);
            float body_bottom = std::max(geom.open_px.y, geom.close_px.y);
            float body_height = body_bottom - body_top;
            // Enforce minimum body height
            constexpr float MIN_BODY_HEIGHT = 2.0f;
            if (body_height < MIN_BODY_HEIGHT) {
                float center_y = (body_top + body_bottom) * 0.5f;
                body_top = center_y - MIN_BODY_HEIGHT * 0.5f;
                body_bottom = center_y + MIN_BODY_HEIGHT * 0.5f;
            }
            geom.body_min = ImVec2(left_px.x, body_top);
            geom.body_max = ImVec2(right_px.x, body_bottom);
            geom.color = ImGui::GetColorU32(geom.is_bullish ? bullCol : bearCol);
            return geom;
        }

        void draw_candle_wick(ImDrawList* draw_list, const CandleGeometry& geom) {
            if (geom.high_px.y < geom.body_min.y) { // upper wick
                draw_list->AddLine(
                    ImVec2(geom.high_px.x, geom.high_px.y),
                    ImVec2(geom.high_px.x, geom.body_min.y),
                    geom.color, 1.0f
                );
            }
            if (geom.low_px.y > geom.body_max.y) { // Lower wick (body bottom to low)
                draw_list->AddLine(
                    ImVec2(geom.low_px.x, geom.body_max.y),
                    ImVec2(geom.low_px.x, geom.low_px.y),
                    geom.color, 1.0f
                );
            }
        }

        void draw_candle_body(ImDrawList* draw_list, const CandleGeometry& geom) {
            draw_list->AddRectFilled(geom.body_min, geom.body_max, geom.color);
        }

        bool is_candle_visible(const CandleGeometry& geom, const ImVec2& plot_min, const ImVec2& plot_max) {
            return (geom.body_max.x >= plot_min.x &&
                    geom.body_min.x <= plot_max.x &&
                    geom.body_max.y >= plot_min.y &&
                    geom.body_min.y <= plot_max.y);
        }
    }
    // Public API
    template <typename T>
    int BinarySearch(const T* arr, int l, int r, T x) {
        if (r >= l) {
            int mid = l + (r - l) / 2;
            if (arr[mid] == x)
                return mid;
            if (arr[mid] > x)
                return BinarySearch(arr, l, mid - 1, x);
            return BinarySearch(arr, mid + 1, r, x);
        }
        return -1;
    }

    void PlotCandlestick(
        const char* label_id,
        const double* xs,
        const double* opens,
        const double* closes,
        const double* lows,
        const double* highs,
        int count,
        bool tooltip,
        float width_param,
        ImVec4 bullCol,
        ImVec4 bearCol) {
        if (count <= 0) return;
        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        double half_width = calculate_half_width(xs, count, width_param);
        if (ImPlot::BeginItem(label_id)) {
            ImPlot::GetCurrentItem()->Color = IM_COL32(128, 128, 128, 255);
            // Get plot bounds for visibility culling
            ImVec2 plot_min = ImPlot::GetPlotPos();
            ImVec2 plot_size = ImPlot::GetPlotSize();
            ImVec2 plot_max = ImVec2(plot_min.x + plot_size.x, plot_min.y + plot_size.y);
            ImPlot::PushPlotClipRect();
            int rendered = 0;
            for (int i = 0; i < count; ++i) {
                // Prepare candle geometry
                CandleGeometry geom = prepare_candle(
                    xs[i], opens[i], closes[i], lows[i], highs[i],
                    half_width, bullCol, bearCol
                );
                if (!is_candle_visible(geom, plot_min, plot_max)) {
                    continue;
                }
                draw_candle_wick(draw_list, geom);
                draw_candle_body(draw_list, geom);
                rendered++;
            }
            ImPlot::PopPlotClipRect();
            ImPlot::EndItem();
        }
    }

    void DrawAxisValueTag(
        double value,
        const char* text,
        ImU32 bg_color,
        bool draw_line)
    {
        if (!text || text[0] == '\0') return;
        if (!(value == value)) return;  // NaN guard

        const ImVec2 plot_pos  = ImPlot::GetPlotPos();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const float plot_left   = plot_pos.x;
        const float plot_right  = plot_pos.x + plot_size.x;
        const float plot_top    = plot_pos.y;
        const float plot_bottom = plot_pos.y + plot_size.y;

        // Pixel Y for the value on the current Y-axis.
        const float raw_y = ImPlot::PlotToPixels(ImPlotPoint(0.0, value)).y;
        // Clamp so the tag stays glued to the axis even when the level is
        // off-screen (matches TradingView: tag pins to top/bottom edge).
        const float y = ImClamp(raw_y, plot_top + 1.0f, plot_bottom - 1.0f);

        // Window draw list (NOT foreground): this paints the tag above the
        // candles/axis but still BELOW the crosshair hover label, which is drawn
        // later in the same window draw list. The axis gutter lives outside the
        // plot clip rect, but the window list isn't clipped to it here.
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Allow drawing into the axis gutter to the right of the plot, even if
        // ImPlot's clip rect is still active on the window list at this point.
        const ImVec2 ts0 = ImGui::CalcTextSize(text);
        const float clip_right = plot_right + 2.0f + ts0.x + 64.0f;
        dl->PushClipRect(ImVec2(plot_left, plot_top - 2.0f),
                         ImVec2(clip_right, plot_bottom + 2.0f), true);

        // ── Tag box in the right-hand (Opposite) Y-axis gutter ──
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float pad_x = 5.0f, pad_y = 2.0f;
        const float box_w = ts.x + pad_x * 2.0f;
        const float box_h = ts.y + pad_y * 2.0f;

        // Sit flush against the plot's right edge (a 2px notch points at the axis).
        const float box_left = plot_right + 2.0f;
        const float box_top  = ImClamp(y - box_h * 0.5f, plot_top, plot_bottom - box_h);

        // ── Dotted reference line at the value level ──
        // Spans the full plot width AND continues across the gutter to meet the
        // tag (matches TradingView). Drawn at the clamped y so it always renders
        // while the level is on-screen; skipped only when truly off-range.
        if (draw_line && raw_y >= plot_top - 1.0f && raw_y <= plot_bottom + 1.0f) {
            const ImU32 line_col = (bg_color & 0x00FFFFFF) | (uint32_t(140) << 24);
            const float dot = 2.0f, gap = 3.0f;  // fine dotted look
            const float line_end = box_left;     // run into the gutter, up to the tag
            for (float x = plot_left; x < line_end; x += dot + gap) {
                dl->AddLine(ImVec2(x, y),
                            ImVec2(ImMin(x + dot, line_end), y),
                            line_col, 1.0f);
            }
        }

        const ImVec2 r_min(box_left, box_top);
        const ImVec2 r_max(box_left + box_w, box_top + box_h);
        dl->AddRectFilled(r_min, r_max, bg_color, 2.0f);

        // White text, centered.
        dl->AddText(ImVec2(box_left + pad_x, box_top + pad_y),
                    IM_COL32(255, 255, 255, 255), text);

        dl->PopClipRect();
    }
}