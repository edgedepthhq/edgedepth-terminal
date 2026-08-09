#include "ui/debug_widget.h"
#include "rendering/theme.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>

// ── statics ────────────────────────────────────────────────────────────────
constexpr const char* DebugWidget::kActors[];

// ── ctor/dtor ─────────────────────────────────────────────────────────────
DebugWidget::DebugWidget(const Terminal::Pair& pair, const AppContext& ctx)
    : pair_(pair)
    , title_("DEBUG " + pair.exchange + " " + pair.symbol)
    , ctx_(ctx)
{
    filtered_indices_.reserve(DebugManager::MAX_ENTRIES);
    picker_start_.init_from_now(-1);
}

DebugWidget::~DebugWidget() {
    if (subscribed_) ctx_.debug_mgr().unsubscribe();
}

// ── case-insensitive substring match (no alloc) ───────────────────────────
bool DebugWidget::substr_match_ci(const char* haystack, const char* needle) {
    if (!needle[0]) return true;
    for (const char* p = haystack; *p; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b &&
               std::tolower(static_cast<unsigned char>(*a)) ==
               std::tolower(static_cast<unsigned char>(*b))) {
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// ── Compact mode: suppress repetitive ignition/breakout/positioning noise ──
//
// Mirrors the server-side debug.Filter logic so historical NATS data
// (produced before the server filter existed) can be viewed cleanly.
//
// Ignition candidates: suppress if score delta < 0.02 AND dist unchanged
//   within 60s. Always pass state transitions (IGNITED, PROMOTE, EXPIRED, etc.)
//
// Breakout proximity: adaptive by distance:
//   >3%: suppress unless dist changes by ≥0.5% or 5min elapsed
//   1-3%: suppress unless dist changes by ≥0.1% or 60s elapsed
//   <1%: always show
//   Always pass state transitions (INVALIDATED, CONFIRMED, TEST, etc.)
//
// Positioning publish: suppress consecutive identical messages.

// Helper: extract "dist=X.Y%" → writes into out, returns pointer to out
static const char* extract_dist(const char* msg, char* out, size_t out_sz) {
    const char* p = std::strstr(msg, "dist=");
    if (!p) { out[0] = '\0'; return out; }
    p += 5; // skip "dist="
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < out_sz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return out;
}

// Helper: parse "6.6%" or "-6.85%" → absolute float value
static float parse_abs_dist(const char* s) {
    if (!s[0]) return 999.0f;
    const char* p = s;
    if (*p == '-') ++p;
    float whole = 0, frac = 0, div = 1;
    bool in_frac = false;
    while (*p && *p != '%') {
        if (*p == '.') { in_frac = true; ++p; continue; }
        if (*p >= '0' && *p <= '9') {
            float d = static_cast<float>(*p - '0');
            if (in_frac) { div *= 10; frac += d / div; }
            else whole = whole * 10 + d;
        }
        ++p;
    }
    return whole + frac;
}

bool DebugWidget::compact_should_skip(const DebugEntry& e,
    IgnitionLast* ign, int& ign_n,
    BreakoutLast* brk, int& brk_n,
    char* last_pos_msg)
{
    // ── Ignition candidates ──────────────────────────────────────────
    if (std::strcmp(e.actor, "pattern") == 0 && std::strcmp(e.category, "ignition") == 0) {
        const char* msg = e.msg;

        // State transitions - never skip
        if (std::strstr(msg, "IGNITED") || std::strstr(msg, "PROMOTE") ||
            std::strstr(msg, "GEOMETRY") || std::strstr(msg, "EXPIRED") ||
            std::strstr(msg, "CANDIDATE held") || std::strstr(msg, "SKIP") ||
            std::strstr(msg, "candidate update") || std::strstr(msg, "TRACKED"))
            return false;

        // Only compact "candidate " lines
        if (std::strncmp(msg, "candidate ", 10) != 0) return false;

        char dist[16];
        extract_dist(msg, dist, sizeof(dist));

        // Find or create tracker for this detector
        int slot = -1;
        for (int i = 0; i < ign_n; ++i) {
            if (std::strcmp(ign[i].det, e.detector) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (ign_n >= 8) return false; // overflow, keep
            slot = ign_n++;
            std::strncpy(ign[slot].det, e.detector, 47);
            ign[slot].det[47] = '\0';
            ign[slot].score = e.score;
            std::strncpy(ign[slot].dist, dist, 15);
            ign[slot].dist[15] = '\0';
            ign[slot].ts = e.ts;
            return false; // first occurrence, show
        }

        float score_delta = std::fabs(e.score - ign[slot].score);
        bool dist_changed = std::strcmp(dist, ign[slot].dist) != 0;
        int64_t elapsed = e.ts - ign[slot].ts;

        if (score_delta >= 0.02f || dist_changed || elapsed >= 60000) {
            ign[slot].score = e.score;
            std::strncpy(ign[slot].dist, dist, 15);
            ign[slot].dist[15] = '\0';
            ign[slot].ts = e.ts;
            return false; // meaningful change, show
        }
        return true; // suppress
    }

    // ── Breakout proximity ───────────────────────────────────────────
    if (std::strcmp(e.actor, "pattern") == 0 && std::strcmp(e.category, "breakout") == 0) {
        const char* msg = e.msg;

        // State transitions - never skip
        if (std::strstr(msg, "INVALIDATED") || std::strstr(msg, "CONFIRMED") ||
            std::strstr(msg, "TEST") || std::strstr(msg, "EXPIRED") ||
            std::strstr(msg, "trendlines_crossed") || std::strstr(msg, "breakout_confirmed"))
            return false;

        char dist[16];
        extract_dist(msg, dist, sizeof(dist));
        if (!dist[0]) return false; // not a proximity log

        float dist_abs = parse_abs_dist(dist);

        // Find or create tracker
        int slot = -1;
        for (int i = 0; i < brk_n; ++i) {
            if (std::strcmp(brk[i].det, e.detector) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (brk_n >= 16) return false;
            slot = brk_n++;
            std::strncpy(brk[slot].det, e.detector, 47);
            brk[slot].det[47] = '\0';
            std::strncpy(brk[slot].dist, dist, 15);
            brk[slot].dist[15] = '\0';
            brk[slot].dist_abs = dist_abs;
            brk[slot].ts = e.ts;
            return false;
        }

        // Adaptive thresholds
        if (dist_abs < 1.0f) {
            // Approaching breakout - always show
            std::strncpy(brk[slot].dist, dist, 15);
            brk[slot].dist[15] = '\0';
            brk[slot].dist_abs = dist_abs;
            brk[slot].ts = e.ts;
            return false;
        }

        float min_change;
        int64_t min_interval;
        if (dist_abs < 3.0f) {
            min_change = 0.1f;
            min_interval = 60000;
        } else {
            min_change = 0.5f;
            min_interval = 300000;
        }

        float delta = std::fabs(dist_abs - brk[slot].dist_abs);
        int64_t elapsed = e.ts - brk[slot].ts;

        if (delta >= min_change || elapsed >= min_interval) {
            std::strncpy(brk[slot].dist, dist, 15);
            brk[slot].dist[15] = '\0';
            brk[slot].dist_abs = dist_abs;
            brk[slot].ts = e.ts;
            return false;
        }
        return true; // suppress
    }

    // ── Positioning publish ──────────────────────────────────────────
    if (std::strcmp(e.actor, "positioning") == 0 && std::strcmp(e.category, "publish") == 0) {
        if (std::strcmp(e.msg, last_pos_msg) == 0)
            return true; // identical, suppress
        std::strncpy(last_pos_msg, e.msg, 255);
        last_pos_msg[255] = '\0';
        return false;
    }

    // ── Tick volume value_area ────────────────────────────────────────
    // Suppress identical POC/VAH/VAL (ignore volume portion after "vol=")
    if (std::strcmp(e.actor, "tick_volume") == 0 && std::strcmp(e.category, "value_area") == 0) {
        // Simple: the full message rarely changes, let the existing
        // actor/category filters handle it. No extra dedup needed here
        // since tick_volume is low-frequency (every 30s per tf).
    }

    return false; // default: don't skip
}

void DebugWidget::rebuild_filtered_indices() {
    const bool range = ctx_.debug_mgr().is_range_active();
    const size_t total = range ? ctx_.debug_mgr().range_count() : ctx_.debug_mgr().count();

    // Detect whether filters changed vs just new data appended
    const bool filters_changed =
              range != prev_range_active_
              || actor_idx_ != prev_actor_idx_
              || compact_mode_ != prev_compact_
              || std::strcmp(cat_buf_, prev_cat_buf_) != 0
              || std::strcmp(srch_buf_, prev_srch_buf_) != 0;

    const bool dirty = filters_dirty_
              || total != prev_count_
              || filters_changed;

    if (!dirty) return;

    filtered_indices_.clear();

    const char* actor_f = (actor_idx_ == 0) ? "" : kActors[actor_idx_];

    // ── Compact mode state tracking ──────────────────────────────────
    IgnitionLast ign_last[8] = {};
    BreakoutLast brk_last[16] = {};
    int ign_n = 0, brk_n = 0;
    char last_pos_msg[256] = {};

    for (size_t i = 0; i < total; ++i) {
        const DebugEntry& e = range
            ? ctx_.debug_mgr().range_entry_at(i)
            : ctx_.debug_mgr().entry_at(i);

        if (actor_f[0] && std::strcmp(e.actor, actor_f) != 0) continue;
        if (cat_buf_[0] && !substr_match_ci(e.category, cat_buf_)) continue;
        if (srch_buf_[0] && !substr_match_ci(e.msg, srch_buf_)) continue;

        // ── Compact dedup ────────────────────────────────────────────
        if (compact_mode_ && compact_should_skip(e,
                ign_last, ign_n, brk_last, brk_n, last_pos_msg))
            continue;

        filtered_indices_.push_back(i);
    }

    prev_actor_idx_ = actor_idx_;
    prev_compact_ = compact_mode_;
    std::memcpy(prev_cat_buf_, cat_buf_, sizeof(cat_buf_));
    std::memcpy(prev_srch_buf_, srch_buf_, sizeof(srch_buf_));
    prev_count_ = total;
    prev_range_active_ = range;
    filters_dirty_ = false;

    if (filters_changed) {
        selection_mask_.assign(filtered_indices_.size(), false);
        sel_anchor_     = -1;
        sel_last_click_ = -1;
        has_selection_   = false;
    } else {
        selection_mask_.resize(filtered_indices_.size(), false);
    }
}

// ── update ─────────────────────────────────────────────────────────────────
void DebugWidget::update() {
    // Lazy subscribe: first update() call guaranteed to be post-WS-open
    if (!subscribed_) {
        ctx_.debug_mgr().subscribe(pair_.symbol);
        subscribed_ = true;
    }
    const size_t current = ctx_.debug_mgr().count();
    if (current != last_count_) {
        // Register brand-new entries for flash highlight
        const float now = ImGui::GetTime();
        for (size_t i = last_count_; i < current; ++i) {
            const DebugEntry& e = ctx_.debug_mgr().entry_at(i % DebugManager::MAX_ENTRIES);
            flashes_[flash_head_ % FLASH_SLOTS] = { e.ts, now };
            ++flash_head_;
        }
        last_count_ = current;
        filters_dirty_ = true;  // new data → rebuild filtered index
        if (auto_scroll_ && user_at_bottom_) scroll_to_bottom_ = true;
    }
}

// ── selection helpers ──────────────────────────────────────────────────
void DebugWidget::clear_selection() {
    if (!has_selection_) return;
    std::fill(selection_mask_.begin(), selection_mask_.end(), false);
    sel_anchor_     = -1;
    sel_last_click_ = -1;
    has_selection_   = false;
}

void DebugWidget::select_range(int from, int to) {
    if (from > to) std::swap(from, to);
    const int n = static_cast<int>(selection_mask_.size());
    from = std::max(from, 0);
    to   = std::min(to, n - 1);
    for (int i = from; i <= to; ++i)
        selection_mask_[i] = true;
    has_selection_ = true;
}

void DebugWidget::copy_selection_to_clipboard() const {
    const bool range = ctx_.debug_mgr().is_range_active();
    std::string buf;
    buf.reserve(4096);
    buf += "Timestamp\tActor\tCategory\tTimeframe\tDetector\tWindow\tScore\tThreshold\tPrice\tMessage\n";

    int copied = 0;
    for (size_t i = 0; i < filtered_indices_.size(); ++i) {
        if (i >= selection_mask_.size() || !selection_mask_[i]) continue;
        const size_t idx = filtered_indices_[i];
        const DebugEntry& e = range
            ? ctx_.debug_mgr().range_entry_at(idx)
            : ctx_.debug_mgr().entry_at(idx);
        char line[512];
        std::snprintf(line, sizeof(line),
            "%lld\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.8f\t%s\n",
            (long long)e.ts,
            e.actor, e.category, e.tf_label,
            e.detector, e.window,
            e.score, e.thresh, e.price,
            e.msg);
        buf += line;
        ++copied;
    }

    if (copied == 0) return;
    EM_ASM({
        var text = UTF8ToString($0);
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text);
        }
    }, buf.c_str());
    // Can't set copy_flash_ here (const), handled by caller
}

// ── render ─────────────────────────────────────────────────────────────────
void DebugWidget::render() {
    if (!is_open) return;
    ImGui::SetNextWindowSize(ImVec2(1000, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // PushID INSIDE Begin/End - scopes all child IDs within this window only
    ImGui::PushID(title_.c_str());
    // Ctrl+C: copy selected rows if any, else copy all
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
            if (has_selection_) {
                copy_selection_to_clipboard();
                int sel_count = 0;
                for (bool b : selection_mask_) if (b) ++sel_count;
                copy_flash_ = 1.0f;
                copy_flash_count_ = sel_count;
            } else {
                ctx_.debug_mgr().copy_all_to_clipboard();
                copy_flash_ = 1.0f;
                copy_flash_count_ = static_cast<int>(ctx_.debug_mgr().count());
            }
        }
    // Escape clears selection
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            clear_selection();
        }
    // Ctrl+A selects all filtered rows
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            if (!filtered_indices_.empty()) {
                selection_mask_.assign(filtered_indices_.size(), true);
                sel_anchor_     = 0;
                sel_last_click_ = static_cast<int>(filtered_indices_.size()) - 1;
                has_selection_  = true;
            }
        }
    render_toolbar();
    ImGui::Separator();
    render_table();
    ImGui::PopID();  // before End
    ImGui::End();
    ctx_.debug_mgr().mark_rendered();
}

void DebugWidget::render_toolbar() {
    // Actor combo
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##actor", kActors[actor_idx_])) {
        for (int i = 0; i < static_cast<int>(kNumActors); ++i) {
            ImGui::PushID(i);
            const bool sel = (actor_idx_ == i);
            if (ImGui::Selectable(kActors[i], sel)) {
                actor_idx_ = i;
                filters_dirty_ = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, 6.0f);
    // Category filter
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputTextWithHint("##cat", "category", cat_buf_, sizeof(cat_buf_)))
        filters_dirty_ = true;
    ImGui::SameLine(0.0f, 6.0f);
    // Message search
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::InputTextWithHint("##srch", "search msg", srch_buf_, sizeof(srch_buf_)))
        filters_dirty_ = true;
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Checkbox("##compact", &compact_mode_))
        filters_dirty_ = true;
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::TextDisabled("compact");
    ImGui::SameLine(0.0f, 12.0f);
    // Auto-scroll toggle
    ImGui::Checkbox("##as", &auto_scroll_);
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::TextDisabled("scroll");
    ImGui::SameLine(0.0f, 12.0f);
    // Clear / re-fetch
    if (ImGui::SmallButton("Clear")) {
        ctx_.debug_mgr().subscribe(pair_.symbol);  // triggers backfill, resets ring
        last_count_ = 0;
        flash_head_ = 0;
        filters_dirty_ = true;
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::SmallButton("Copy All")) {
        ctx_.debug_mgr().copy_all_to_clipboard();
        copy_flash_ = 1.0f;
        copy_flash_count_ = static_cast<int>(ctx_.debug_mgr().count());
    }
    if (has_selection_) {
        ImGui::SameLine(0.0f, 6.0f);
        int sel_count = 0;
        for (bool b : selection_mask_) if (b) ++sel_count;
        char sel_label[32];
        std::snprintf(sel_label, sizeof(sel_label), "Copy %d", sel_count);
        if (ImGui::SmallButton(sel_label)) {
            copy_selection_to_clipboard();
            copy_flash_ = 1.0f;
            copy_flash_count_ = sel_count;
        }
    }
    if (copy_flash_ > 0.0f) {
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(0.15f, 0.65f, 0.60f, copy_flash_),
            "Copied %d rows", copy_flash_count_);
        copy_flash_ -= ImGui::GetIO().DeltaTime * 2.0f;
    }
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0.0f, 12.0f);

    ImGui::TextDisabled("From:");
    ImGui::SameLine(0.0f, 4.0f);
    DateTimePicker::render("start", picker_start_);
    ImGui::SameLine(0.0f, 4.0f);

    if (ImGui::SmallButton("Load")) {
        int64_t start_ms = picker_start_.to_ms();
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (start_ms > 0 && start_ms < now_ms) {
            ctx_.debug_mgr().load_range(start_ms, now_ms);
            filters_dirty_ = true;
        }
    }

    if (ctx_.debug_mgr().is_range_active()) {
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton("Live")) {
            ctx_.debug_mgr().clear_range();
            filters_dirty_ = true;
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ctx_.debug_mgr().is_range_loading()) {
            ImGui::TextDisabled("Loading...");
        } else {
            char range_info[64];
            std::snprintf(range_info, sizeof(range_info), "%zu range entries",
                ctx_.debug_mgr().range_count());
            ImGui::TextDisabled("%s", range_info);
        }
    }
    // Entry count right-aligned
    const size_t total = ctx_.debug_mgr().is_range_active()
        ? ctx_.debug_mgr().range_count()
        : ctx_.debug_mgr().count();
    char status[64];
    std::snprintf(status, sizeof(status), "%zu entries", total);
    const float sw = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - sw - 16.0f);
    ImGui::TextDisabled("%s", status);
}

