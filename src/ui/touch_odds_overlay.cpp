#include "ui/touch_odds_overlay.h"
#include "core/display_time_zone.h"
#include "core/research_url.h"
#include "core/touch_odds.h"
#include "rendering/theme.h"
#include "implot.h"
#include <algorithm>
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace touch_odds_overlay {
namespace {
touch_odds::Snapshot snapshot;
bool have = false;
bool hosted = false;  // the web host mounts the bridge for admins only (server-side role check)
bool zone_hovered = false;  // last frame's zone-label hover: the cursor readout yields to its tooltip
void read() {
#ifdef __EMSCRIPTEN__
    static int frame = -1, rev = -1;
    if (frame == ImGui::GetFrameCount()) return;
    frame = ImGui::GetFrameCount();
    // Main-thread bridge shared by all charts; the host bumps the revision on every publish.
    const int next = emscripten_run_script_int("window.__EDGEDEPTH_REACH_REV__|0");
    if (next == rev) return;
    rev = next;
    hosted = rev > 0;
    const char* raw = emscripten_run_script_string("JSON.stringify(window.__EDGEDEPTH_REACH__ || null)");
    const auto j = nlohmann::json::parse(raw ? raw : "null", nullptr, false);
    have = touch_odds::parse(j, snapshot);
#endif
}
bool matches(const Terminal::Pair& pair) {
    return have && hosted && pair.exchange == "binancef"
        && research_url::normalize_symbol(pair.symbol) == snapshot.symbol;
}
void hhmm(int64_t ms, char* out, size_t size) {
    if (!DisplayTimeZone::instance().format(ms, TimeZoneFormat::TimeMinutes, out, size)) std::snprintf(out, size, "--:--");
}
void cascade_chip(const ImVec2& plot, int64_t clock) {
    using namespace touch_odds;
    char text[96];
    const bool tier = snapshot.tier_at(clock);
    const char* reason = missing_reason(snapshot.status);
    if (tier) std::snprintf(text, sizeof(text), "CASCADE RISK 1H \xc2\xb7 %s", snapshot.tier == Tier::Top1 ? "TOP 1%" : snapshot.tier == Tier::Top5 ? "TOP 5%" : "NOT IN TOP 5%");
    else std::snprintf(text, sizeof(text), "CASCADE RISK 1H \xc2\xb7 UNAVAILABLE");
    const ImVec4 fg = tier && snapshot.tier == Tier::Top1 ? Theme::Tokens::WARN : tier && snapshot.tier == Tier::Top5 ? Theme::Tokens::TX1 : Theme::Tokens::TX3;
    auto* dl = ImPlot::GetPlotDrawList();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 tsz = ImGui::CalcTextSize(text);
    const ImVec2 a(plot.x + 10.0f, plot.y + 30.0f), b(a.x + 18.0f + tsz.x, a.y + 22.0f);
    dl->AddRectFilled(a, b, Theme::u32(fg, 0.12f), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(fg, 0.40f), Theme::Radius::R2, 0, 1.0f);
    dl->AddText(ImVec2(a.x + 9.0f, (a.y + b.y - tsz.y) * 0.5f), Theme::u32(fg), text);
    float right = b.x;
    if (reason && snapshot.status != "no_current_frame") {
        char note[96];
        std::snprintf(note, sizeof(note), "Touch odds: %s", reason);
        dl->AddText(ImVec2(right + 10.0f, (a.y + b.y - tsz.y) * 0.5f), Theme::u32(Theme::Tokens::TX3), note);
    }
    ImGui::PopFont();
    const auto mouse = ImGui::GetMousePos();
    if (!ImPlot::IsPlotHovered() || mouse.x < a.x || mouse.x > right || mouse.y < a.y || mouse.y > b.y) return;
    Theme::begin_tooltip();
    if (tier) {
        char from[16], to[16];
        hhmm(snapshot.hour_start, from, sizeof(from)); hhmm(snapshot.hour_end, to, sizeof(to));
        ImGui::Text("P9 cascade tier for %s to %s.", from, to);
        ImGui::TextUnformatted("Top 1% / top 5%: this market's forecast chance of a cascade in the hour (price down 5%+");
        ImGui::TextUnformatted("in 15 min while open interest falls 3%+) is above P9's frozen cutoff for the riskiest");
        ImGui::TextUnformatted("1% / 5% of market-hours in testing.");
        ImGui::TextUnformatted("Not a direction call. Research preview; forward test first look 1 Dec 2026.");
    } else {
        ImGui::TextUnformatted("No current P9 tier: outside the fitted market set, or this hour is not published yet.");
    }
    Theme::end_tooltip();
}
}  // namespace

bool available(const Terminal::Pair& pair) {
    read();
    return hosted && pair.exchange == "binancef";
}

int64_t window_end(const Terminal::Pair& pair, int64_t clock) {
    read();
    return matches(pair) && snapshot.bands_at(clock) ? snapshot.expires_at : 0;
}

void menu(const Terminal::Pair& pair, bool& enabled) {
    if (!available(pair)) return;
    if (Theme::menu_toggle("Touch odds, next 20 min (admin preview)", enabled)) enabled = !enabled;
    if (Theme::begin_menu_group("Touch odds source and limits")) {
        Theme::menu_note("Blue bands: calibrated chance the 5-minute mark price touches each band within 20 minutes of the forecast time. Bands overlap in outcome, so never add them. Beside the cursor: the chance of touching that level within 1h, 4h and 24h, for levels 0.5% to 10% from the live price (the saved mark in replay). Replay horizons keep their original forecast deadlines. Chip: the P9 hourly cascade tier. All are frozen research models that passed historical tests; live-window tests are pending and the layer stays admin-only until they pass.");
        Theme::end_menu_group();
    }
}

// Long-range readout beside the cursor: the frozen 1h/4h/24h touch odds for the hovered level,
// measured from the live price with volatility as of the last frame (0.5% to 10% away only).
void crosshair(const ImVec2& pos, const ImVec2& size, int64_t clock, double live) {
    const auto& lr = snapshot.longrange;
    if (!lr.at(clock) || !(live > 0) || !ImPlot::IsPlotHovered() || zone_hovered) return;
    const double level = ImPlot::GetPlotMousePos().y;
    double p[3];
    for (int k = 0; k < 3; ++k) if ((p[k] = touch_odds::probability(lr, lr.horizons[k], live, level)) < 0) return;
    auto pct = [](double v, char* out, size_t n) {
        if (v < 0.01) std::snprintf(out, n, "<1%%"); else if (v > 0.99) std::snprintf(out, n, ">99%%"); else std::snprintf(out, n, "%.0f%%", 100.0*v);
    };
    char a[8], b[8], c[8], text[96];
    pct(p[0], a, sizeof(a)); pct(p[1], b, sizeof(b)); pct(p[2], c, sizeof(c));
    std::snprintf(text, sizeof(text), "%sTOUCH %+.2f%%  1h %s  4h %s  24h %s", snapshot.history ? "RECORDED " : "", 100.0*(level/live - 1.0), a, b, c);
    auto* dl = ImPlot::GetPlotDrawList();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 tsz = ImGui::CalcTextSize(text), mouse = ImGui::GetMousePos();
    float x = mouse.x + 16.0f;
    if (x + tsz.x + 12.0f > pos.x + size.x) x = mouse.x - 16.0f - tsz.x - 12.0f;
    x = std::clamp(x, pos.x + 2.0f, std::max(pos.x + 2.0f, pos.x + size.x - tsz.x - 14.0f));
    const float y = std::clamp(mouse.y - tsz.y*0.5f - 3.0f, pos.y + 2.0f, pos.y + size.y - tsz.y - 8.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tsz.x + 12.0f, y + tsz.y + 6.0f), Theme::u32(Theme::Tokens::PANEL, 0.94f));
    dl->AddRect(ImVec2(x, y), ImVec2(x + tsz.x + 12.0f, y + tsz.y + 6.0f), Theme::u32(Theme::Tokens::REST, 0.55f), 0.0f, 0, 1.0f);
    dl->AddText(ImVec2(x + 6.0f, y + 3.0f), Theme::u32(Theme::Tokens::TX1), text);
    ImGui::PopFont();
}

