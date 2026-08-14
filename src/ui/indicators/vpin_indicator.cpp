// ═══════════════════════════════════════════════════════════════════════════════
// vpin_indicator.cpp - Toxicity pane render ("Eras" revamp, 2026-08-13)
//
// Render model (supersedes the SPEC §2 line coloring; James sign-off from the
// mockup round - SPEC/tokens.json mirror pending):
//   · VPIN line = ONE bright ink step-hold stroke (TX1). The trend must read
//     at a glance; per-segment regime coloring chopped it into confetti.
//   · Regime = full-height background WASHES (merged runs, >= ELEVATED only,
//     alpha quantized 0.05/0.07/0.09, NORMAL = void) + a 3px on-axis strip
//     as the crisp edge. Seams land exactly on state-change prints.
//   · Threshold rules .30/.45/.60 at 20% alpha with in-plot labels.
//   · Persistent corner readout: regime word + brain + latest VPIN.
//   · The hold past the last finalized bucket renders at tentative alpha -
//     stale data must not masquerade as a live shelf.
//
// Two render paths (2026-07-04 FPS fix - the pane must cost O(plot width),
// not O(points); per-print draws were ~10-15k prims zoomed out):
//   · EXACT: ≤ ~1.25 points/px - true step-hold shelves per volume-clock
//     print (H then V), tentative-confidence alpha per segment.
//   · DECIMATED: one vertical min-max line per pixel column. Sub-pixel
//     shelves are unreadable anyway; the min-max column preserves spike
//     honesty (no averaging - grain rule).
// Washes + strip are merged into runs (one rect per regime run, both paths).
// ═══════════════════════════════════════════════════════════════════════════════

#include "vpin_indicator.h"
#include "indicator_tokens.h"
#include "rendering/theme.h"
#include "core/stream_presence.h"
#include "types/types.h"

#include <algorithm>
#include <cmath>

namespace Indicators {

namespace {

    // 1px dotted horizontal rule (dash pattern on/off px) in pixel space.
    void dotted_hline(ImDrawList* dl, float x0, float x1, float y,
                      ImU32 col, float on_px, float off_px) {
        const float step = on_px + off_px;
        if (step <= 0.0f || x1 <= x0) return;
        for (float x = x0; x < x1; x += step) {
            dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + on_px, x1), y), col, 1.0f);
        }
    }

    const char* regime_word(int8_t r) {
        switch (r) {
            case 1:  return "ELEVATED";
            case 2:  return "HIGH";
            case 3:  return "CRITICAL";
            default: return "NORMAL";
        }
    }

} // namespace

size_t VPINIndicator::first_visible(double x_min) const {
    // First index with ts >= x_min, then step back one so the shelf that
    // ENTERS the window from the left still draws.
    auto it = std::lower_bound(pts_.begin(), pts_.end(), x_min,
        [](const Series::VPINPoint& p, double x) {
            return static_cast<double>(p.ts_ms) < x;
        });
    size_t idx = static_cast<size_t>(it - pts_.begin());
    return idx > 0 ? idx - 1 : 0;
}

bool VPINIndicator::get_latest_tag_color(ImU32& out_color) const {
    if (pts_.empty()) { out_color = IndiTokens::TAG_NEUTRAL; return true; }
    const int8_t r = pts_.back().regime;
    out_color = (r <= 0 || !regime_coloring_) ? IndiTokens::TAG_NEUTRAL
                                              : IndiTokens::risk_color(r);
    return true;
}

void VPINIndicator::render_settings() {
    // §4.5 settings - F2 popover pilot. Persisted blob is S4 (F2 full).
    ImGui::Checkbox("Regime display", &regime_coloring_);
    if (ImGui::IsItemHovered())
        Theme::tooltip("Show toxicity regimes as background washes + the on-axis strip\n(HMM when available, threshold fallback otherwise)");
    ImGui::Checkbox("Imbalance line", &show_imbalance_);
    if (ImGui::IsItemHovered())
        Theme::tooltip("Order-imbalance companion (per completed volume bucket),\nstep-hold, drawn in the pane's secondary slate style");
}

