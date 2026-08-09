// ═══════════════════════════════════════════════════════════════════════════════
// research_moment_panel.cpp - see research_moment_panel.h. The floating
// "Investigate this minute" reader over the chart.
//
// Copy rules (the web repo's Tier-1 rules apply to terminal strings too):
// no em dashes, no ALL-CAPS chunk over 6 words, and NO rates or counts of
// what followed - counting lives in Research, where denominators are pinned.
// ═══════════════════════════════════════════════════════════════════════════════

#include "research_moment_panel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "../core/research_url.h"
#include "../education/transport_emit.h"
#include "../rendering/theme.h"
#include "upsell_modal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ui {

namespace {

// ── Frozen panel copy (greppable, one block) ─────────────────────────────────
constexpr const char* kTag = "INVESTIGATE";
constexpr const char* kLoading = "Reading this minute from the record";
constexpr const char* kZeroStandouts =
    "Nothing stood out at this minute. All %d readings sat inside their usual range.";
constexpr const char* kLead = "%d of %d readings stood out at this minute.";
constexpr const char* kFiredOne = "1 rulebook condition was firing at this minute.";
constexpr const char* kFiredMany = "%d rulebook conditions were firing at this minute.";
constexpr const char* kHonesty =
    "These readings describe the minute, not what follows it. Counts with denominators live in Research.";
constexpr const char* kLiveNote =
    "Live read: the newest closed minute, not the minute you pointed at.";
constexpr const char* kProvisionalNote = "Some readings cover a candle that is still forming.";
constexpr const char* kMsgEdgeFallback =
    "The record has not reached this minute yet. It rebuilds nightly and runs about 27 to 40 "
    "hours behind live.";
constexpr const char* kMsgNoBridge =
    "This read needs the EdgeDepth page around the terminal. Open Research directly instead.";
constexpr const char* kMsgTimeout = "The page did not answer. Open Research directly instead.";
constexpr const char* kMsgBadResult = "The read came back malformed. Open Research directly instead.";
constexpr const char* kCtaSearch = "Search the record for this";
constexpr const char* kCtaLive = "Get a live read";
constexpr const char* kCtaOpen = "Open in Research";
constexpr const char* kCtaRetry = "Retry";
constexpr const char* kCtaClose = "Close";

constexpr float kPanelW = 400.0f;
constexpr double kTimeoutS = 12.0;

bool bridge_present() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return window.__EDGEDEPTH_RESEARCH_BRIDGE__ === 1 ? 1 : 0; }) == 1;
#else
    return false;
#endif
}

// "2026-08-01T13:57:00Z" → "2026-08-01 13:57 UTC" (display only; the ISO
// stays the wire form).
std::string iso_to_header(const std::string& iso) {
    if (iso.size() < 16) return iso;
    std::string out = iso.substr(0, 16);
    out[10] = ' ';
    return out + " UTC";
}

bool primary_button(const char* label, float w) {
    using namespace Theme;
    ImGui::PushStyleColor(ImGuiCol_Button, Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_TX);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_INK);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
    ImGui::PushFont(Fonts::ui_semibold());
    const bool hit = ImGui::Button(label, ImVec2(w, 34.0f));
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return hit;
}

bool ghost_button(const char* label, float w) {
    using namespace Theme;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    const bool hit = ImGui::Button(label, ImVec2(w, 26.0f));
    ImGui::PopStyleColor(4);
    return hit;
}

}  // namespace

ResearchMomentPanel& ResearchMomentPanel::instance() {
    static ResearchMomentPanel inst;
    return inst;
}