void DebugWidget::render_table() {
    const float avail_h = ImGui::GetContentRegionAvail().y;
    if (avail_h < 20.0f) return;
    rebuild_filtered_indices();
    const bool range = ctx_.debug_mgr().is_range_active();
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders        |
        ImGuiTableFlags_RowBg          |
        ImGuiTableFlags_ScrollY        |
        ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##dbtbl", 6, kFlags, ImVec2(0.0f, avail_h))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("TIME",  ImGuiTableColumnFlags_WidthFixed,   82.0f);
    ImGui::TableSetupColumn("ACTOR", ImGuiTableColumnFlags_WidthFixed,   80.0f);
    ImGui::TableSetupColumn("CAT",   ImGuiTableColumnFlags_WidthFixed,   72.0f);
    ImGui::TableSetupColumn("TF",    ImGuiTableColumnFlags_WidthFixed,   36.0f);
    ImGui::TableSetupColumn("SCORE", ImGuiTableColumnFlags_WidthFixed,   80.0f);
    ImGui::TableSetupColumn("MSG",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const float now = ImGui::GetTime();
    const int filtered_count = static_cast<int>(filtered_indices_.size());
    ImGuiListClipper clipper;
    clipper.Begin(filtered_count);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t idx = filtered_indices_[row];
            const DebugEntry& e = range
                ? ctx_.debug_mgr().range_entry_at(idx)
                : ctx_.debug_mgr().entry_at(idx);
            bool is_new = false;
            if (!range) {  // no flash highlights for historical range data
                for (size_t fi = 0; fi < FLASH_SLOTS; ++fi) {
                    if (flashes_[fi].ts == e.ts &&
                        (now - flashes_[fi].born) < 0.15f) {
                        is_new = true;
                        break;
                    }
                }
            }
            render_row(idx, e, is_new, row);
        }
    }
    const float scroll_y     = ImGui::GetScrollY();
    const float scroll_max_y = ImGui::GetScrollMaxY();
    user_at_bottom_ = (scroll_max_y <= 0.0f) || (scroll_y >= scroll_max_y - 20.0f);
    if (scroll_to_bottom_) {
        ImGui::SetScrollHereY(1.0f);
        scroll_to_bottom_ = false;
    }
    ImGui::EndTable();
}

