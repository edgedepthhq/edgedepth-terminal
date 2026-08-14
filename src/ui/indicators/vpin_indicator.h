#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// vpin_indicator.h - Toxicity pane (VPIN, Indicators V1 Wave B pathfinder)
//
// Implementation contract: SPEC §2 as amended by the "Eras" revamp
// (James pick from the 2026-08-13 mockup round; SPEC/tokens.json mirror
// pending):
//   · fixed 0-1.0 axis, gridlines .25/.50/.75, dotted landmark rules at
//     .30/.45/.60 at 20% alpha WITH in-plot value labels
//   · VPIN = 1px STEP-HOLD line (H then V at each volume-clock print) in
//     ONE bright ink (TX1); shelves are honest - quiet symbols hold; the
//     hold past the last finalized bucket renders at tentative alpha
//   · conf < 0.65 in HMM eras → that segment at 0.55 alpha (quantized)
//   · regime = full-height background washes (>= ELEVATED only, alpha
//     .05/.07/.09, halved in threshold-fallback eras) + 3px on-axis strip;
//     color seams land exactly on state-change prints (no extra glyphs);
//     fallback eras at strip alpha × 0.4 + one THRESH|HMM→ seam
//   · persistent corner readout: regime word · brain (+conf) · latest VPIN
//   · axis tag = regime color, NORMAL → neutral (SPEC §0.2)
//
// Data = finalized volume-bucket prints from the IndicatorSeriesManager
// (ChartWidget repopulates on revision change). No separate building point:
// every wire VPINStateUpdate IS a bucket completion (parity grain rule).
// ═══════════════════════════════════════════════════════════════════════════════

#include "indicator_base.h"
#include "core/indicator_series.h"
#include <cstdint>
#include <vector>

namespace Indicators {

    inline int format_vpin_axis(double value, char* buf, int size, void*) {
        return snprintf(buf, size, "%6.4f", value);
    }

    class VPINIndicator : public IndicatorBase {
    public:
        VPINIndicator() {
            height_ratio_ = 0.22f;
            height_pixels_ = 150.0f;
        }

        // ChartWidget repopulate path (revision-gated)
        void set_points(const std::vector<Series::VPINPoint>& pts) {
            pts_ = pts;
        }
        void clear_points() { pts_.clear(); }
        size_t point_count() const { return pts_.size(); }

        // ── IndicatorBase ──
        void render_content(double x_min, double x_max) override;
        void get_y_limits(double, double, double& y_min, double& y_max) const override {
            y_min = 0.0; y_max = 1.0;   // SPEC §2: fixed, never rescales
        }
        void update() override {}
        void clear() override { pts_.clear(); }
        const char* get_name() const override { return "TOXICITY"; }
        ImPlotFormatter get_y_formatter() const override { return format_vpin_axis; }

        bool get_latest_value(double& out_value) const override {
            if (pts_.empty()) return false;
            out_value = static_cast<double>(pts_.back().vpin);
            return true;
        }
        int get_latest_direction() const override { return 0; }
        void format_latest_value(double value, char* buf, int size) const override {
            format_vpin_axis(value, buf, size, nullptr);
        }
        // SPEC §0.2: tag fill = regime color, NORMAL → neutral.
        bool get_latest_tag_color(ImU32& out_color) const override;

        // ── Settings (F2 pilot) ──
        bool has_settings() const override { return true; }
        void render_settings() override;

    private:
        std::vector<Series::VPINPoint> pts_;   // ascending ts_ms (SeriesCache order)

        // §4.5 settings: regime coloring on/off + order-imbalance companion.
        bool regime_coloring_ = true;
        bool show_imbalance_  = false;

        // first visible index minus one (entering shelf), via binary search
        size_t first_visible(double x_min) const;
    };

} // namespace Indicators