void VPINIndicator::render_content(double x_min, double x_max) {
    // SPEC §2 axis: gridlines at 0.25/0.50/0.75 (limits fixed 0-1 via
    // get_y_limits). Setup calls are legal until the first draw.
    static const double kTicks[] = {0.0, 0.25, 0.50, 0.75, 1.00};
    ImPlot::SetupAxisTicks(ImAxis_Y1, kTicks, 5);

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const float px_left  = plot_pos.x;
    const float px_right = plot_pos.x + plot_size.x;
    const float px_bot   = plot_pos.y + plot_size.y;

    // ── Dotted threshold landmarks (.30/.45/.60) - drawn always, named.
    //    At the old 9% alpha the line floated in an unlabeled 0-1 void. ──
    {
        const float ys[3] = {
            ImPlot::PlotToPixels(0.0, IndiTokens::TOX_THRESHOLD_1).y,
            ImPlot::PlotToPixels(0.0, IndiTokens::TOX_THRESHOLD_2).y,
            ImPlot::PlotToPixels(0.0, IndiTokens::TOX_THRESHOLD_3).y,
        };
        const ImU32 rule_col =
            IndiTokens::mul_alpha(IndiTokens::RISK_NORMAL, IndiTokens::TOX_RULE_ALPHA);
        for (float y : ys) {
            dotted_hline(dl, px_left, px_right, y, rule_col,
                         IndiTokens::TOX_THRESH_DASH_ON, IndiTokens::TOX_THRESH_DASH_OFF);
        }
        static const char* const kLbl[3] = {"0.30", "0.45", "0.60"};
        ImGui::PushFont(Theme::Fonts::label());
        const float fh = ImGui::GetFontSize();
        for (int t = 0; t < 3; ++t) {
            dl->AddText(ImVec2(px_left + 4.0f, ys[t] - fh - 1.0f),
                        IndiTokens::INK_FAINT, kLbl[t]);
        }
        ImGui::PopFont();
    }

    if (pts_.empty()) {
        // Distinguish "no prints yet" from "this feed never carries VPIN": on
        // a raw self-hosted feed the pane would otherwise sit silently blank
        // forever (see stream_presence.h).
        if (StreamPresence::instance().absent(
                static_cast<uint32_t>(Terminal::Stream::VPINState))) {
            const char* msg = "VPIN reads from EdgeDepth's hosted analytics. "
                              "This feed has not delivered it.";
            ImGui::PushFont(Theme::Fonts::label());
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(px_left + (px_right - px_left - ts.x) * 0.5f,
                               plot_pos.y + (plot_size.y - ts.y) * 0.5f),
                        ImGui::GetColorU32(Theme::Tokens::TX3), msg);
            ImGui::PopFont();
        }
        ImPlot::PopPlotClipRect();
        return;
    }

    const size_t i0 = first_visible(x_min);
    size_t i_end = i0;
    while (i_end < pts_.size() &&
           static_cast<double>(pts_[i_end].ts_ms) <= x_max) ++i_end;
    const size_t n_vis = i_end - i0;

    const float strip_top = px_bot - IndiTokens::TOX_STRIP_H_V2_PX;

    // ── Regime RUNS, merged (one rect per regime/era run) ──
    // NORMAL draws nothing but still breaks runs; seams land exactly on
    // state-change prints (SPEC §2 - the seams ARE the transitions).
    // Shared by the full-height washes and the on-axis strip; wash alpha is
    // quantized per regime and halved in threshold-fallback eras.
    auto draw_regime_runs = [&](float y_top, float y_bot, bool wash) {
        int8_t run_regime = -1;
        bool   run_era    = false;
        float  run_x0     = 0.0f;
        auto run_alpha = [&](int8_t r, bool era) -> float {
            if (wash) {
                const float a = (r == 1) ? IndiTokens::TOX_WASH_ELEVATED
                              : (r == 2) ? IndiTokens::TOX_WASH_HIGH
                                         : IndiTokens::TOX_WASH_CRITICAL;
                return era ? a : a * IndiTokens::TOX_WASH_FALLBACK;
            }
            return IndiTokens::TOX_STRIP_ALPHA *
                   (era ? 1.0f : IndiTokens::TOX_FALLBACK_MUL);
        };
        auto flush_run = [&](float x1) {
            if (run_regime >= 1 && x1 > run_x0) {
                dl->AddRectFilled(ImVec2(run_x0, y_top), ImVec2(x1, y_bot),
                                  IndiTokens::mul_alpha(IndiTokens::risk_color(run_regime),
                                                        run_alpha(run_regime, run_era)));
            }
        };
        for (size_t i = i0; i < i_end; ++i) {
            const auto& p = pts_[i];
            const bool era = (p.hmm_state >= 0);
            const float x  = ImPlot::PlotToPixels(
                std::max(static_cast<double>(p.ts_ms), x_min), 0.0).x;
            if (i == i0) {
                run_regime = p.regime; run_era = era; run_x0 = x;
            } else if (p.regime != run_regime || era != run_era) {
                flush_run(x);
                run_regime = p.regime; run_era = era; run_x0 = x;
            }
        }
        const float x_last_edge = ImPlot::PlotToPixels(x_max, 0.0).x;
        flush_run(x_last_edge);  // final run extends to the right edge
    };

    // Regime eras: full-height washes UNDER everything, strip as the crisp
    // edge. The V1 "no bg-tint" rejection assumed 3 stacked panes under a hot
    // Field; with restraint (>= ELEVATED only, alpha <= 0.09) the wash is the
    // regime-at-a-glance layer the 2px strip never managed to be.
    if (regime_coloring_) {
        draw_regime_runs(plot_pos.y, px_bot, true);
        draw_regime_runs(strip_top, px_bot, false);
    }

    // ── THRESH → HMM changeover seam (rendered once, both paths) ──
    for (size_t i = i0; i + 1 < i_end; ++i) {
        if (pts_[i].hmm_state < 0 && pts_[i + 1].hmm_state >= 0) {
            const float sx = ImPlot::PlotToPixels(
                static_cast<double>(pts_[i + 1].ts_ms), 0.0).x;
            if (sx >= px_left && sx <= px_right) {
                dl->AddLine(ImVec2(sx, px_bot - 14.0f), ImVec2(sx, px_bot),
                            IndiTokens::mul_alpha(IndiTokens::INK_DIM, 0.5f), 1.0f);
                ImGui::PushFont(Theme::Fonts::label());
                // ASCII only - the U+2192 arrow is not in the label atlas
                // (rendered "?" in the 2026-07-04 screenshot round).
                const char* lhs = "THRESH";
                const ImVec2 lw = ImGui::CalcTextSize(lhs);
                dl->AddText(ImVec2(sx - lw.x - 4.0f, px_bot - 14.0f),
                            IndiTokens::INK_FAINT, lhs);
                dl->AddText(ImVec2(sx + 4.0f, px_bot - 14.0f),
                            IndiTokens::INK_DIM, "HMM >");
                ImGui::PopFont();
            }
            break;
        }
    }

    // ── VPIN line - exact step-hold vs pixel-column decimation ──
    const bool decimate = n_vis > static_cast<size_t>(plot_size.x * 1.25f) &&
                          plot_size.x > 0.0f;

    // One bright ink stroke; regime lives in the washes + strip. The
    // tentative-confidence quantized alpha survives (DECISIONS §2.2).
    auto stroke_color = [&](const Series::VPINPoint& p) -> ImU32 {
        ImU32 c = Theme::u32(Theme::Tokens::TX1);
        if (p.hmm_state >= 0 && p.hmm_conf < IndiTokens::TENTATIVE_CONF)
            c = IndiTokens::mul_alpha(c, IndiTokens::TENTATIVE_ALPHA);
        return c;
    };

    if (!decimate) {
        for (size_t i = i0; i < i_end; ++i) {
            const auto& p = pts_[i];
            const double t0 = static_cast<double>(p.ts_ms);
            const bool   last = (i + 1 >= pts_.size());
            const double t1 = last ? x_max
                                   : std::min(static_cast<double>(pts_[i + 1].ts_ms), x_max);
            ImU32 col = stroke_color(p);
            // The hold past the LAST finalized bucket is not a print - render
            // it tentative instead of masquerading as a live shelf.
            if (last)
                col = IndiTokens::mul_alpha(col, IndiTokens::TENTATIVE_ALPHA);
            const ImVec2 a = ImPlot::PlotToPixels(std::max(t0, x_min), static_cast<double>(p.vpin));
            const ImVec2 b = ImPlot::PlotToPixels(t1,                  static_cast<double>(p.vpin));
            dl->AddLine(a, b, col, 1.0f);                       // H shelf
            if (!last && i + 1 < i_end) {                        // V riser at the next print
                const ImVec2 c = ImPlot::PlotToPixels(t1, static_cast<double>(pts_[i + 1].vpin));
                dl->AddLine(b, c, col, 1.0f);
            }
        }
        // Shelf entering from the left when the next visible print is far
        // right of i_end-1 handled implicitly; final shelf handled above.
    } else {
        // One min-max vertical per pixel column; column alpha = the LAST
        // print's tentative state (no blending). Connect columns with the
        // running level so flat stretches still read as a line.
        float  col_x   = 0.0f;
        float  col_min = 0.0f, col_max = 0.0f;
        float  col_last_y = 0.0f;
        ImU32  col_col = 0;
        bool   have_col = false;
        float  prev_x = 0.0f, prev_y = 0.0f;
        bool   have_prev = false;

        auto flush_col = [&]() {
            if (!have_col) return;
            if (have_prev && col_x > prev_x + 0.5f) {
                // horizontal hold between columns (gap = quiet stretch)
                dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(col_x, prev_y), col_col, 1.0f);
            }
            dl->AddLine(ImVec2(col_x, col_min), ImVec2(col_x, col_max), col_col, 1.0f);
            prev_x = col_x; prev_y = col_last_y; have_prev = true;
        };

        for (size_t i = i0; i < i_end; ++i) {
            const auto& p = pts_[i];
            const ImVec2 pp = ImPlot::PlotToPixels(
                std::max(static_cast<double>(p.ts_ms), x_min),
                static_cast<double>(p.vpin));
            const float x = std::floor(pp.x);
            if (!have_col || x != col_x) {
                flush_col();
                col_x = x; col_min = pp.y; col_max = pp.y;
                have_col = true;
            } else {
                col_min = std::min(col_min, pp.y);
                col_max = std::max(col_max, pp.y);
            }
            col_last_y = pp.y;
            col_col = stroke_color(p);
        }
        flush_col();
        // Final shelf: hold the last value to the right edge, tentative.
        if (have_prev) {
            const float xr = ImPlot::PlotToPixels(x_max, 0.0).x;
            if (xr > prev_x)
                dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(xr, prev_y),
                            IndiTokens::mul_alpha(col_col, IndiTokens::TENTATIVE_ALPHA), 1.0f);
        }
    }

    // ── Order-imbalance companion (setting, default off) - step-hold in
    //    the pane's secondary slate; decimated the same way. ──
    if (show_imbalance_) {
        if (!decimate) {
            for (size_t i = i0; i < i_end; ++i) {
                const auto& p = pts_[i];
                const double t0 = static_cast<double>(p.ts_ms);
                const bool   last = (i + 1 >= pts_.size());
                const double t1 = last ? x_max
                                       : std::min(static_cast<double>(pts_[i + 1].ts_ms), x_max);
                const ImVec2 a = ImPlot::PlotToPixels(std::max(t0, x_min),
                                                      static_cast<double>(p.imbalance));
                const ImVec2 b = ImPlot::PlotToPixels(t1, static_cast<double>(p.imbalance));
                dl->AddLine(a, b, IndiTokens::CFTI_LINE, 1.0f);
                if (!last && i + 1 < i_end) {
                    const ImVec2 c = ImPlot::PlotToPixels(t1,
                        static_cast<double>(pts_[i + 1].imbalance));
                    dl->AddLine(b, c, IndiTokens::CFTI_LINE, 1.0f);
                }
            }
        } else {
            float col_x = 0.0f, col_min = 0.0f, col_max = 0.0f;
            bool  have_col = false;
            for (size_t i = i0; i < i_end; ++i) {
                const auto& p = pts_[i];
                const ImVec2 pp = ImPlot::PlotToPixels(
                    std::max(static_cast<double>(p.ts_ms), x_min),
                    static_cast<double>(p.imbalance));
                const float x = std::floor(pp.x);
                if (!have_col || x != col_x) {
                    if (have_col)
                        dl->AddLine(ImVec2(col_x, col_min), ImVec2(col_x, col_max),
                                    IndiTokens::CFTI_LINE, 1.0f);
                    col_x = x; col_min = pp.y; col_max = pp.y; have_col = true;
                } else {
                    col_min = std::min(col_min, pp.y);
                    col_max = std::max(col_max, pp.y);
                }
            }
            if (have_col)
                dl->AddLine(ImVec2(col_x, col_min), ImVec2(col_x, col_max),
                            IndiTokens::CFTI_LINE, 1.0f);
        }
    }

    // ── Persistent corner readout next to the pane name - regime word +
    //    brain + latest VPIN without needing a hover (regime was hover-only).
    //    Mono font; regime word in regime color. ──
    if (!pts_.empty()) {
        const auto& lp = pts_.back();
        const ImU32 word_col = (regime_coloring_ && lp.regime >= 1)
            ? IndiTokens::risk_color(lp.regime) : IndiTokens::INK;
        char rest[64];
        if (lp.hmm_state >= 0)
            snprintf(rest, sizeof(rest), " \xC2\xB7 HMM %.2f \xC2\xB7 VPIN %.4f",
                     lp.hmm_conf, lp.vpin);
        else
            snprintf(rest, sizeof(rest), " \xC2\xB7 THRESH \xC2\xB7 VPIN %.4f", lp.vpin);
        ImGui::PushFont(Theme::Fonts::label());
        const float name_w = ImGui::CalcTextSize(get_name()).x;
        ImGui::PopFont();
        ImGui::PushFont(Theme::Fonts::mono_sm());
        const char* word = regime_word(lp.regime);
        const ImVec2 base(px_left + 6.0f + name_w + 10.0f, plot_pos.y + 4.0f);
        dl->AddText(base, word_col, word);
        dl->AddText(ImVec2(base.x + ImGui::CalcTextSize(word).x, base.y),
                    IndiTokens::INK_DIM, rest);
        ImGui::PopFont();
    }

    // ── Hover: name the brain (SPEC §2 - "HIGH · HMM 0.82" vs "HIGH · THRESH") ──
    if (ImPlot::IsPlotHovered()) {
        const ImPlotPoint mp = ImPlot::GetPlotMousePos();
        auto it = std::upper_bound(pts_.begin(), pts_.end(), mp.x,
            [](double x, const Series::VPINPoint& p) {
                return x < static_cast<double>(p.ts_ms);
            });
        if (it != pts_.begin()) {
            const auto& p = *(it - 1);
            Theme::begin_tooltip();
            ImGui::PushFont(Theme::Fonts::mono_sm());
            if (p.hmm_state >= 0)
                ImGui::Text("%s \xC2\xB7 HMM %.2f", regime_word(p.regime), p.hmm_conf);
            else
                ImGui::Text("%s \xC2\xB7 THRESH", regime_word(p.regime));
            ImGui::Text("VPIN %.4f", p.vpin);
            if (show_imbalance_) ImGui::Text("IMB  %.4f", p.imbalance);
            ImGui::PopFont();
            Theme::end_tooltip();
        }
    }

    ImPlot::PopPlotClipRect();
}

} // namespace Indicators