// ── row ────────────────────────────────────────────────────────────────────
void DebugWidget::render_row(size_t row_idx, const DebugEntry& e, bool is_new, int filtered_row) {
    ImGui::PushID(static_cast<int>(row_idx));
    ImGui::TableNextRow();

    const bool is_selected = (filtered_row >= 0
        && filtered_row < static_cast<int>(selection_mask_.size())
        && selection_mask_[filtered_row]);

    // Selection highlight (teal tint) takes priority over new-entry flash
    if (is_selected) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            IM_COL32(38, 166, 154, 60));
    } else if (is_new) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            IM_COL32(80, 70, 10, 90));
    }

    // TIME
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", fmt_time(e.ts));
    // Capture row Y extents from the first column's item rect
    const float row_min_y = ImGui::GetItemRectMin().y;
    const float row_max_y = ImGui::GetItemRectMax().y;

    // ACTOR
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, actor_color(e.actor));
    ImGui::TextUnformatted(e.actor);
    ImGui::PopStyleColor();

    // CAT
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", e.category);

    // TF
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", e.tf_label);

    // SCORE/THRESH
    ImGui::TableNextColumn();
    if (e.score > 0.0f || e.thresh > 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_Text, score_color(e.score, e.thresh));
        ImGui::Text("%.2f/%.2f", e.score, e.thresh);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("-");
    }

    // MSG
    ImGui::TableNextColumn();
    if (e.detector[0]) {
        ImGui::TextDisabled("[%s]", e.detector);
        ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::TextUnformatted(e.msg);

    // ── Row click detection via direct mouse hit-test ──────────────────
    // ImGui::Selectable with SpanAllColumns doesn't reliably fire on
    // shift+click, so we test the row rect ourselves.
    {
        ImGuiTable* table = ImGui::GetCurrentTable();
        const float x_min = table->WorkRect.Min.x;
        const float x_max = table->WorkRect.Max.x;
        ImVec2 rmin(x_min, row_min_y);
        ImVec2 rmax(x_max, row_max_y);

        const bool hovered = ImGui::IsMouseHoveringRect(rmin, rmax, false);

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const bool shift = ImGui::GetIO().KeyShift;

            if (shift && sel_anchor_ >= 0) {
                // Shift+click: select contiguous range from anchor to here
                std::fill(selection_mask_.begin(), selection_mask_.end(), false);
                select_range(sel_anchor_, filtered_row);
                sel_last_click_ = filtered_row;
            } else {
                // Plain click: single select
                std::fill(selection_mask_.begin(), selection_mask_.end(), false);
                if (filtered_row >= 0 && filtered_row < static_cast<int>(selection_mask_.size())) {
                    selection_mask_[filtered_row] = true;
                    has_selection_ = true;
                }
                sel_anchor_     = filtered_row;
                sel_last_click_ = filtered_row;
            }
        }

        // Tooltip on row hover
        if (hovered) {
            // Use a short delay to avoid flicker - only show after ~300ms hover
            if (ImGui::GetIO().MousePos.x >= 0.0f) {
                Theme::begin_tooltip();
                ImGui::Text("ts:       %lld", (long long)e.ts);
                ImGui::Text("actor:    %s / %s", e.actor, e.category);
                ImGui::Text("tf:       %s", e.tf_label);
                if (e.detector[0]) ImGui::Text("detector: %s  win: %s", e.detector, e.window);
                if (e.score > 0.0f) ImGui::Text("score:    %.4f / %.4f", e.score, e.thresh);
                if (e.price > 0.0)  ImGui::Text("price:    %.6f", e.price);
                ImGui::Separator();
                ImGui::TextWrapped("%s", e.msg);
                Theme::end_tooltip();
            }
        }
    }

    ImGui::PopID();
}

