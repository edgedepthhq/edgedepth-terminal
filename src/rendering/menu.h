#pragma once
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <unordered_set>
#include <algorithm>

#include "core/app_context.h"
#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"
#include "types/types.h"

class PositionsPanel;
class Widget;
class StreamManager;

namespace Menu {

    struct SymbolPickerState {
        bool open = false;
        char search_buf[64] = {};
        enum class PendingWidget {
            None, Orderbook, DOM, Trades, Stats, Charts, Debug, PaperTrading
        } pending = PendingWidget::None;

        int selected_category = -1;
        enum class SortMode { Symbol, Score, VPIN, Price, Change24h, Volume } sort_mode = SortMode::Symbol;
        bool sort_ascending = true; // true = ascending, false = descending

        // Exchange the picker list is filtered to (venue toggle). "binancef" | "hl".
        std::string selected_exchange = "binancef";
        std::string prev_exchange     = "binancef";
        int exchange_total = 0; // count of listed symbols for selected_exchange (header)

        std::vector<std::string> cached_categories;

        // Active symbol display in menu bar (set from main.cpp at init)
        std::string active_symbol_label; // "BTC/USDT@BINANCEF"
        std::string active_symbol;       // "btcusdt"
        std::string active_exchange;     // "binancef"

        // When true, picker replaces existing widgets instead of adding new ones
        bool replace_mode = false;

        // ── Cached filtered+sorted list (rebuilt only when inputs change) ──
        std::vector<const SymbolMetadata*> cached_visible;
        char prev_search[64] = {};
        int prev_category = -1;
        SortMode prev_sort_mode = SortMode::Symbol;
        bool prev_sort_asc = true;
        int64_t prev_ticker_update_ms = 0;
        int64_t prev_scanner_update_ms = 0;
        int64_t last_data_rebuild_ms = 0;
        bool cache_dirty = true;

        // ── Recents + favorites (persisted to localStorage) ──
        std::vector<std::string> recents;          // most-recent first, capped at 8
        std::unordered_set<std::string> favorites; // symbol keys (lowercase)
        std::vector<int> cached_category_counts;   // parallel to cached_categories
        bool persistence_loaded = false;
        int keyboard_sel = -1;                     // highlighted row (index into cached_visible)
        bool scroll_to_sel = false;                // request to scroll the highlighted row into view

        void record_recent(const std::string& sym) {
            recents.erase(std::remove(recents.begin(), recents.end(), sym), recents.end());
            recents.insert(recents.begin(), sym);
            if (recents.size() > 8) recents.resize(8);
        }
        bool is_favorite(const std::string& sym) const { return favorites.count(sym) != 0; }
        void toggle_favorite(const std::string& sym) {
            if (!favorites.erase(sym)) favorites.insert(sym);
        }

        void invalidate_cache() { cache_dirty = true; }
    };

    inline SymbolPickerState g_symbol_picker;

    void render_symbol_picker_popup(
        std::vector<std::unique_ptr<Widget>>& widgets,
        const AppContext& ctx
    );

    // ── Add-widget request (chart-toolbar "+ widget" → main loop) ────────
    // In the embedded chromes (lesson / event / pack /demo) the native topbar is
    // suppressed, so the old topbar "+" symbol picker never renders. The chart
    // toolbar's "+ widget" instead files a resolved request for THIS chart's
    // symbol (single-symbol replays), drained once per frame by the main loop in
    // EVERY host mode. PaperTrading carries no symbol and always uses this path.
    struct WidgetAddRequest {
        SymbolPickerState::PendingWidget type = SymbolPickerState::PendingWidget::None;
        Terminal::Pair pair;
        PriceFormatter fmt;
        double tick_size = 0.0;
        bool pending = false;
    };
    inline WidgetAddRequest g_widget_add_request;

    void resolve_widget_add_request(
        std::vector<std::unique_ptr<Widget>>& widgets,
        const AppContext& ctx
    );
}
