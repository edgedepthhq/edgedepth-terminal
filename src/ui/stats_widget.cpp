// ═══════════════════════════════════════════════════════════════════════════════
// stats_widget.cpp - Stats panel v2 (see stats_widget.h + stats-panel.SPEC.md).
//
// STEP 1 (shell): window + status strip + header/price block + collapsible section
// headers (with PRO tags and free-tier UNLOCK -> UpsellModal) + pinned settings
// row. Section BODIES (live rows + premium placeholders + meters/chips/lock) land
// in step 2; settings behaviors + replay-aware values in step 3.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/stats_widget.h"
#include "core/stream_presence.h"
#include "rendering/theme.h"
#include "core/entitlements.h"
#include "ui/upsell_modal.h"
#include "core/analytics_manager.h"
#include "core/indicator_series.h"
#include "core/candle_manager.h"
#include "imgui.h"

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <string>

using namespace Theme;

namespace {

inline ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Small pill: optional fill + optional border + centered mono-tiny label. Used for
// the PRO tag and the UNLOCK chip. Returns the pill width.
float draw_tag(ImDrawList* dl, ImVec2 pos, const char* txt,
               ImU32 bg, ImU32 border, ImU32 fg, float pad_x) {
    ImGui::PushFont(Fonts::mono_sm());
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const float w = ts.x + pad_x * 2.0f;
    const float h = ts.y + 3.0f;
    const ImVec2 b(pos.x + w, pos.y + h);
    if (bg)     dl->AddRectFilled(pos, b, bg, 2.0f);
    if (border) dl->AddRect(pos, b, border, 2.0f, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad_x, pos.y + 1.5f), fg, txt);
    ImGui::PopFont();
    return w;
}

}  // namespace

StatsWidget::StatsWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt)
    : pair_(pair)
    , title_("Stats " + pair.exchange + " " + pair.symbol)
    , stream_key_{pair, Terminal::Stream::Stats, 1}
    , ctx_(ctx)
    , fmt_(fmt)
{
    StreamHandler<Terminal::Stat> handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Stat& s) {
            static_cast<StatsWidget*>(ptr)->handle_stat(s);
        }
    };
    ctx_.stream_mgr().subscribe_stats(stream_key_, handler);
}

StatsWidget::~StatsWidget() {
    ctx_.stream_mgr().unsubscribe_stats(stream_key_, this);
}

void StatsWidget::handle_stat(const Terminal::Stat& stat) {
    // Best-effort trades/sec from consecutive stat deltas (cumulative counters).
    if (have_prev_) {
        const double dt = static_cast<double>(stat.timestamp_ms - prev_stat_.timestamp_ms) / 1000.0;
        const double dn = static_cast<double>((stat.trade_buy + stat.trade_sell)
                                            - (prev_stat_.trade_buy + prev_stat_.trade_sell));
        if (dt > 0.0 && dn >= 0.0) tps_ = dn / dt;
    }
    prev_stat_ = stat;
    have_prev_ = true;

    // Backstop for partial Stats: proto3 has no field presence, so a freshly
    // rolled-over bucket can arrive with mark / OI / next-funding = 0 until the
    // next tick refreshes them. Carry the last non-zero level forward so the panel
    // does not flicker to 0. (The backend sampler carry-forward is the real fix;
    // this also covers a not-yet-deployed backend + the very first stat.) Funding
    // is left as-is -- 0 is a legitimate funding rate.
    const Terminal::Stat prev_shown = current_stat_;
    current_stat_ = stat;
    if (current_stat_.mark_price == 0.0)        current_stat_.mark_price        = prev_shown.mark_price;
    if (current_stat_.open_interest_usd == 0.0) current_stat_.open_interest_usd = prev_shown.open_interest_usd;
    if (current_stat_.next_funding_time == 0)   current_stat_.next_funding_time = prev_shown.next_funding_time;
}

