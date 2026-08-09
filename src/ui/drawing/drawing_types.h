#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// drawing_types.h - user drawing-tools data model (TradingView-style annotations)
//
// Pure data: no ImGui/ImPlot includes. Anchors are (epoch_ms, price) so drawings
// survive pan/zoom, timeframe changes, replay, and display-timezone changes
// (axis times are structurally UTC; zone switches relabel ticks only).
// Serialized per symbol to localStorage by DrawingManager (JSON v1).
// ═══════════════════════════════════════════════════════════════════════════════
#include <cstdint>
#include <string>
#include <vector>

namespace drawing {

enum class Tool : uint8_t {
    Cursor = 0,     // default - select / drag / edit
    Trendline,
    Arrow,          // trendline + oriented head at the second anchor
    Ray,            // through both anchors, extended to the plot edge
    ExtendedLine,   // extended past BOTH anchors
    HLine,          // full-width horizontal level (1 anchor: price)
    HRay,           // anchor -> right plot edge
    VLine,          // full-height vertical time line (1 anchor: time)
    CrossLine,      // H + V through one anchor
    Rectangle,      // 2 corner anchors, filled box
    Brush,          // freehand polyline stroke
    Measure,        // EPHEMERAL 2-click readout - never stored/persisted
    PriceRange,     // persistent vertical measure (delta price / %)
    DateRange,      // persistent horizontal measure (bars / duration)
    Fib,            // fibonacci retracement between 2 anchors
    LongPosition,   // entry + derived target/stop boxes (R:R)
    ShortPosition,
    Text,           // 1 anchor + text payload
    Channel,        // parallel channel: base A->B + parallel offset
    Polyline,       // click-per-vertex open polyline
    COUNT
};

// Anchor in data space. t_ms is snapped to the candle boundary at placement.
struct Anchor {
    int64_t t_ms  = 0;
    double  price = 0.0;
};

enum class LinePattern : uint8_t { Solid = 0, Dashed = 1, Dotted = 2 };

// IM_COL32-compatible packing (ABGR in memory) without an ImGui include.
constexpr uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |  static_cast<uint32_t>(r);
}
constexpr uint32_t with_alpha(uint32_t col, uint8_t a) {
    return (col & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

// Default line color: the design system's primary text tone (#dfe6ee) - reads
// on the near-black canvas without stealing the accent role from BRAND.
inline constexpr uint32_t kDefaultColor = pack_rgba(0xdf, 0xe6, 0xee, 0xff);

struct Style {
    uint32_t    color   = kDefaultColor;
    float       width   = 1.5f;
    LinePattern pattern = LinePattern::Solid;
    // Fill for area tools (rectangle/channel/fib bands). 0 = derive from color
    // at the tool's default alpha.
    uint32_t    fill    = 0;
};

// Fib levels: bit i of Drawing::fib_mask enables kFibLevels[i].
inline constexpr double kFibLevels[] = {
    0.0, 0.236, 0.382, 0.5, 0.618, 0.786, 1.0,   // retracements (default on)
    1.272, 1.618, 2.618, 3.618                    // extensions (default off)
};
inline constexpr int      kFibLevelCount  = 11;
inline constexpr uint32_t kFibDefaultMask = 0x7F;  // first 7 levels

// One committed drawing. Flat struct (no polymorphism): per-tool extras are
// optional fields, present only when the tool uses them (mirrors the JSON).
struct Drawing {
    uint64_t          id = 0;
    Tool              tool = Tool::Trendline;
    std::vector<Anchor> anchors;
    Style             style;
    bool              locked = false;
    bool              hidden = false;

    // Text
    std::string text;
    float       font_size = 14.0f;
    // Fib
    uint32_t fib_mask = kFibDefaultMask;
    // Long/Short position (anchors[0] = entry, anchors[1].t_ms = right edge)
    double stop   = 0.0;
    double target = 0.0;
    // Channel: price offset of the parallel line vs the base A->B line
    double chan_off = 0.0;
};

// ── Tool metadata ────────────────────────────────────────────────────────────

// Anchors a tool needs to commit. 0 = variable length (Brush/Polyline),
// committed by release / double-click instead of a fixed count.
constexpr int anchors_required(Tool t) {
    switch (t) {
        case Tool::HLine: case Tool::HRay: case Tool::VLine:
        case Tool::CrossLine: case Tool::Text:
        case Tool::LongPosition: case Tool::ShortPosition:
            return 1;
        case Tool::Trendline: case Tool::Arrow: case Tool::Ray:
        case Tool::ExtendedLine: case Tool::Rectangle: case Tool::Measure:
        case Tool::PriceRange: case Tool::DateRange: case Tool::Fib:
            return 2;
        case Tool::Channel:
            return 3;  // 3rd click sets chan_off; stored as 2 anchors + offset
        case Tool::Brush: case Tool::Polyline:
            return 0;
        default:
            return 0;
    }
}

// Display name (toolbar tooltips, dropdown rows).
constexpr const char* tool_name(Tool t) {
    switch (t) {
        case Tool::Cursor:        return "Cursor";
        case Tool::Trendline:     return "Trendline";
        case Tool::Arrow:         return "Arrow";
        case Tool::Ray:           return "Ray";
        case Tool::ExtendedLine:  return "Extended line";
        case Tool::HLine:         return "Horizontal line";
        case Tool::HRay:          return "Horizontal ray";
        case Tool::VLine:         return "Vertical line";
        case Tool::CrossLine:     return "Cross line";
        case Tool::Rectangle:     return "Rectangle";
        case Tool::Brush:         return "Brush";
        case Tool::Measure:       return "Measure";
        case Tool::PriceRange:    return "Price range";
        case Tool::DateRange:     return "Date range";
        case Tool::Fib:           return "Fib retracement";
        case Tool::LongPosition:  return "Long position";
        case Tool::ShortPosition: return "Short position";
        case Tool::Text:          return "Text";
        case Tool::Channel:       return "Parallel channel";
        case Tool::Polyline:      return "Polyline";
        default:                  return "?";
    }
}

// Stable JSON key (schema v1) - never rename, unknown keys skip on load.
constexpr const char* tool_key(Tool t) {
    switch (t) {
        case Tool::Trendline:     return "trendline";
        case Tool::Arrow:         return "arrow";
        case Tool::Ray:           return "ray";
        case Tool::ExtendedLine:  return "xline";
        case Tool::HLine:         return "hline";
        case Tool::HRay:          return "hray";
        case Tool::VLine:         return "vline";
        case Tool::CrossLine:     return "cross";
        case Tool::Rectangle:     return "rect";
        case Tool::Brush:         return "brush";
        case Tool::PriceRange:    return "prange";
        case Tool::DateRange:     return "drange";
        case Tool::Fib:           return "fib";
        case Tool::LongPosition:  return "long";
        case Tool::ShortPosition: return "short";
        case Tool::Text:          return "text";
        case Tool::Channel:       return "channel";
        case Tool::Polyline:      return "polyline";
        default:                  return "";  // Cursor/Measure are never stored
    }
}

inline Tool tool_from_key(const std::string& key) {
    for (int i = 0; i < static_cast<int>(Tool::COUNT); ++i) {
        const Tool t = static_cast<Tool>(i);
        const char* k = tool_key(t);
        if (k[0] != '\0' && key == k) return t;
    }
    return Tool::COUNT;  // unknown - caller skips
}

// Capacity caps (enforced on mutation AND load).
inline constexpr size_t kMaxDrawings      = 200;
inline constexpr size_t kMaxBrushPoints   = 512;
inline constexpr size_t kMaxPolylinePoints = 64;

}  // namespace drawing
