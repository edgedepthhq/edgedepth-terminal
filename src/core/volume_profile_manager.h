#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// volume_profile_manager.h - VPVR (Volume Profile Visible Range) state
//
// Stores aggregated volume profiles per symbol for the visible chart range.
// ChartWidget reads this during render to draw the VPVR sidebar.
//
// Data flow:
//   ChartWidget scroll/zoom → request_profile() → WS get_volume_profile
//   → backend queries tick_volume DB → VolumeProfileResponse proto
//   → on_profile_response() → ChartWidget reads get_profile() during render
//
//   Live updates: STREAM_TICK_VOLUME (1m bucket completions)
//   → on_tick_volume_update() → incremental add to current profile
// ═══════════════════════════════════════════════════════════════════════════════

#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace pb {
class VolumeProfileResponse;
}

class StreamManager;

class VolumeProfileManager {
public:
    // ── Per-level data ──────────────────────────────────────────────────────
    struct Level {
        double price          = 0.0;
        double buy_volume     = 0.0;
        double sell_volume    = 0.0;
        double total_volume   = 0.0;
        double delta          = 0.0;   // buy - sell
        double volume_pct     = 0.0;   // % of total
        bool   is_poc         = false;
        bool   in_value_area  = false;
    };

    // ── Aggregated profile for one symbol's visible range ───────────────────
    struct ProfileData {
        std::vector<Level> levels;
        double poc            = 0.0;
        double vah            = 0.0;
        double val            = 0.0;
        double total_volume   = 0.0;
        double value_area_vol = 0.0;
        int64_t start_time    = 0;
        int64_t end_time      = 0;
        bool   valid          = false;
    };

    // ── Rendering modes (matching MMT) ────────────────────────────────────
    enum class Mode {
        Standard,       // Green buy / red sell bars stacked
        TotalVolume,    // Single blue bar (buy+sell combined)
        TotalDelta      // 3-layer: dark total, mid dominant, bright delta
    };

    VolumeProfileManager() = default;

    // Called by ChartWidget when visible X range changes (debounced internally).
    // stream_mgr used to send the WS request.
    void request_profile(const std::string& symbol,
                         int64_t start_ms, int64_t end_ms,
                         double tick_per_row,
                         StreamManager* stream_mgr);

    // Called by MessageHandler when VolumeProfileResponse arrives.
    void on_profile_response(const std::string& symbol,
                             const pb::VolumeProfileResponse& resp);

    // Read by ChartWidget during render. Returns nullptr if no valid profile.
    const ProfileData* get_profile(const std::string& symbol) const;

    // Invalidate cached profile - forces re-request on next chart render.
    // Used on replay rewind to discard stale future volume data.
    void invalidate(const std::string& symbol);
    void invalidate_all();

    // ── Config ──────────────────────────────────────────────────────────────
    Mode  mode()          const { return mode_; }
    void  set_mode(Mode m)      { mode_ = m; }
    bool  show_poc()      const { return show_poc_; }
    void  set_show_poc(bool v)  { show_poc_ = v; }
    bool  show_vah_val()  const { return show_vah_val_; }
    void  set_show_vah_val(bool v) { show_vah_val_ = v; }
    bool  show_values()   const { return show_values_; }
    void  set_show_values(bool v) { show_values_ = v; }
    float width_pct()     const { return width_pct_; }
    void  set_width_pct(float v)  { width_pct_ = v; }
    bool  enabled()       const { return enabled_; }
    void  set_enabled(bool v)     { enabled_ = v; }

private:
    std::unordered_map<std::string, ProfileData> profiles_;

    // Config state
    Mode  mode_         = Mode::Standard;
    bool  show_poc_     = true;
    bool  show_vah_val_ = true;
    bool  show_values_  = false;
    float width_pct_    = 0.20f;  // 20% of chart width
    bool  enabled_      = false;

    // Debounce - avoid re-requesting on every frame during scroll
    int64_t last_request_time_ms_ = 0;
    int64_t last_start_ms_        = 0;
    int64_t last_end_ms_          = 0;

    static constexpr int64_t kDebounceMs = 300;
    static constexpr double  kRangeChangePct = 0.05; // 5% change triggers re-request
};