void StatsWidget::update() {
    // Live PositioningState subscribe - lazy + self-healing across replay round
    // trips. Positioning frames land in the AnalyticsManager via message_handler
    // (no widget callback), so we just request the stream like the chart's VPIN.
    // send_subscribe is a no-op in replay; reset the flag while replaying so the
    // stream re-subscribes when live resumes (send_subscribe streams are not in
    // the resume_live_subscriptions registry).
    const bool is_replay = ctx_.candle_mgr().replay_start_time_ms() > 0;
    if (is_replay) {
        positioning_subscribed_ = false;
    } else if (!positioning_subscribed_) {
        ctx_.stream_mgr().send_subscribe(StreamKey{ pair_, Terminal::Stream::PositioningState, 0 });
        // VPIN too, so the panel's VPIN row does not depend on the chart having
        // the VPIN indicator active (double-subscribe is idempotent server-side).
        ctx_.stream_mgr().send_subscribe(StreamKey{ pair_, Terminal::Stream::VPINState, 0 });
        positioning_subscribed_ = true;
    }
}

void StatsWidget::render() {
    if (!is_open) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0.0f, 0.0f));
    const bool vis = ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoScrollbar);
    if (!vis) { ImGui::End(); ImGui::PopStyleVar(2); return; }

    // TODO(step 3): replay-aware - warn dot + AS OF banner + scrubber-time values.
    const bool live = true;

    render_status_strip(live);
    if (!live) render_replay_banner();
    render_header_price();

    // Sections scroll; the settings row (and step-2 signal chip) stay pinned.
    const float pinned_h = 30.0f;
    const float body_h = ImGui::GetContentRegionAvail().y - pinned_h;
    ImGui::BeginChild("##stats_body", ImVec2(0.0f, body_h > 0.0f ? body_h : 0.0f), false);
    if (render_section_header(SEC_ORDER_FLOW, "ORDER FLOW", true))
        render_order_flow();
    if (render_section_header(SEC_POSITIONING, "POSITIONING", false))
        render_positioning();
    if (render_section_header(SEC_LIQ_RISK, "LIQUIDATIONS & RISK", true))
        render_liq_risk();
    ImGui::EndChild();

    render_settings_row();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void StatsWidget::render_status_strip(bool live) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float H = 28.0f, PADX = 10.0f;
    const float cy = org.y + H * 0.5f;

    dl->AddCircleFilled(ImVec2(org.x + PADX + 3.0f, cy), 3.0f,
                        col(live ? Tokens::UP : Tokens::WARN));

    const std::string t = "STATS \xc2\xb7 " + upper(pair_.symbol);
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(org.x + PADX + 13.0f, cy - ImGui::GetFontSize() * 0.5f),
                col(Tokens::TX2), t.c_str());
    ImGui::PopFont();

    if (Entitlements::is_pro()) {
        ImGui::PushFont(Fonts::mono_sm());
        const float tw = ImGui::CalcTextSize("PRO").x + 8.0f;
        ImGui::PopFont();
        draw_tag(dl, ImVec2(org.x + ww - PADX - tw, cy - 7.0f), "PRO",
                 0, col(Tokens::BRAND_LINE), col(Tokens::BRAND_TX), 4.0f);
    } else {
        ImGui::PushFont(Fonts::mono_sm());
        const float tw = ImGui::CalcTextSize("FREE").x;
        dl->AddText(ImVec2(org.x + ww - PADX - tw, cy - ImGui::GetFontSize() * 0.5f),
                    col(Tokens::TX3), "FREE");
        ImGui::PopFont();
    }

    dl->AddLine(ImVec2(org.x, org.y + H), ImVec2(org.x + ww, org.y + H), col(Tokens::BD1), 1.0f);
    ImGui::Dummy(ImVec2(ww, H));
}

void StatsWidget::render_replay_banner() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float H = 24.0f, PADX = 12.0f;
    const float cy = org.y + H * 0.5f;

    dl->AddRectFilled(org, ImVec2(org.x + ww, org.y + H), col(Tokens::WARN_SOFT));
    dl->AddCircleFilled(ImVec2(org.x + PADX + 2.5f, cy), 2.5f, col(Tokens::WARN));
    ImGui::PushFont(Fonts::mono_sm());
    dl->AddText(ImVec2(org.x + PADX + 10.0f, cy - ImGui::GetFontSize() * 0.5f),
                col(Tokens::WARN), "AS OF -- UTC");  // TODO(step 3): scrubber time
    ImGui::PopFont();
    dl->AddLine(ImVec2(org.x, org.y + H), ImVec2(org.x + ww, org.y + H), col(Tokens::BD1), 1.0f);
    ImGui::Dummy(ImVec2(ww, H));
}