// ── color helpers ──────────────────────────────────────────────────────────
ImVec4 DebugWidget::actor_color(const char* actor) {
    if (!actor || !actor[0]) return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    if (!std::strcmp(actor, "pattern"))      return ImVec4(0.40f, 0.82f, 0.40f, 1.0f);
    if (!std::strcmp(actor, "notification")) return ImVec4(1.00f, 0.80f, 0.20f, 1.0f);
    if (!std::strcmp(actor, "vpin"))         return ImVec4(0.40f, 0.70f, 1.00f, 1.0f);
    if (!std::strcmp(actor, "orderbook"))    return ImVec4(0.38f, 0.65f, 0.94f, 1.0f);
    if (!std::strcmp(actor, "positioning"))  return ImVec4(0.80f, 0.50f, 1.00f, 1.0f);
    if (!std::strcmp(actor, "stat"))         return ImVec4(1.00f, 0.50f, 0.30f, 1.0f);
    if (!std::strcmp(actor, "liq_heatmap"))  return ImVec4(0.94f, 0.33f, 0.33f, 1.0f);
    if (!std::strcmp(actor, "tick_volume"))  return ImVec4(0.15f, 0.65f, 0.60f, 1.0f);
    if (!std::strcmp(actor, "candle"))       return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    // Unknown actor: hash to hue
    uint32_t h = 2166136261u;
    for (const char* p = actor; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(static_cast<float>(h & 0xFF) / 255.0f, 0.65f, 0.88f, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

ImVec4 DebugWidget::score_color(float score, float thresh) {
    if (thresh <= 0.0f) return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    const float r = score / thresh;
    if (r >= 1.0f) return Theme::Colors::BUY_GREEN;
    if (r >= 0.7f) return ImVec4(1.0f, 0.85f, 0.20f, 1.0f);
    return ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
}


const char* DebugWidget::fmt_time(int64_t unix_ms) {
    static char buf[16];
    const time_t t  = unix_ms / 1000;
    const int    ms = static_cast<int>(unix_ms % 1000);
    struct tm tm_info{};
    // UTC, consistent with the rest of the terminal (tape, chart axis, transport).
#ifdef _WIN32
    gmtime_s(&tm_info, &t);
#else
    gmtime_r(&t, &tm_info);
#endif
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ms);
    return buf;
}