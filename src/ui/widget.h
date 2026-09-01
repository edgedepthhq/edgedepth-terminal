#pragma once
#include <deque>
#include <chrono>
#include <string>

#include "../stream_handler.h"
#include "../types/types.h"
#include "imgui.h"

struct StreamKey;
class StreamManager;

enum class WidgetType {
    Trades,
    Orderbook,
    Chart,
    DOM,
    Stats,
    Heatmap,
    DebugLog,
    PaperTrading,
    ReplayLibrary,
    Watchlist
};

enum class UpdateFrequency {
    Turbo, // 120 FPS+
    RealTime,    // 60 FPS - OrderBook, DOM, Trades feed
    Standard,    // 30 FPS - Charts, Stats
    Slow         // 10 FPS - Heatmaps, slow-changing data
};

class Widget {
public:
    virtual ~Widget() = default;

    virtual void render() = 0;
    virtual void update() = 0;
    virtual WidgetType type() const = 0;
    virtual const char* title() const = 0;

    // Called on replay backward skip (<<). Widgets should clear any buffered
    // data with timestamps after cutoff_ms. Default: no-op.
    virtual void on_rewind(int64_t /*cutoff_ms*/) {}

    // Instrument precision (tick size, price/qty formatting) can arrive AFTER a
    // widget is built: SymbolRegistry answers over the network and a pack header
    // carries its own tick. Widgets that CACHE either one must re-read them here
    // instead of living with what the registry happened to know at construction.
    // Called from the frame loop whenever SymbolRegistry::epoch() moves.
    // Default: no-op (widgets that look the registry up per draw need nothing).
    virtual void refresh_instrument() {}

    // ═══ Performance Hints ═══
    virtual UpdateFrequency update_frequency() const { return UpdateFrequency::Standard; }
    virtual bool is_fast_update() const {
        return update_frequency() == UpdateFrequency::RealTime;
    }

    bool is_visible() const {
        return is_open && was_visible_last_frame_;
    }

    bool should_update(int frame_counter) const {
        if (!is_visible()) return false;
        switch (update_frequency()) {
            case UpdateFrequency::RealTime:
                return true;  // Every frame
            case UpdateFrequency::Standard:
                return (frame_counter % 2) == 0;  // Every 2nd frame (30 FPS)
            case UpdateFrequency::Slow:
                return (frame_counter % 6) == 0;  // Every 6th frame (10 FPS)
            default:
                return true;
        }
    }

    bool is_open = true;
    bool is_replay_widget = false;  // Created during replay - auto-closed on exit

    // Append suffix to widget title (e.g. "##replay" for ImGui ID uniqueness)
    void set_title_suffix(const std::string& suffix) { title_suffix_ = suffix; }
    const std::string& title_suffix() const { return title_suffix_; }
    struct WindowHints {
        bool use_hints = false;
        float pos_x = 0.0f;
        float pos_y = 0.0f;
        float size_x = 400.0f;
        float size_y = 600.0f;
    } hints;

protected:
    Widget() = default;

    // The StreamManager this widget SUBSCRIBED to.
    //
    // AppContext is one shared struct, and a replay swaps its manager pointers
    // IN PLACE (main.cpp: app_ctx.streams = replay_ctx->streams on the way in,
    // build_app_context() on the way out). A destructor that re-resolves
    // ctx_.stream_mgr() therefore asks a DIFFERENT manager to forget a handler
    // it never held: the subscription stays behind in the first manager with a
    // widget_ptr that is about to dangle, and the second manager loses nothing.
    // Pin the manager at subscribe time and unsubscribe through this pointer.
    //
    // It stays valid because ReplayManager retires a replay context for a frame
    // instead of freeing it inside the swap callback, so no widget is ever
    // erased after the manager it subscribed to has gone. See
    // ReplayManager::release_retired_context.
    StreamManager* subscribed_streams_ = nullptr;

    // Track visibility for optimization
    bool was_visible_last_frame_ = false;
    std::string title_suffix_;  // Optional suffix for ImGui ID uniqueness
    // Helper for derived classes to use in render()
    bool begin_render(const char* title, ImGuiWindowFlags flags = 0) {
        if (!is_open) return false;
        if (title_suffix_.empty()) {
            bool visible = ImGui::Begin(title, &is_open, flags);
            was_visible_last_frame_ = visible;
            return visible;
        }
        // Append suffix for ImGui ID uniqueness (e.g. "##replay")
        std::string full_title = std::string(title) + title_suffix_;
        bool visible = ImGui::Begin(full_title.c_str(), &is_open, flags);
        was_visible_last_frame_ = visible;
        return visible;
    }
    void end_render() {
        ImGui::End();
    }
};