void StatsWidget::render_header_price() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 12.0f, H = 96.0f;

    // Symbol + exchange
    float y = org.y + 11.0f;
    const std::string sym = upper(pair_.symbol);
    ImGui::PushFont(Fonts::ui_semibold());
    dl->AddText(ImVec2(org.x + PADX, y), col(Tokens::TX1), sym.c_str());
    const float sw = ImGui::CalcTextSize(sym.c_str()).x;
    ImGui::PopFont();
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(org.x + PADX + sw + 7.0f, y + 3.0f), col(Tokens::TX3),
                upper(pair_.exchange).c_str());
    ImGui::PopFont();

    // Mark price (large mono) + 24h change (placeholder - no 24h field on the stream)
    y += 21.0f;
    char mk[40];
    snprintf(mk, sizeof(mk), fmt_.price_fmt, current_stat_.mark_price);
    ImGui::PushFont(Fonts::mono_lg());
    dl->AddText(ImVec2(org.x + PADX, y), col(Tokens::TX1), mk);
    const float mw = ImGui::CalcTextSize(mk).x;
    ImGui::PopFont();
    ImGui::PushFont(Fonts::mono_sm());
    dl->AddText(ImVec2(org.x + PADX + mw + 10.0f, y + 9.0f), col(Tokens::TX3), "-- %");
    ImGui::PopFont();

    // Sub-pairs: 24H H/L and 24H VOL (placeholders - TODO 24h ticker source)
    y += 31.0f;
    auto sub = [&](float x, const char* label, const char* value) {
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(x, y), col(Tokens::TX3), label);
        ImGui::PopFont();
        ImGui::PushFont(Fonts::mono_sm());
        dl->AddText(ImVec2(x, y + 12.0f), col(Tokens::TX1), value);
        ImGui::PopFont();
    };
    sub(org.x + PADX, "24H H/L", "-- / --");
    sub(org.x + PADX + 160.0f, "24H VOL", "--");

    dl->AddLine(ImVec2(org.x, org.y + H), ImVec2(org.x + ww, org.y + H), col(Tokens::BD1), 1.0f);
    ImGui::Dummy(ImVec2(ww, H));
}

bool StatsWidget::render_section_header(Section s, const char* name, bool pro_section) {
    ImGui::PushID(name);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float H = 26.0f, PADX = 12.0f;
    const bool show_unlock = pro_section && !Entitlements::is_pro();

    float uw = 0.0f;
    if (show_unlock) {
        ImGui::PushFont(Fonts::mono_sm());
        uw = ImGui::CalcTextSize("UNLOCK").x + 12.0f;   // pad_x 6 each side
        ImGui::PopFont();
    }

    // Toggle hit target - kept clear of the UNLOCK chip so clicks never overlap.
    const float toggle_w = show_unlock ? (ww - uw - PADX * 2.0f) : ww;
    ImGui::SetCursorScreenPos(org);
    ImGui::InvisibleButton("##hdr", ImVec2(toggle_w > 1.0f ? toggle_w : 1.0f, H));
    if (ImGui::IsItemClicked()) section_open_[s] = !section_open_[s];

    if (show_unlock) {
        const ImVec2 up(org.x + ww - PADX - uw, org.y + H * 0.5f - 8.0f);
        ImGui::SetCursorScreenPos(up);
        ImGui::InvisibleButton("##unlock", ImVec2(uw, 16.0f));
        if (ImGui::IsItemClicked())
            ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Generic);
    }

    // Visuals
    dl->AddLine(org, ImVec2(org.x + ww, org.y), col(Tokens::BD1), 1.0f);   // top border
    const float cy = org.y + H * 0.5f, cx = org.x + PADX;
    const ImU32 c3 = col(Tokens::TX3);
    if (section_open_[s])
        dl->AddTriangleFilled(ImVec2(cx, cy - 2.0f), ImVec2(cx + 8.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy + 3.0f), c3);
    else
        dl->AddTriangleFilled(ImVec2(cx, cy - 4.0f), ImVec2(cx, cy + 4.0f), ImVec2(cx + 5.0f, cy), c3);

    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(cx + 15.0f, cy - ImGui::GetFontSize() * 0.5f), col(Tokens::TX2), name);
    const float nw = ImGui::CalcTextSize(name).x;
    ImGui::PopFont();

    if (pro_section)
        draw_tag(dl, ImVec2(cx + 15.0f + nw + 8.0f, cy - 7.0f), "PRO",
                 0, col(Tokens::BRAND_LINE), col(Tokens::BRAND_TX), 4.0f);
    if (show_unlock)
        draw_tag(dl, ImVec2(org.x + ww - PADX - uw, cy - 8.0f), "UNLOCK",
                 col(Tokens::BRAND), 0, col(Tokens::BRAND_INK), 6.0f);

    ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + H));
    ImGui::PopID();
    return section_open_[s];
}