void ResearchMomentPanel::open(const std::string& symbol_raw, int64_t minute_ms) {
    symbol_raw_ = symbol_raw;
    symbol_norm_ = research_url::normalize_symbol(symbol_raw);
    symbol_upper_ = symbol_norm_;
    std::transform(symbol_upper_.begin(), symbol_upper_.end(), symbol_upper_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    minute_ms_ = research_url::floor_minute_ms(minute_ms);
    anchor_pos_ = ImGui::GetMousePos();
    open_ = true;
    live_read_ = false;
    standouts_.clear();
    total_readings_ = 0;
    fired_rules_ = 0;
    any_provisional_ = false;
    tags_note_.clear();
    mfields_.clear();
    bucket_label_.clear();
    message_.clear();
    bridge_available_ = bridge_present();
    if (!bridge_available_) {
        state_ = State::Error;
        message_ = kMsgNoBridge;
        return;
    }
    send_request(false);
}

void ResearchMomentPanel::send_request(bool live) {
    ++req_seq_;
    last_request_live_ = live;
    state_ = State::Loading;
    request_wall_ = ImGui::GetTime();
    nlohmann::json req;
    req["reqId"] = req_seq_;
    req["symbol"] = symbol_norm_;
    req["iso"] = research_url::iso_utc(minute_ms_);
    req["live"] = live;
    edu::transport::dispatch_state("edgedepth:research-moment",
                                   "__EDGEDEPTH_RESEARCH_MOMENT_REQ__", req.dump());
}

void ResearchMomentPanel::consume_pending_result() {
#ifdef __EMSCRIPTEN__
    // Studio set_source pattern in reverse: the bridge staged a JSON string
    // in a window global and poked the void export; copy it in-glue (where
    // lengthBytesUTF8/stringToUTF8/_malloc are always available) and clear
    // the global so a stale result can never be re-read.
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var s = window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__;
            if (!s) return 0;
            delete window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__;
            s = String(s);
            var len = lengthBytesUTF8(s);
            var buf = _malloc(len + 1);
            stringToUTF8(s, buf, len + 1);
            return buf;
        } catch (e) {
            console.warn('research_moment_result: failed reading result global:', e);
            return 0;
        }
    }));
    if (raw) {
        apply_result_json(raw);
        free(raw);
    }
#endif
}

void ResearchMomentPanel::apply_result_json(const char* json) {
    if (!open_) return;
    // No-throw parse: exceptions are disabled in the WASM build, so a throw
    // would abort. A discarded parse renders the error state instead.
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        state_ = State::Error;
        message_ = kMsgBadResult;
        return;
    }
    // Only the NEWEST request may answer; a slow earlier fetch is stale.
    if (!j.contains("reqId") || !j["reqId"].is_number_integer() ||
        j["reqId"].get<int>() != req_seq_) {
        return;
    }
    const std::string state = j.value("state", "");
    const std::string message = j.value("message", "");

    // Auth outcomes funnel into the ONE conversion surface (§8.3) and the
    // panel leaves the screen: rendering a second paywall would fork it.
    if (state == "auth") {
        close();
        UpsellModal::instance().open_login(message.empty() ? nullptr : message.c_str());
        return;
    }
    if (state == "pro") {
        close();
        UpsellModal::instance().open(UpsellModal::Trigger::Research,
                                     message.empty() ? nullptr : message.c_str());
        return;
    }
    if (state == "edge") {
        state_ = State::Edge;
        message_ = message.empty() ? kMsgEdgeFallback : message;
        return;
    }
    if (state != "ok" && state != "ok_live") {
        state_ = State::Error;
        message_ = message.empty() ? kMsgBadResult : message;
        return;
    }

    live_read_ = (state == "ok_live");
    bucket_label_ = iso_to_header(j.value("bucketTime", ""));
    total_readings_ = j.value("totalReadings", 0);
    fired_rules_ = j.value("firedRules", 0);
    standouts_.clear();
    any_provisional_ = false;
    tags_note_ = j.value("tagsNote", "");
    if (j.contains("standouts") && j["standouts"].is_array()) {
        for (const auto& r : j["standouts"]) {
            if (!r.is_object()) continue;
            Row row;
            row.name = r.value("name", "");
            row.id = r.value("id", "");
            row.value = r.value("value", "");
            row.detail = r.value("detail", "");
            row.tag = r.value("tag", "");
            row.is_new = r.value("isNew", false);
            row.provisional = r.value("provisional", false);
            any_provisional_ = any_provisional_ || row.provisional;
            if (!row.name.empty()) standouts_.push_back(std::move(row));
        }
    }
    std::vector<std::string> checked;
    if (j.contains("checked") && j["checked"].is_array()) {
        for (const auto& c : j["checked"])
            if (c.is_string()) checked.push_back(c.get<std::string>());
    }
    mfields_ = research_url::join_mfields(checked);
    state_ = live_read_ ? State::ReadyLive : State::Ready;
}