void zone_labels(const Terminal::Pair& pair, int64_t clock, double live, const char* price_fmt,
                 const std::vector<touch_zones::Zone>& zones, bool replay) {
    read();
    if (snapshot.history != replay) return;
    if (snapshot.history) live = snapshot.longrange.mark;
    zone_hovered = false;
    const auto& lr = snapshot.longrange;
    if (!matches(pair) || !lr.at(clock) || !(live > 0) || zones.empty()) return;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    auto* dl = ImPlot::GetPlotDrawList();
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImPlot::IsPlotHovered();
    auto pct = [](double v, char* out, size_t n) {
        if (v < 0.01) std::snprintf(out, n, "<1%%"); else if (v > 0.99) std::snprintf(out, n, ">99%%"); else std::snprintf(out, n, "%.0f%%", 100.0*v);
    };
    float used[8]; int n_used = 0;
    const touch_zones::Zone* hover = nullptr; double hover_p[3]{};
    ImPlot::PushPlotClipRect();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (const auto& z : zones) {
        double p[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) ok = ok && (p[k] = touch_odds::probability(lr, lr.horizons[k], live, z.price)) >= 0;
        if (!ok) continue;
        const float y = ImPlot::PlotToPixels(0.0, z.price).y;
        if (y < pos.y + 4 || y > pos.y + size.y - 4) continue;
        char a[8], b[8], c[8], text[64];
        pct(p[2], a, sizeof(a)); pct(p[1], b, sizeof(b)); pct(p[0], c, sizeof(c));
        std::snprintf(text, sizeof(text), "24h %s  4h %s  1h %s", a, b, c);
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        bool overlaps = false;
        for (int k = 0; k < n_used; ++k) overlaps = overlaps || std::abs(used[k] - y) < tsz.y + 6.0f;
        if (overlaps || n_used >= 8) continue;
        const float x1 = pos.x + size.x - 8.0f, x0 = x1 - tsz.x - 12.0f, y0 = y - tsz.y*0.5f - 3.0f, y1 = y + tsz.y*0.5f + 3.0f;
        dl->AddLine(ImVec2(x0 - 10.0f, y), ImVec2(x0, y), Theme::u32(Theme::Tokens::REST, 0.70f), 1.0f);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Theme::u32(Theme::Tokens::PANEL, 0.94f));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Theme::u32(Theme::Tokens::REST, 0.70f), 0.0f, 0, 1.0f);
        dl->AddText(ImVec2(x0 + 6.0f, y - tsz.y*0.5f), Theme::u32(Theme::Tokens::TX1), text);
        used[n_used++] = y;
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1) { hover = &z; std::copy(p, p + 3, hover_p); }
    }
    ImGui::PopFont();
    ImPlot::PopPlotClipRect();
    if (!hover) return;
    zone_hovered = true;
    char price[48], a[8], b[8], c[8];
    std::snprintf(price, sizeof(price), price_fmt, hover->price);
    pct(hover_p[0], c, sizeof(c)); pct(hover_p[1], b, sizeof(b)); pct(hover_p[2], a, sizeof(a));
    Theme::begin_tooltip();
    ImGui::Text("Scenario zone at %s (%+.2f%% from %s)", price, 100.0*(hover->price/live - 1.0), snapshot.history ? "the saved mark" : "the live price");
    ImGui::Text("Chance price touches it: 1h %s, 4h %s, 24h %s.", c, b, a);
    ImGui::TextUnformatted("The zone is estimated liquidation fuel from candles, not observed positions. In our");
    ImGui::TextUnformatted("registered test the zones did not change these odds; they come from volatility alone.");
    ImGui::TextUnformatted("Reaching a zone also did not make a further 1% run more likely in a second test.");
    ImGui::TextUnformatted("Frozen model; live-window test pending. Admin preview.");
    Theme::end_tooltip();
}