void StatsWidget::render_settings_row() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float H = 30.0f, PADX = 12.0f;

    ImGui::SetCursorScreenPos(org);
    ImGui::InvisibleButton("##settings_row", ImVec2(ww > 1.0f ? ww : 1.0f, H));
    if (ImGui::IsItemClicked()) settings_open_ = !settings_open_;

    dl->AddLine(org, ImVec2(org.x + ww, org.y), col(Tokens::BD1), 1.0f);
    const float cy = org.y + H * 0.5f, cx = org.x + PADX;
    const ImU32 c3 = col(Tokens::TX3);
    if (settings_open_)
        dl->AddTriangleFilled(ImVec2(cx, cy - 2.0f), ImVec2(cx + 8.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy + 3.0f), c3);
    else
        dl->AddTriangleFilled(ImVec2(cx, cy - 4.0f), ImVec2(cx, cy + 4.0f), ImVec2(cx + 5.0f, cy), c3);
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(cx + 15.0f, cy - ImGui::GetFontSize() * 0.5f), col(Tokens::TX2), "SETTINGS");
    ImGui::PopFont();
    // TODO(step 3): expanded body - density, sparklines, OI/VPIN window, collapse-all.

    ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + H));
}

// ── Section-body primitives (step 2) ────────────────────────────────────────
namespace {

void meter_track(ImDrawList* dl, float x, float y, float w, ImU32 c) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 4.0f), c, 1.0f);
}

// Proportional two-sided: left = a (up), right = 1-a (down), 1px gap.
void meter_proportional(ImDrawList* dl, float x, float y, float w, float a,
                        ImU32 c_up, ImU32 c_down) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    const float lw = (w - 1.0f) * a;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + lw, y + 4.0f), c_up, 1.0f);
    dl->AddRectFilled(ImVec2(x + lw + 1.0f, y), ImVec2(x + w, y + 4.0f), c_down, 1.0f);
}

// Directional center-zero: v in [-1,1]; fill from center, up right / down left.
void meter_directional(ImDrawList* dl, float x, float y, float w, float v,
                       ImU32 c_up, ImU32 c_down, ImU32 track, ImU32 tick) {
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    meter_track(dl, x, y, w, track);
    const float cxp = x + w * 0.5f;
    if (v >= 0.0f) dl->AddRectFilled(ImVec2(cxp, y), ImVec2(cxp + (w * 0.5f) * v, y + 4.0f), c_up, 1.0f);
    else           dl->AddRectFilled(ImVec2(cxp + (w * 0.5f) * v, y), ImVec2(cxp, y + 4.0f), c_down, 1.0f);
    dl->AddLine(ImVec2(cxp, y - 2.0f), ImVec2(cxp, y + 6.0f), tick, 1.0f);
}

// Fill 0..1.
void meter_fill(ImDrawList* dl, float x, float y, float w, float v, ImU32 track, ImU32 fill) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    meter_track(dl, x, y, w, track);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * v, y + 4.0f), fill, 1.0f);
}

// Amber regime chip; level 0 normal / 1 elevated / 2 high / 3 critical.
void draw_regime_chip(ImDrawList* dl, ImVec2 pos, const char* txt, int level) {
    ImGui::PushFont(Fonts::mono_sm());
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const ImVec2 b(pos.x + ts.x + 12.0f, pos.y + ts.y + 3.0f);
    ImU32 border = col(Tokens::BD2), bg = 0, fg = col(Tokens::TX3);
    if (level == 1)      { border = col(Tokens::WARN); fg = col(Tokens::WARN); }
    else if (level == 2) { border = col(Tokens::WARN); bg = col(Tokens::WARN_SOFT); fg = col(Tokens::WARN); }
    else if (level >= 3) { border = col(Tokens::WARN); bg = col(Tokens::WARN); fg = col(Tokens::BRAND_INK); }
    if (bg) dl->AddRectFilled(pos, b, bg, 2.0f);
    dl->AddRect(pos, b, border, 2.0f, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + 6.0f, pos.y + 1.5f), fg, txt);
    ImGui::PopFont();
}

