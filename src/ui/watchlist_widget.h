#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// watchlist_widget.h - Symbol watchlist sidebar (edgedepth v2 · dense grid 1b)
//
// Docked left rail at Layout::WATCHLIST_W (384px). A non-scrolling header stack
// (title bar · filter row · category + venue selectors · SYMBOL/LAST/24H% sort
// chips) is pinned above a scrolling list of dense rows: star · sparkline ·
// symbol (ellipsized) · last · 24h% in fixed tabular columns, plus a dim
// 24h-volume figure beneath each row.
//
// Data: last / 24h% / 24h-volume come from the always-on TickerManager stream
// (owned by AppShell - no subscription here); sparklines from a client-side ring
// buffer sampled in update(); categories + price formatting from SymbolRegistry.
// Row click switches the terminal symbol (page-reload flow, same as the picker).
//
// Sort dimensions: Symbol / Last / 24h% (header chips) + Volume (the Vol button),
// each asc/desc; the active-volume sort turns the row volume bars accent. Star
// favourites are in-memory only (no persistence layer yet). A compact toggle,
// and auto-condense when the rail is dragged narrow, drop the sparkline for a
// denser variant.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/widget.h"
#include "core/app_context.h"
#include "core/symbol_metadata.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

class WatchlistWidget : public Widget {
public:
    WatchlistWidget(const Terminal::Pair& active_pair, const AppContext& ctx);

    void update() override;
    void render() override;
    WidgetType type() const override { return WidgetType::Watchlist; }
    const char* title() const override { return "Watchlist"; }
    UpdateFrequency update_frequency() const override { return UpdateFrequency::Slow; }

private:
    static constexpr int SPARK_N = 30;        // samples per sparkline
    static constexpr double SAMPLE_MS = 2000; // sample cadence

    struct Spark {
        std::array<float, SPARK_N> v{};
        int head = 0;   // next write slot
        int count = 0;  // valid samples
        void push(float x) {
            v[head] = x;
            head = (head + 1) % SPARK_N;
            if (count < SPARK_N) count++;
        }
    };

    enum class Sort : uint8_t { Symbol, Change, Volume, Price };

    Terminal::Pair active_pair_;
    const AppContext& ctx_;

    // Filter / sort state
    char search_buf_[48] = {};
    int selected_category_ = -1;     // -1 = All
    std::string selected_exchange_;  // empty = all venues
    Sort sort_ = Sort::Volume;
    bool sort_desc_ = true;          // Symbol defaults A->Z; others biggest first
    std::vector<std::string> categories_;
    std::vector<int> category_counts_;   // pair count per category (aligned to categories_)
    int all_count_ = 0;                  // active-symbol total (the "All" count)
    int scoped_count_ = 0;               // active count after the exchange filter
    std::array<int, 2> exchange_counts_{}; // Binance Futures, Hyperliquid
    bool categories_loaded_ = false;

    // Row extras: starred favourites (in-memory), compact mode, and the
    // category-popover search box.
    std::unordered_set<std::string> favorites_;
    bool compact_ = false;               // user compact toggle (auto when rail is narrow)
    char cat_search_[48] = {};           // category-popover substring filter

    // Sparkline ring buffers (exchange+symbol pair key -> samples), sampled in update()
    std::unordered_map<std::string, Spark> sparks_;
    int64_t last_sample_ms_ = 0;

    // Cached visible list (rebuilt when filter/sort/ticker changes)
    std::vector<const SymbolMetadata*> visible_;
    int64_t prev_ticker_ms_ = 0;
    int64_t last_resort_ms_ = 0;
    char prev_search_[48] = {};
    int prev_category_ = -2;
    std::string prev_exchange_ = "__unset";
    Sort prev_sort_ = Sort::Symbol;
    bool prev_sort_desc_ = true;

    void rebuild_visible();
    void build_category_counts();
    void render_title_bar();
    void render_filter_bar();         // full-width symbol filter input
    void render_category_selector();  // category + venue selectors and popovers
    void render_sort_header();        // SYMBOL / LAST / 24H% sort chips
    void render_rows();
    void draw_sparkline(const Spark& s, ImVec2 origin, float w, float h,
                        const ImVec4& col) const;
};