void render(const Terminal::Pair& pair, int64_t clock, const char* price_fmt, double live, bool replay) {
    read();
    if (snapshot.history != replay) return;
    if (snapshot.history) live = snapshot.longrange.mark;
    if (!matches(pair)) return;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    ImPlot::PushPlotClipRect();
    cascade_chip(pos, clock);
    if (!snapshot.bands_at(clock)) { crosshair(pos, size, clock, live); ImPlot::PopPlotClipRect(); return; }
    const auto lim = ImPlot::GetPlotLimits();
    const double start = std::max<double>(clock, snapshot.asof);
    if (start < lim.X.Min || start >= lim.X.Max) { ImPlot::PopPlotClipRect(); return; }  // panned away from now
    const float right_edge = pos.x + size.x - 4.0f;
    const float x0 = ImPlot::PlotToPixels(start, 0.0).x;
    // True to time where the margin allows; never narrower than a readable column.
    const float x1 = std::min(right_edge, std::max(ImPlot::PlotToPixels(double(snapshot.expires_at), 0.0).x, x0 + 72.0f));
    if (x1 - x0 < 24.0f) { ImPlot::PopPlotClipRect(); return; }
    auto* dl = ImPlot::GetPlotDrawList();
    const ImVec4 hue = Theme::Tokens::REST;
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImPlot::IsPlotHovered();
    int hover = -1;
    float label_y[6]; int labels = 0;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (int i = 0; i < snapshot.band_count; ++i) {
        const auto& b = snapshot.bands[i];
        float top = ImPlot::PlotToPixels(0.0, b.high).y, bottom = ImPlot::PlotToPixels(0.0, b.low).y;
        if (bottom < pos.y || top > pos.y + size.y) continue;
        if (bottom - top < 2.0f) { const float mid = (top + bottom) * 0.5f; top = mid - 1.0f; bottom = mid + 1.0f; }
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), Theme::u32(hue, float(0.04 + 0.14 * b.p)));
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bottom), Theme::u32(hue, 0.55f), 0.0f, 0, 1.0f);
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= top && mouse.y <= bottom) hover = i;
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * b.p);
        const ImVec2 tsz = ImGui::CalcTextSize(pct);
        const float y = (top + bottom) * 0.5f;
        bool overlaps = false;
        for (int k = 0; k < labels; ++k) overlaps = overlaps || std::abs(label_y[k] - y) < tsz.y + 2.0f;
        if (overlaps || x1 - x0 < tsz.x + 12.0f) continue;
        const ImVec2 at(x0 + 5.0f, y - tsz.y * 0.5f);
        dl->AddRectFilled(ImVec2(at.x - 3.0f, at.y - 1.0f), ImVec2(at.x + tsz.x + 3.0f, at.y + tsz.y + 1.0f), Theme::u32(Theme::Tokens::PANEL, 0.94f));
        dl->AddText(at, Theme::u32(Theme::Tokens::TX1), pct);
        label_y[labels++] = y;
    }
    ImGui::PopFont();
    if (hover < 0) crosshair(pos, size, clock, live);
    ImPlot::PopPlotClipRect();
    if (hover < 0) return;
    const auto& b = snapshot.bands[hover];
    char at[16], low[48], high[48], mark[48];
    hhmm(snapshot.asof, at, sizeof(at));
    std::snprintf(low, sizeof(low), price_fmt, b.low); std::snprintf(high, sizeof(high), price_fmt, b.high); std::snprintf(mark, sizeof(mark), price_fmt, snapshot.mark);
    Theme::begin_tooltip();
    ImGui::Text("%.1f%% chance of touching %s to %s", 100.0 * b.p, low, high);
    ImGui::Text("within 20 minutes of %s (mark %s, %+.2f%% to the near edge).", at, mark, 100.0 * ((b.side > 0 ? b.low : b.high) / snapshot.mark - 1.0));
    ImGui::TextUnformatted("Mark price on 5-minute bars, not exact entry or fills. Bands overlap: never add them.");
    ImGui::TextUnformatted("Frozen model; historical tests passed, live-window test scored 12 Oct 2026. Admin preview.");
    Theme::end_tooltip();
}
}  // namespace touch_odds_overlay