// Tiny padlock glyph (~9px) at top-left p: body + shackle.
void draw_lock(ImDrawList* dl, ImVec2 p, ImU32 c) {
    dl->AddRectFilled(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x + 8.0f, p.y + 10.0f), c, 1.0f);
    dl->AddRect(ImVec2(p.x + 1.5f, p.y), ImVec2(p.x + 6.5f, p.y + 5.0f), c, 0.0f, 0, 1.2f);
}

void fmt_compact(double v, char* buf, size_t n) {
    const double a = v < 0.0 ? -v : v;
    if (a >= 1e9)      snprintf(buf, n, "%.2fB", v / 1e9);
    else if (a >= 1e6) snprintf(buf, n, "%.2fM", v / 1e6);
    else if (a >= 1e3) snprintf(buf, n, "%.1fK", v / 1e3);
    else               snprintf(buf, n, "%.0f", v);
}

void fmt_usd(double v, char* buf, size_t n) {
    char t[24];
    fmt_compact(v, t, sizeof(t));
    snprintf(buf, n, "$%s", t);
}

}  // namespace

void StatsWidget::metric_row(const Row& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 12.0f;
    const bool locked = r.pro && !Entitlements::is_pro();
    const bool has_meter  = r.meter != 0;
    const bool has_detail = r.detail != nullptr;

    const float top = org.y + 7.0f;

    // Label (always crisp - the value proposition stays readable when locked)
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(org.x + PADX, top + 1.0f), col(Tokens::TX3), r.label);
    ImGui::PopFont();

    float y2 = top + 18.0f;

    if (locked) {
        ImVec4 bar = Tokens::BD2; bar.w = 0.45f;
        dl->AddRectFilled(ImVec2(org.x + ww - PADX - 96.0f, top),
                          ImVec2(org.x + ww - PADX - 16.0f, top + 12.0f), col(bar), 2.0f);
        draw_lock(dl, ImVec2(org.x + ww - PADX - 9.0f, top + 1.0f), col(Tokens::TX3));
        if (has_meter) {
            ImVec4 mt = Tokens::BD2; mt.w = 0.35f;
            dl->AddRectFilled(ImVec2(org.x + PADX, y2), ImVec2(org.x + ww - PADX, y2 + 4.0f), col(mt), 1.0f);
            y2 += 9.0f;
        }
        if (has_detail) y2 += 13.0f;   // keep the row height stable vs the unlocked state
    } else {
        // Value (right-aligned, mono)
        ImGui::PushFont(Fonts::mono());
        const float vw = ImGui::CalcTextSize(r.value).x;
        const float vx = org.x + ww - PADX - vw;
        dl->AddText(ImVec2(vx, top), col(r.value_col), r.value);
        ImGui::PopFont();

        // Chip before the value
        if (r.chip) {
            ImGui::PushFont(Fonts::mono_sm());
            const float cw = ImGui::CalcTextSize(r.chip).x + 12.0f;
            ImGui::PopFont();
            draw_regime_chip(dl, ImVec2(vx - cw - 8.0f, top - 1.0f), r.chip, r.chip_level);
        }

        // Meter, then detail
        if (has_meter) {
            const float mx = org.x + PADX, mw = ww - PADX * 2.0f;
            if (r.meter == 1)
                meter_proportional(dl, mx, y2, mw, r.meter_a, col(r.meter_c1), col(r.meter_c2));
            else if (r.meter == 2)
                meter_directional(dl, mx, y2, mw, r.meter_a, col(Tokens::UP), col(Tokens::DOWN),
                                  col(Tokens::ELEV), col(Tokens::BD2));
            else
                meter_fill(dl, mx, y2, mw, r.meter_a, col(Tokens::ELEV), col(r.meter_c1));
            y2 += 9.0f;
        }
        if (has_detail) {
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImVec2(org.x + PADX, y2), col(r.detail_col), r.detail);
            ImGui::PopFont();
            y2 += 13.0f;
        }
    }

    // Submit a real item (not just a cursor move) so the scroll child grows its
    // content size / boundaries - otherwise ImGui warns and can't size/scroll.
    ImGui::Dummy(ImVec2(ww, (y2 + 7.0f) - org.y));
}