void ResearchMomentPanel::open_handoff(bool live) const {
    const std::string url = live ? research_url::moment_live_url(symbol_norm_, mfields_)
                                 : research_url::moment_url(symbol_norm_, minute_ms_, mfields_);
#ifdef __EMSCRIPTEN__
    // A new tab, never a navigation: the terminal holds live WS state and
    // possibly a replay session, and navigating away throws both away.
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url.c_str());
#else
    (void)url;
#endif
}

void ResearchMomentPanel::close() {
    open_ = false;
    state_ = State::Loading;
}

bool ResearchMomentPanel::blocks_replay_escape() const {
    return open_ || esc_frame_ == ImGui::GetFrameCount();
}

void ResearchMomentPanel::render() {
    if (!open_) return;
    using namespace Theme;

    if (state_ == State::Loading && ImGui::GetTime() - request_wall_ > kTimeoutS) {
        state_ = State::Error;
        message_ = kMsgTimeout;
    }

    // Near the click, clamped so a right-edge or bottom-edge click never
    // pushes the panel off-screen.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = anchor_pos_;
    pos.x = std::min(pos.x, vp->Pos.x + vp->Size.x - kPanelW - 16.0f);
    pos.x = std::max(pos.x, vp->Pos.x + 8.0f);
    pos.y = std::min(pos.y, vp->Pos.y + vp->Size.y * 0.45f);
    pos.y = std::max(pos.y, vp->Pos.y + 8.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.0f),
                                        ImVec2(kPanelW, vp->Size.y * 0.85f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    bool keep = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##edx_research_moment", &keep, flags)) {
        // AlwaysAutoResize height is only known AFTER Begin: a tall result
        // (tags + direction lines) anchored at the click can escape the
        // viewport bottom and hide the CTAs. Clamp by shifting up; the
        // correction lands on the frame after the size settles.
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float bottom_limit = vp->Pos.y + vp->Size.y - 12.0f;
            if (wp.y + ws.y > bottom_limit) {
                const float ny = std::max(vp->Pos.y + 8.0f, bottom_limit - ws.y);
                ImGui::SetWindowPos(ImVec2(wp.x, ny));
            }
        }
        // Floating surfaces get the soft shadow (same treatment as ChartCtx).
        {
            ImDrawList* d = ImGui::GetWindowDrawList();
            const ImVec2 wa = ImGui::GetWindowPos();
            const ImVec2 wb(wa.x + ImGui::GetWindowSize().x, wa.y + ImGui::GetWindowSize().y);
            d->PushClipRectFullScreen();
            for (int i = 5; i >= 1; --i) {
                const float e = static_cast<float>(i) * 2.0f;
                d->AddRect(ImVec2(wa.x - e, wa.y - e + 3.0f), ImVec2(wb.x + e, wb.y + e + 3.0f),
                           IM_COL32(0, 0, 0, 14), Radius::R3 + e, 0, 1.6f);
            }
            d->PopClipRect();
        }
        render_body();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Esc closes the panel wherever focus sits; blocks_replay_escape() keeps
    // the same press from also stopping an active replay.
    if (open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        esc_frame_ = ImGui::GetFrameCount();
        keep = false;
    }
    if (!keep) close();
}

