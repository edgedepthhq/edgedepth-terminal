#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// research_moment_panel.h - the in-terminal "Investigate this minute" reader
// (host-page
// relay - option 2, the recommended one).
//
// Right-clicking a chart minute opens this floating panel with the registry
// readings that stood out at that minute, in reader words. The panel is an
// APPETIZER for the web surface, deliberately: it renders strings the web
// bridge derived (readerName + isDistinctive live in the web app, the one
// place those rules exist) and never counts, rates, or thresholds - the
// frozen, test-pinned honesty copy stays on the web side only. The one CTA
// hands off to /research via research_url.h.
//
// Relay contract (the studio set_source pattern, in reverse):
//   C++ →  window.__EDGEDEPTH_RESEARCH_MOMENT_REQ__ = {reqId, symbol, iso, live}
//          + CustomEvent 'edgedepth:research-moment'   (transport_emit dispatch)
//   web →  window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__ = JSON string
//          + Module.__research_moment_result()          (void KEEPALIVE export)
// The bridge (the web app's researchMomentBridge.ts, installed by
// TerminalEmbed) advertises itself as window.__EDGEDEPTH_RESEARCH_BRIDGE__=1;
// without it (standalone serve) the panel falls back to "open Research".
//
// Auth outcomes never render here: AUTH_REQUIRED routes to the login variant
// and GRANT_REQUIRED to the Pro variant of UpsellModal - the ONE conversion
// surface every gate funnels into (upsell_modal.h §8.3).
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace ui {

class ResearchMomentPanel {
public:
    static ResearchMomentPanel& instance();

    // Open for a right-clicked minute. `symbol_raw` is the display pair
    // ("BTC/USDT" or "btcusdt" - normalized inside); `minute_ms` must
    // already be the 60000-floored minute (chart_widget captures it
    // separately from the timeframe-floored replay timestamp).
    void open(const std::string& symbol_raw, int64_t minute_ms);

    // Call ONCE per frame from the main render loop (after the widgets, next
    // to UpsellModal::render()).
    void render();

    [[nodiscard]] bool is_open() const { return open_; }

    // True while the panel owns the Escape key (open now, or closed BY
    // Escape this frame). The replay stop-on-Esc checks this so closing the
    // panel never also kills an active replay.
    [[nodiscard]] bool blocks_replay_escape() const;

    // KEEPALIVE entry (_research_moment_result): the bridge staged a result
    // JSON in the window global; read + apply it.
    void consume_pending_result();

private:
    ResearchMomentPanel() = default;

    enum class State : uint8_t { Loading, Ready, ReadyLive, Edge, Error };

    struct Row {
        std::string name;    // reader words, supplied by the web bridge
        std::string id;      // registry id (feature.*)
        std::string value;   // display-formatted by the bridge
        std::string detail;  // worded direction for signed readings ("" = none)
        std::string tag;     // "NEW"/"STANDING" vs -1h ("" = no honest baseline)
        bool is_new = false; // styling hint that pairs with tag
        bool provisional = false;
    };

    void send_request(bool live);
    void apply_result_json(const char* json);
    void render_body();
    void open_handoff(bool live) const;
    void close();

    // ── State ────────────────────────────────────────────────────────────
    bool    open_ = false;
    State   state_ = State::Loading;
    bool    bridge_available_ = false;   // web relay advertised itself at open()
    bool    last_request_live_ = false;  // what Retry should re-send
    int     req_seq_ = 0;          // matches results to the newest request
    double  request_wall_ = 0.0;   // ImGui::GetTime() at send, for the timeout
    int     esc_frame_ = -1;       // frame the panel consumed Escape on
    ImVec2  anchor_pos_{};         // right-click position, clamp-positioned

    std::string symbol_raw_;       // as the terminal displays it
    std::string symbol_norm_;      // "btcusdt" - the web's form
    std::string symbol_upper_;     // "BTCUSDT" - the header's form
    int64_t     minute_ms_ = 0;    // the POINTED minute (60000-floored)

    bool             live_read_ = false;  // result is the live substitution
    std::string      bucket_label_;       // "2026-08-01 13:57 UTC" of the result
    std::string      message_;            // edge/error notice text
    std::vector<Row> standouts_;
    int              total_readings_ = 0;
    int              fired_rules_ = 0;
    bool             any_provisional_ = false;
    std::string      tags_note_;          // explains the -1h comparison ("" = untagged)
    std::string      mfields_;            // pre-checked ids for the handoff URL
};

}  // namespace ui
