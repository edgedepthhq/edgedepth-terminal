#pragma once
#include "ui/widget.h"
#include "core/debug_manager.h"
#include "core/app_context.h"
#include "ui/datetime_picker.h"
#include "types/types.h"
#include <string>
#include <array>
#include <vector>
#include <cstdint>

class DebugWidget : public Widget {
public:
    DebugWidget(const Terminal::Pair& pair, const AppContext& ctx);
    ~DebugWidget() override;

    void render() override;
    void update() override;

    WidgetType  type()  const override { return WidgetType::DebugLog; }
    const char* title() const override { return title_.c_str(); }
    UpdateFrequency update_frequency() const override { return UpdateFrequency::Standard; }

private:
    Terminal::Pair pair_;
    std::string    title_;
    const AppContext& ctx_;
    bool           subscribed_       = false;
    bool           auto_scroll_      = true;
    bool           scroll_to_bottom_ = false;
    size_t         last_count_       = 0;
    bool prev_range_active_ = false;

    // ── Filters (client-side only — no re-subscribe on change) ───────────
    static constexpr const char* kActors[] = {
        "All", "pattern", "notification", "vpin",
        "orderbook", "positioning", "stat",
        "liq_heatmap", "tick_volume", "candle"
    };
    static constexpr size_t kNumActors = 10;
    int  actor_idx_   = 0;      // 0 = All
    char cat_buf_[32] = {};
    char srch_buf_[64]= {};
    bool compact_mode_ = false; // suppress repetitive ignition/breakout/positioning noise

    bool user_at_bottom_ = true;
    float copy_flash_ = 0.0f;
    int   copy_flash_count_ = 0;

    // ── Filtered index for ImGuiListClipper ───────────────────────────────
    // Rebuilt once per frame (only when data or filters change).
    // Stores indices into the ring buffer for rows that pass filters.
    std::vector<size_t> filtered_indices_;
    int  prev_actor_idx_  = -1;
    char prev_cat_buf_[32] = {};
    char prev_srch_buf_[64] = {};
    size_t prev_count_    = 0;
    bool prev_compact_    = false;
    bool filters_dirty_   = true;

    DateTimePicker picker_start_;

    // ── Row selection (indices into filtered_indices_) ─────────────────
    std::vector<bool> selection_mask_;   // parallel to filtered_indices_
    int  sel_anchor_     = -1;          // filtered row of first click
    int  sel_last_click_ = -1;          // last clicked filtered row (for shift-extend)
    bool has_selection_   = false;

    void clear_selection();
    void select_range(int from, int to);      // inclusive, in filtered-row space
    void copy_selection_to_clipboard() const;

    void rebuild_filtered_indices();
    static bool substr_match_ci(const char* haystack, const char* needle);

    // ── Compact mode dedup structs (stack-allocated, used during rebuild) ──
    struct IgnitionLast { char det[48]; float score; char dist[16]; int64_t ts; };
    struct BreakoutLast { char det[48]; char dist[16]; float dist_abs; int64_t ts; };
    static bool compact_should_skip(const DebugEntry& e,
        IgnitionLast* ign, int& ign_n,
        BreakoutLast* brk, int& brk_n,
        char* last_pos_msg);

    // ── Flash: 150 ms highlight on new entries ────────────────────────────
    static constexpr size_t FLASH_SLOTS = 32;
    struct Flash { int64_t ts = 0; float born = 0.0f; };
    std::array<Flash, FLASH_SLOTS> flashes_{};
    size_t flash_head_ = 0;

    // ── Helpers ───────────────────────────────────────────────────────────
    void render_toolbar();
    void render_table();
    void render_row(size_t row_idx, const DebugEntry& e, bool is_new, int filtered_row);

    static ImVec4      actor_color(const char* actor);
    static ImVec4      score_color(float score, float thresh);
    static const char* fmt_time(int64_t unix_ms);
};