void ResearchMomentPanel::render_body() {
    using namespace Theme;
    const float w = ImGui::GetContentRegionAvail().x;

    // ── Header: tag + close, then symbol + minute ────────────────────────
    ImGui::PushFont(Fonts::label());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::TextUnformatted(kTag);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(w - 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    if (ImGui::Selectable("\xc3\x97", false, 0, ImVec2(12.0f, 0.0f))) {
        close();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleColor();

    ImGui::PushFont(Fonts::ui_semibold());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::TextUnformatted(symbol_upper_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(Fonts::mono_sm());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    ImGui::TextUnformatted(bucket_label_.empty()
                               ? research_url::minute_header_utc(minute_ms_).c_str()
                               : bucket_label_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // ── Body per state ───────────────────────────────────────────────────
    if (state_ == State::Loading) {
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
        char line[96];
        snprintf(line, sizeof(line), "%s%.*s", kLoading, dots, "...");
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        return;
    }

    if (state_ == State::Edge || state_ == State::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (state_ == State::Edge) {
            if (primary_button(kCtaLive, w)) send_request(true);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaOpen, w)) open_handoff(false);
        } else if (bridge_available_) {
            if (primary_button(kCtaRetry, w)) send_request(last_request_live_);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaOpen, w)) open_handoff(false);
        } else {
            if (primary_button(kCtaOpen, w)) open_handoff(false);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaClose, w)) close();
        }
        return;
    }

    // Ready / ReadyLive.
    if (state_ == State::ReadyLive) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::WARN);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kLiveNote);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    char lead[128];
    if (standouts_.empty()) {
        snprintf(lead, sizeof(lead), kZeroStandouts, total_readings_);
    } else {
        snprintf(lead, sizeof(lead), kLead, static_cast<int>(standouts_.size()), total_readings_);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(lead);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // Standout rows: reader name over the registry id, value right-aligned.
    // Both name AND id are visible, exactly like the web moment panel. The
    // tag (NEW vs STANDING against -1h), the worded direction and the id are
    // all bridge-supplied strings rendered verbatim; only their COLORS are
    // decided here (a new standout gets the accent, a standing one stays
    // muted - it is the afternoon's weather, not this minute's event).
    for (const Row& row : standouts_) {
        const ImVec2 line_start = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::PopStyleColor();
        if (!row.tag.empty()) {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::PushFont(Fonts::label());
            ImGui::PushStyleColor(ImGuiCol_Text, row.is_new ? Tokens::BRAND : Tokens::TX3);
            ImGui::TextUnformatted(row.tag.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        ImGui::PushFont(Fonts::mono());
        const float vw = ImGui::CalcTextSize(row.value.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(line_start.x + w - vw, line_start.y));
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::TextUnformatted(row.value.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();

        if (!row.detail.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(row.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::PushFont(Fonts::mono_xs());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        if (row.provisional) {
            char idbuf[96];
            snprintf(idbuf, sizeof(idbuf), "%s (forming)", row.id.c_str());
            ImGui::TextUnformatted(idbuf);
        } else {
            ImGui::TextUnformatted(row.id.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    if (!tags_note_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tags_note_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    if (fired_rules_ > 0) {
        char fired[96];
        if (fired_rules_ == 1) {
            snprintf(fired, sizeof(fired), "%s", kFiredOne);
        } else {
            snprintf(fired, sizeof(fired), kFiredMany, fired_rules_);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(fired);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    if (any_provisional_) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kProvisionalNote);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(kHonesty);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (primary_button(kCtaSearch, w)) open_handoff(live_read_);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ghost_button(kCtaClose, w)) close();
}

}  // namespace ui

// ═══════════════════════════════════════════════════════════════════════════════
// KEEPALIVE result callback - the bridge calls Module.__research_moment_result()
// after staging window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__. Mirrors the
// _studio_cmd_set_source convention: defined WITH a leading underscore,
// exported as __research_moment_result in BOTH CMake EXPORTED_FUNCTIONS lists.
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef __EMSCRIPTEN__
extern "C" {

EMSCRIPTEN_KEEPALIVE void _research_moment_result(void) {
    ui::ResearchMomentPanel::instance().consume_pending_result();
}

}  // extern "C"
#endif
