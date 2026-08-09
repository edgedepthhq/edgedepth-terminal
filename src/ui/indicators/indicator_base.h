#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "implot.h"

namespace Indicators {
    class IndicatorBase {
    public:
        virtual ~IndicatorBase() = default;

        // Simplified interface - just render content
        virtual void render_content(double x_min, double x_max) = 0;

        // Get Y-axis limits for this indicator
        virtual void get_y_limits(double x_min, double x_max, double& y_min, double& y_max) const = 0;

        // Update indicator data (called when new candle/trade arrives)
        virtual void update() = 0;
        virtual void clear() = 0;
        virtual const char* get_name() const = 0;
        // OPTIONAL: Custom Y-axis format string (used when get_y_formatter() returns null)
        virtual const char* get_y_format() const { return "%8.0f"; }
        // OPTIONAL: Custom Y-axis formatter callback (ImPlotFormatter signature)
        // If non-null, takes precedence over get_y_format().
        virtual ImPlotFormatter get_y_formatter() const { return nullptr; }

        // OPTIONAL: Latest value to pin as a TradingView-style axis tag.
        // Return false if there is no meaningful "current" value (default).
        // When true, `out_value` is the level and the tag is colored by
        // get_latest_direction().
        virtual bool get_latest_value(double& /*out_value*/) const { return false; }

        // OPTIONAL: Direction of the current value for tag coloring.
        // +1 = bullish/green, -1 = bearish/red, 0 = neutral/gray. Default neutral.
        virtual int get_latest_direction() const { return 0; }

        // OPTIONAL: Format the latest value for the axis tag. Defaults to the
        // same format as the Y-axis tick labels (get_y_format()).
        virtual void format_latest_value(double value, char* buf, int size) const {
            snprintf(buf, size, get_y_format(), value);
        }

        // OPTIONAL: explicit axis-tag fill color (Indicators V1, SPEC §0.2 —
        // e.g. Toxicity's regime color). Returns false to keep the default
        // direction-based coloring (get_latest_direction()).
        virtual bool get_latest_tag_color(ImU32& /*out_color*/) const { return false; }

        // OPTIONAL: per-indicator settings (F2 pilot). When has_settings()
        // is true, the tab's right-click menu gains "Settings…" which opens
        // a popover rendered by render_settings() (plain ImGui widgets).
        virtual bool has_settings() const { return false; }
        virtual void render_settings() {}

        bool is_visible() const { return visible_; }
        void set_visible(bool v) { visible_ = v; }

        // Get/set height in pixels — the stacked-plot height, adjustable by
        // the splitter drag between stacked panes. Floor = --pane-min-h-px
        // (120, tokens.json); ceiling 360 deviates from the token max (180)
        // deliberately so resize is useful — flagged for design sign-off.
        float get_height_pixels() const { return height_pixels_; }
        void set_height_pixels(float h) {
            height_pixels_ = std::clamp(h, 120.0f, 360.0f);
        }

        // Get/set height ratio (relative to main chart)
        float get_height_ratio() const { return height_ratio_; }
        void set_height_ratio(float h) { height_ratio_ = h; }

    protected:
        bool visible_ = true;
        float height_ratio_ = 0.35f;
        float height_pixels_ = 180.0f;
    };
}