void StatsWidget::section_subhead(const char* txt) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(Fonts::label());
    dl->AddText(ImVec2(org.x + 12.0f, org.y + 6.0f), col(Tokens::TX3), txt);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ww, 20.0f));
}

// One honest line instead of a panel of dashes. Keyed on frame absence, not
// entitlements (bare mode defaults to Pro, so an entitlement check cannot
// tell a self-hosted feed from the hosted one); see stream_presence.h.
void StatsWidget::feed_note() {
    if (!StreamPresence::instance().absent(
            static_cast<uint32_t>(Terminal::Stream::PositioningState))) return;
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 12.0f;
    ImGui::PushFont(Fonts::label());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::SetCursorScreenPos(ImVec2(org.x + PADX, org.y + 4.0f));
    // PushTextWrapPos is window-local, not screen-space.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ww - PADX * 2.0f);
    ImGui::TextUnformatted(
        "The analytics rows read from EdgeDepth's hosted feed. "
        "This feed has not delivered those streams, so they stay empty. "
        "Price, book, tape and liquidations are live.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ww, 6.0f));
}

// ── Order flow (premium core; leads) ────────────────────────────────────────
void StatsWidget::render_order_flow() {
    feed_note();
    const PositioningState* pos = ctx_.analytics_mgr().get_positioning(pair_.symbol);

    // VPIN - latest finalized volume-bucket print from the SeriesCache (already
    // parsed for the chart). Value + regime chip + fill gauge (amber). PRO.
    {
        const auto& series = ctx_.series_mgr().vpin(pair_.symbol);
        char val[16] = "--";
        const char* chip = "--"; int chip_lvl = 0; float mval = 0.0f;
        if (!series.empty()) {
            const Series::VPINPoint& p = series.back();
            snprintf(val, sizeof(val), "%.2f", p.vpin);
            mval = p.vpin;
            static const char* const kRegime[4] = { "NORMAL", "ELEVATED", "HIGH", "CRITICAL" };
            const int r = (p.regime >= 0 && p.regime <= 3) ? p.regime : 0;
            chip = kRegime[r]; chip_lvl = r;
        }
        metric_row(Row{ .label = "VPIN", .pro = true, .value = val,
                        .meter = 3, .meter_a = mval, .meter_c1 = Tokens::WARN,
                        .chip = chip, .chip_level = chip_lvl });
    }
    // CVD (session) + taker buy/sell pressure - from PositioningStateUpdate. PRO.
    {
        char val[24] = "--"; char det[48] = "BUY -- \xc2\xb7 SELL --";
        float mval = 0.5f; ImVec4 vcol = Tokens::TX1;
        if (pos) {
            char c[20]; fmt_compact(pos->cvd, c, sizeof(c));
            snprintf(val, sizeof(val), "%s%s", pos->cvd > 0.0 ? "+" : "", c);
            vcol = pos->cvd >= 0.0 ? Tokens::UP : Tokens::DOWN;
            const double sum = pos->taker_buy_ratio + pos->taker_sell_ratio;
            mval = sum > 0.0 ? static_cast<float>(pos->taker_buy_ratio / sum) : 0.5f;
            snprintf(det, sizeof(det), "BUY %.0f%% \xc2\xb7 SELL %.0f%%",
                     mval * 100.0f, (1.0f - mval) * 100.0f);
        }
        metric_row(Row{ .label = "CVD (SESSION)", .pro = true, .value = val, .value_col = vcol,
                        .detail = det, .meter = 1, .meter_a = mval });
    }
    // Trades / sec - LIVE from the stat stream (buys/sells/imbalance). FREE.
    {
        const long long b = static_cast<long long>(current_stat_.trade_buy);
        const long long s = static_cast<long long>(current_stat_.trade_sell);
        const double tot = static_cast<double>(b + s);
        const double imb = tot > 0.0 ? static_cast<double>(b - s) / tot : 0.0;
        char val[24]; snprintf(val, sizeof(val), "%.0f/s", tps_);
        char bc[16], sc[16];
        fmt_compact(static_cast<double>(b), bc, sizeof(bc));
        fmt_compact(static_cast<double>(s), sc, sizeof(sc));
        char det[72];
        snprintf(det, sizeof(det), "BUYS %s \xc2\xb7 SELLS %s \xc2\xb7 IMB %+.0f%%", bc, sc, imb * 100.0);
        metric_row(Row{ .label = "TRADES / SEC", .value = val, .detail = det });
    }
    // Orderbook imbalance - PLACEHOLDER: bid/ask-heavy + persistence. PRO.
    metric_row(Row{ .label = "ORDERBOOK IMBALANCE", .pro = true, .value = "--",
                    .detail = "-- persistence", .meter = 2 });
    // Smart money - PLACEHOLDER cluster. PRO.
    section_subhead("SMART MONEY");
    // Accumulation bias - live proxy from Positioning.smart_money_bias (the true
    // iceberg-based bias is Tier 2 via SmartMoneyUpdate on the analytics stream).
    {
        char val[16] = "--"; float mval = 0.0f;
        if (pos) {
            snprintf(val, sizeof(val), "%+.2f", pos->smart_money_bias);
            mval = static_cast<float>(pos->smart_money_bias);
        }
        metric_row(Row{ .label = "ACCUMULATION BIAS", .pro = true, .value = val, .meter = 2, .meter_a = mval });
    }
    metric_row(Row{ .label = "ICEBERGS", .pro = true, .value = "-- BUY \xc2\xb7 -- SELL" });
    metric_row(Row{ .label = "SUPPORT / RESIST", .pro = true, .value = "-- / --", .detail = "-- away" });
}

// ── Positioning ─────────────────────────────────────────────────────────────
void StatsWidget::render_positioning() {
    const PositioningState* pos = ctx_.analytics_mgr().get_positioning(pair_.symbol);
    // Funding - LIVE rate + countdown to next funding. FREE, amber.
    {
        char val[24]; snprintf(val, sizeof(val), "%+.4f%%", current_stat_.funding * 100.0);
        char det[32] = "NEXT --:--:--";
        if (current_stat_.next_funding_time > 0) {
            long long ms = current_stat_.next_funding_time
                         - static_cast<long long>(Entitlements::wall_now_ms());  // TODO(step 3): replay clock
            if (ms < 0) ms = 0;
            const long long s = ms / 1000;
            snprintf(det, sizeof(det), "NEXT %02d:%02d:%02d",
                     static_cast<int>(s / 3600), static_cast<int>((s % 3600) / 60), static_cast<int>(s % 60));
        }
        metric_row(Row{ .label = "FUNDING", .value = val, .value_col = Tokens::TX1,
                        .detail = det, .detail_col = Tokens::TX1 });
    }
    // Open interest - LIVE. The stat's open_interest_usd field actually carries
    // CONTRACT QTY (backend note in stat.go), so notional USD = contracts * mark.
    // 1H/4H deltas are PLACEHOLDER (need OI history). FREE.
    {
        const double oi_usd = current_stat_.open_interest_usd * current_stat_.mark_price;
        char val[24];
        if (oi_usd > 0.0) fmt_usd(oi_usd, val, sizeof(val));
        else              snprintf(val, sizeof(val), "--");
        // 1H/4H deltas derived client-side from the OI history ring.
        char det[48] = "1H -- \xc2\xb7 4H --";
        bool h1 = false, h4 = false;
        const double d1 = ctx_.analytics_mgr().oi_change_pct(pair_.symbol, 3600000, &h1);
        const double d4 = ctx_.analytics_mgr().oi_change_pct(pair_.symbol, 14400000, &h4);
        if (h1 || h4) {
            char s1[12] = "--", s4[12] = "--";
            if (h1) snprintf(s1, sizeof(s1), "%+.1f%%", d1 * 100.0);
            if (h4) snprintf(s4, sizeof(s4), "%+.1f%%", d4 * 100.0);
            snprintf(det, sizeof(det), "1H %s \xc2\xb7 4H %s", s1, s4);
        }
        metric_row(Row{ .label = "OPEN INTEREST", .value = val, .detail = det });
    }
    // OI velocity - client-derived signed rate/min from the OI history ring. PRO.
    {
        char val[16] = "--"; ImVec4 vcol = Tokens::TX1;
        bool hv = false;
        const double v = ctx_.analytics_mgr().oi_velocity_per_min(pair_.symbol, &hv);
        if (hv) {
            snprintf(val, sizeof(val), "%+.2f%%/m", v * 100.0);
            vcol = v >= 0.0 ? Tokens::UP : Tokens::DOWN;
        }
        metric_row(Row{ .label = "OI VELOCITY", .pro = true, .value = val, .value_col = vcol });
    }
    // Long / short - global account ratio (retail crowd); long up, short down. FREE.
    {
        char val[24] = "-- / --"; float mval = 0.5f;
        if (pos && pos->global_long_account > 0.0) {
            const float lg = static_cast<float>(pos->global_long_account);
            mval = lg;
            snprintf(val, sizeof(val), "%.0f%% / %.0f%%", lg * 100.0f, (1.0f - lg) * 100.0f);
        }
        metric_row(Row{ .label = "LONG / SHORT", .value = val, .meter = 1, .meter_a = mval });
    }
}

// ── Liquidations & risk (signature) ─────────────────────────────────────────
void StatsWidget::render_liq_risk() {
    const PositioningState* pos = ctx_.analytics_mgr().get_positioning(pair_.symbol);
    // Liquidations 24H - LIVE total + longs/shorts. FREE.
    {
        double ll = pos ? pos->long_liq_usd : 0.0, ls = pos ? pos->short_liq_usd : 0.0;
        if (ll <= 0.0 && ls <= 0.0) { ll = current_stat_.liq_long_usd; ls = current_stat_.liq_short_usd; }
        char val[24]; fmt_usd(ll + ls, val, sizeof(val));
        char lc[16], sc[16];
        fmt_usd(ll, lc, sizeof(lc));
        fmt_usd(ls, sc, sizeof(sc));
        char det[64]; snprintf(det, sizeof(det), "LONGS %s \xc2\xb7 SHORTS %s", lc, sc);
        metric_row(Row{ .label = "LIQUIDATIONS 24H", .value = val, .detail = det });
    }
    // Nearest wall - PLACEHOLDER: cluster price + distance/leverage. PRO.
    metric_row(Row{ .label = "NEAREST WALL", .pro = true, .value = "--", .detail = "-- away \xc2\xb7 --x" });
    // Cascade risk - LIVE from Positioning.cascade_risk (0..1). Word + gauge. PRO.
    {
        char val[16] = "--"; float mval = 0.0f; ImVec4 vcol = Tokens::TX1;
        if (pos) {
            const double cr = pos->cascade_risk;
            mval = static_cast<float>(cr);
            snprintf(val, sizeof(val), "%s",
                     cr >= 0.85 ? "EXTREME" : cr >= 0.66 ? "HIGH" : cr >= 0.33 ? "MODERATE" : "LOW");
            if (cr >= 0.66) vcol = Tokens::WARN;   // amber word only when actually high
        }
        metric_row(Row{ .label = "CASCADE RISK", .pro = true, .value = val, .value_col = vcol,
                        .meter = 3, .meter_a = mval, .meter_c1 = Tokens::WARN });
    }
    // Market stress (contagion) - replay-only today (no live stream); "--" live. PRO.
    {
        char val[16] = "--"; char det[24] = "CONTAGION --"; float mval = 0.0f; ImVec4 vcol = Tokens::TX1;
        const ContagionState* cg = ctx_.analytics_mgr().get_contagion();
        if (cg) {
            const double ms = cg->market_stress;
            mval = static_cast<float>(ms);
            snprintf(val, sizeof(val), "%s",
                     ms >= 0.85 ? "CRISIS" : ms >= 0.5 ? "STRESS" : ms >= 0.25 ? "ELEVATED" : "CALM");
            snprintf(det, sizeof(det), "CONTAGION %.2f", ms);
            if (ms >= 0.5) vcol = Tokens::WARN;
        }
        metric_row(Row{ .label = "MARKET STRESS", .pro = true, .value = val, .value_col = vcol,
                        .detail = det, .meter = 3, .meter_a = mval, .meter_c1 = Tokens::WARN });
    }
}
