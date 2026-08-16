#include "rendering/liq_field_renderer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "implot.h"

#include "core/candle_manager.h"
#include "core/heatmap_colormap.h"
#include "core/liquidation_heatmap_manager.h"
#include "types/frame_profiler.h"

namespace {

// Cascade (the pending branch's forward projection) - the `cascade` spec:
// alpha x0.78 vs history behind a 1px 7%-white seam at the live edge; the projection tail
// steps down 88/95/100% of its length at 100/55/28% alpha (interim - a linear fade over the
// final 12% arrives with the texture-quad path).
constexpr float kCascadeTailFrac[3]  = {0.88f, 0.95f, 1.00f};
constexpr float kCascadeTailAlpha[3] = {1.00f, 0.55f, 0.28f};
constexpr ImU32 kCascadeSeam = IM_COL32(255, 255, 255, 18);       // 1px @ 7% white

}  // namespace

// (Re)build the 2D time×price Field cache as horizontal SEGMENTS. A single left→right walk over ALL
// loaded candles maintains the fuel currently standing at each price bucket: each candle CONSUMES the
// fuel its [low,high] trades through (emitting a finished segment, then going dark) and DEPOSITS fresh
// fuel at its per-tier liquidation prices. A level therefore emits a NEW segment each time it's swept
// and re-lit - giving the time-varying intensity + dense fill MMT shows (vs the old single-block carve).
// Purely candle-derived → identical live and in replay; rebuilt only when the closed-candle set / mask /
// timeframe changes. §5b(b).
void LiqFieldRenderer::rebuild(uint8_t lmask, int64_t tf_ms) {
    // Rebuild spikes attribute to this section in the perf overlay / spike log
    // (fires on candle-set/mask/TF signature change - frequent during replay).
    ProfileScope _ps("LiqRebuild");
    (void)tf_ms;
    segs_.clear();
    max_mag_ = 0.0f;
    norm_lo_ = 0.0f;
    norm_hi_ = 0.0f;
    bw_ = 0.0;
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();
    if (candles.empty() && !has_bld) return;

    // LOG-PRICE buckets (2026-07-02d): bucket k = llround(ln(price)/lbw), so a bucket is knobs_.bps
    // of its OWN price at every level - uniform relative row thickness across the whole loaded range.
    // A LINEAR grid cannot serve a symbol whose price spans several × (TAIKO 8×: one shared bucket was
    // sub-pixel at the top of the range AND rendered as giant solid blocks at the bottom). Log levels
    // are still absolute in price (static across zoom/scroll) and candle-derived (replayable); bucket
    // count is inherently bounded (ln(range-ratio)/lbw). lbw is the width in log-price ≈ the relative
    // width (ln(1+x) ≈ x).
    const double lbw = static_cast<double>(std::clamp(knobs_.bps, 1.0f, 50.0f)) * 1.0e-4;
    const double inv_lbw = 1.0 / lbw;

    // ROBUST relative-volume (anomaly) weight - MEDIAN-relative, log-soft-clipped in step() below.
    // The old GLOBAL-mean reference let a few mega-volume pump candles inflate the mean and dim every
    // other deposit (TAIKO: the whole consolidation went sub-floor). Median is viewport-independent
    // and insensitive to those outliers, so consolidation fuel and pump fuel stay on comparable scales.
    std::vector<double> vols;
    vols.reserve(candles.size() + 1);
    for (const auto& c : candles) if (c.volume > 0.0) vols.push_back(c.volume);
    if (has_bld && cm.building_candle().volume > 0.0) vols.push_back(cm.building_candle().volume);
    if (vols.empty()) return;
    const size_t vmid = vols.size() / 2;
    std::nth_element(vols.begin(), vols.begin() + vmid, vols.end());
    const double vmed = vols[vmid];
    if (vmed <= 0.0) return;
    const double wcap = std::max(1.0, static_cast<double>(knobs_.wcap));
    // AGE-DECAY half-life (task-1 belt fix, round 6). Standing fuel decays exponentially with CANDLE
    // age - deposits from days ago fade unless the bucket keeps being refed. Bounds every bucket's
    // steady state (no unbounded standing accumulation → no belts at any load length) while keeping
    // the SUM model's wide, structured dynamic range (rounds 4/5 showed the percentile map just
    // re-normalizes any distribution reshaping into soup/all-bright). Purely candle-timestamp-driven:
    // static across zoom/scroll, identical in replay; consumed segments decay to their CONSUME time,
    // so history stays a fixed record. ≤0 disables.
    const double hl_ms = static_cast<double>(knobs_.halflife_h) * 3600.0e3;

    // Mass-conserving gaussian deposit kernel (±K buckets; K=0 default - bucket coarseness supplies
    // thickness). A fast/vertical move leaves ~1 candle per price level, so point deposits can render
    // as 1-bucket threads - the kernel spreads each deposit into a legible band. Σkw = 1 keeps total
    // deposited mass identical to a point deposit, so stacked zones don't inflate the normalization.
    const int K = std::clamp(knobs_.kernel, 0, 8);
    double kw[9];
    {
        const double ksig = std::max(0.5, static_cast<double>(K) / 1.6659);  // edge bucket ≈ 0.25×center
        double ksum = 0.0;
        for (int d = 0; d <= K; ++d) {
            kw[d] = std::exp(-(static_cast<double>(d) * d) / (2.0 * ksig * ksig));
            ksum += (d == 0) ? kw[d] : 2.0 * kw[d];
        }
        for (int d = 0; d <= K; ++d) kw[d] /= ksum;
    }

    static constexpr double  kLev[6] = {5.0, 10.0, 25.0, 50.0, 75.0, 100.0};
    static constexpr uint8_t kBit[6] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20};

    struct State { double f; int64_t run_start; int64_t last_ms; };  // decayed fuel sum + run start +
    std::map<long long, State> live;                 // last deposit time (decay is applied lazily);
                                                     // key = llround(ln(price)/lbw) → ordered consume
    auto decayed = [&](const State& st, int64_t now_ms) -> double {
        return (hl_ms > 0.0 && now_ms > st.last_ms)
            ? st.f * std::exp2(-static_cast<double>(now_ms - st.last_ms) / hl_ms) : st.f;
    };

    auto emit = [&](long long k, int64_t start_ms, int64_t end_ms, double f) {
        if (f <= 0.0) return;
        const float fi = static_cast<float>(f);
        const float pr = static_cast<float>(std::exp(static_cast<double>(k) * lbw));
        segs_.push_back({ pr, pr, fi, start_ms, end_ms });
        if (fi > max_mag_) max_mag_ = fi;
    };

    auto step = [&](const Terminal::Candle& c) {
        if (c.high < c.low || c.low <= 0.0) return;
        // CONSUME: standing fuel this candle's [low,high] trades through ends here (emit + go dark).
        const long long klo = std::llround(std::log(c.low)  * inv_lbw);
        const long long khi = std::llround(std::log(c.high) * inv_lbw);
        auto it   = live.lower_bound(klo);
        auto stop = live.upper_bound(khi);
        while (it != stop) {
            emit(it->first, it->second.run_start, c.timestamp_ms, decayed(it->second, c.timestamp_ms));
            it = live.erase(it);
        }
        // DEPOSIT: candle opens leveraged size → project to its per-tier liquidation prices.
        const double p = (c.high + c.low + c.close) / 3.0;
        if (p <= 0.0 || c.volume <= 0.0) return;
        // ANOMALY weight (MMT's flow-deviation crux) with a NEAR-TIER floor - the round-3e statistic,
        // RESTORED. Brightness comes from the EXCESS over baseline volume (log-compressed + capped);
        // the dim per-candle floor trace applies ONLY to the near tiers (50/75/100×, ≤2% offsets),
        // drawing MMT's short in-channel fragments; far tiers (5/10/25×) are excess-only.
        // Rounds 4/5 post-mortem (belts → soup → all-yellow): gating far deposits harder, then
        // replacing the run SUM with a per-plot MAX, only RESHAPED the intensity distribution - and
        // the dual-percentile map ADAPTS to whatever distribution it gets, so both times it re-
        // normalized the whole field bright (and a 93%-mover's pump projections dominated every level
        // under MAX). The sum keeps the wide, structured dynamic range the map needs; the age-decay
        // above fixes the belts at their actual root: UNBOUNDED STANDING ACCUMULATION over long loads.
        const double ex  = c.volume / vmed - static_cast<double>(knobs_.wbase);
        const double wex = (ex <= 0.0) ? 0.0 : ((ex <= 1.0) ? ex : 1.0 + std::log(ex));
        const double wfl = static_cast<double>(knobs_.wfloor);
        if (wex <= 0.0 && wfl <= 0.0) return;
        auto add = [&](double price, double w) {
            if (price <= 0.0) return;
            const long long k0 = std::llround(std::log(price) * inv_lbw);
            for (int d = -K; d <= K; ++d) {
                auto [jt, fresh] = live.try_emplace(
                    k0 + d, State{0.0, c.timestamp_ms, c.timestamp_ms});
                (void)fresh;
                State& st = jt->second;
                if (!fresh) st.f = decayed(st, c.timestamp_ms);   // lazy decay, then deposit
                st.last_ms = c.timestamp_ms;
                st.f += w * kw[d < 0 ? -d : d];
            }
        };
        for (int t = 0; t < 6; ++t) {
            if (!(lmask & kBit[t])) continue;
            const double invL = 1.0 / kLev[t];
            const double w = std::min(wcap, wex + ((invL <= 0.021) ? wfl : 0.0));
            if (w <= 0.0) continue;
            add(p * (1.0 - invL), w);   // long liq (below entry)
            add(p * (1.0 + invL), w);   // short liq (above entry)
        }
    };

    for (const auto& c : candles) step(c);
    if (has_bld) step(cm.building_candle());

    // Flush fuel still standing → PENDING segments that run to the live/replay edge. Pending fuel is
    // decayed to the latest loaded candle's open (a candle-set timestamp, NOT wall clock - the field
    // stays deterministic between rebuilds and identical in replay).
    int64_t t_ref = candles.empty() ? 0 : candles.back().timestamp_ms;
    if (has_bld) t_ref = std::max(t_ref, cm.building_candle().timestamp_ms);
    for (const auto& [k, st] : live) emit(k, st.run_start, kSegPending, decayed(st, t_ref));
    if (segs_.empty()) { max_mag_ = 0.0f; return; }

    bw_ = lbw;   // stored: LOG bucket width (grid = exp(k·lbw))

    // DUAL-PERCENTILE LOG normalization (on the RAW per-bucket segments, pre-merge) - MMT's Intensity
    // Low/Peak semantics. Segment mass spans orders of magnitude; a single divisor + power curve either
    // crushed the tail (max-norm, the TAIKO vanish) or bunched everything into flat saturated slabs
    // (p98.5 clip, the BTC "colour walls"). t = ln(f/lo)/ln(hi/lo): below p(lo_pct) → dark, p(hi_pct)+
    // → ramp top, and the decades in between spread purple→cyan→yellow like MMT.
    {
        std::vector<float> mags;
        mags.reserve(segs_.size());
        for (const auto& s : segs_) mags.push_back(s.intensity);
        auto pick = [&](float pct) -> float {
            const size_t idx = static_cast<size_t>(
                static_cast<double>(mags.size() - 1) * std::clamp(pct, 0.0f, 1.0f));
            std::nth_element(mags.begin(), mags.begin() + idx, mags.end());
            return mags[idx];
        };
        float hi = pick(std::clamp(knobs_.hi_pct, 0.50f, 1.00f));
        float lo = pick(std::clamp(knobs_.lo_pct, 0.00f, 0.95f));
        if (hi <= 0.0f) hi = max_mag_;
        if (hi <= 0.0f) { segs_.clear(); return; }
        // Degenerate spread (young symbol / near-uniform mass) → fall back to a wide fixed range so
        // the map stays defined; everything then reads bright, which is the honest degenerate render.
        if (lo <= 0.0f || lo >= hi * 0.5f) lo = hi / 256.0f;
        norm_lo_ = lo;
        norm_hi_ = hi;
    }

    // MERGE pass (perf): fuse vertically-adjacent buckets with the SAME time interval and the same
    // QUANTIZED normalized intensity into one taller run - the deposit kernel + dense consolidations
    // emit long identical bucket runs, so this collapses the per-frame AddRectFilled count with no
    // visual change (1/96 of the normalized ramp is under one visible colormap step; the running mean
    // of values inside a shared quantization bin stays inside that bin).
    if (segs_.size() > 1) {
        std::sort(segs_.begin(), segs_.end(),
                  [](const LiqFieldSeg& a, const LiqFieldSeg& b) {
                      if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
                      if (a.end_ms   != b.end_ms)   return a.end_ms   < b.end_ms;
                      return a.price_lo < b.price_lo;
                  });
        const float qlo = norm_lo_;
        const float qinv_lr = 1.0f / std::log(norm_hi_ / qlo);
        auto quant = [&](float f) -> int {   // quantize in FINAL (log-mapped) t-space
            const float t = (f <= qlo) ? 0.0f
                          : std::min(std::log(f / qlo) * qinv_lr, 1.0f);
            return static_cast<int>(std::lround(t * 96.0f));
        };
        std::vector<LiqFieldSeg> merged;
        merged.reserve(segs_.size());
        merged.push_back(segs_[0]);
        int run = 1;
        // Adjacency on the LOG grid is a price RATIO of exp(lbw) between neighbouring bucket centers.
        const float r_lo = static_cast<float>(std::exp(0.5 * lbw));
        const float r_hi = static_cast<float>(std::exp(1.5 * lbw));
        for (size_t i = 1; i < segs_.size(); ++i) {
            const LiqFieldSeg& s = segs_[i];
            LiqFieldSeg& cur = merged.back();
            const float ratio = (cur.price_hi > 0.0f) ? s.price_lo / cur.price_hi : 0.0f;
            if (s.start_ms == cur.start_ms && s.end_ms == cur.end_ms &&
                run < kMaxMergeRun && ratio > r_lo && ratio < r_hi &&
                quant(s.intensity) == quant(cur.intensity)) {
                cur.intensity = (cur.intensity * static_cast<float>(run) + s.intensity) /
                                static_cast<float>(run + 1);
                cur.price_hi = s.price_hi;
                ++run;
            } else {
                merged.push_back(s);
                run = 1;
            }
        }
        segs_.swap(merged);
    }

    // Sort by price so the renderer can binary-search the visible Y-band (then X-cull within).
    std::sort(segs_.begin(), segs_.end(),
              [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
}

// Rebuild the Field cache when its inputs changed (closed-candle set / leverage mask / timeframe).
// Shared by the Field render AND the Liq Profile marginal, so the profile works even when the Field
// layer itself is toggled off. Cheap no-op when nothing changed.
void LiqFieldRenderer::ensure_cache() {
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    if (candles.empty() && !cm.has_building_candle()) return;
    const uint8_t lmask = ctx_.liq_heatmap_mgr().get_leverage_mask();
    const int64_t tf_ms = cm.timeframe_seconds() * 1000;
    const int64_t sig_ts = candles.empty() ? 0 : candles.back().timestamp_ms;
    if (sig_ts != sig_ts_ || candles.size() != sig_n_ ||
        lmask != sig_mask_ || tf_ms != sig_tf_) {
        sig_ts_   = sig_ts;
        sig_n_    = candles.size();
        sig_mask_ = lmask;
        sig_tf_   = tf_ms;
        rebuild(lmask, tf_ms);
        ++rebuild_gen_;   // WS2: keys the texture re-raster (renderer compares)
    }
}

// Liq Heatmap Field - dense candle×leverage liquidation projection (MMT-style backdrop), 2D time×price.
// The field is built as cached horizontal SEGMENTS (rebuild) over ALL loaded candles in absolute price
// buckets, so it's STRICTLY STATIC across zoom/scroll and 100% replayable. This function only maps the
// visible segments to pixels: each runs from its deposit time to its consume time (or to the live/replay
// edge if still pending), colored by its own fuel intensity → the traded region stays dense and the same
// level shows different colors on either side of a sweep.
void LiqFieldRenderer::render() {
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();
    if (candles.empty() && !has_bld) return;

    const int64_t tf_ms = cm.timeframe_seconds() * 1000;
    ensure_cache();   // shared cache - also feeds the Liq Profile marginal (§8)
    if (segs_.empty() || norm_hi_ <= 0.0f || bw_ <= 0.0) return;
    // WS2: texture-quad path - ONE LUT-shaded GPU quad per frame. Returns false
    // on any unsupported case (rows > 4096, shader init failure) → the rect
    // path below stays the A/B fallback (Tweaks → LIQ FIELD RENDER).
    if (knobs_.use_texture && render_textured(tf_ms)) return;
    const double bw = bw_;

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    const double y_min = lims.Y.Min, y_max = lims.Y.Max;
    const double vx_min = lims.X.Min, vx_max = lims.X.Max;   // visible time window (ms)
    if (y_max <= y_min) return;
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double x_ref = lims.X.Min;   // any X - price→pixel-y is X-independent

    const float x_left  = plot_pos.x;
    const float x_right = plot_pos.x + plot_size.x;
    // Pending-fuel right edge = latest built candle (playback head in replay, wall-current live) + a
    // small forward magnet, clamped to the visible right → never paints across the empty future.
    int64_t latest_ms = 0;
    if (has_bld)                latest_ms = cm.building_candle().timestamp_ms;
    else if (!candles.empty())  latest_ms = candles.back().timestamp_ms;
    float x_live = x_right;
    if (latest_ms > 0) {
        const float cx = ImPlot::PlotToPixels(
            ImPlotPoint(static_cast<double>(latest_ms) + tf_ms * 0.5, y_min)).x;
        x_live = std::clamp(cx, x_left, x_right);
    }
    constexpr double kLiqCascadeMaxCandles =
        static_cast<double>(LiqFieldTextureRenderer::kMaxProjectionCols);
    const float x_projection_end = latest_ms > 0
        ? std::clamp(
            ImPlot::PlotToPixels(ImPlotPoint(
                static_cast<double>(latest_ms) +
                    tf_ms * (0.5 + kLiqCascadeMaxCandles), y_min)).x,
            x_live, x_right)
        : x_right;
    // Base pending right edge = live edge + a small forward magnet. With knobs_.extend on, each
    // pending band instead projects right of the live edge ∝ its own strength (x_pend in the loop) -
    // the MMT future cascade, where the projection length doubles as the histogram bar.
    const float x_edge = std::min(x_projection_end, x_live + plot_size.x * 0.06f);

    // Live building-candle carve: while the current candle trades through a price, pending fuel there
    // stops at the candle NOW (no candle-close lag) - the overlap fix, applied per-frame at render.
    double bld_lo = 1.0, bld_hi = 0.0; int64_t bld_ms = 0;
    if (has_bld) { const auto& b = cm.building_candle(); bld_lo = b.low; bld_hi = b.high; bld_ms = b.timestamp_ms; }

    auto time_to_x = [&](int64_t ms, double off) -> float {
        return std::clamp(
            ImPlot::PlotToPixels(ImPlotPoint(static_cast<double>(ms) + off, y_min)).x, x_left, x_right);
    };

    const float gamma    = std::max(0.1f, knobs_.gamma);
    const int   alpha    = static_cast<int>(std::clamp(knobs_.opacity, 0.0f, 1.0f) * 255.0f);
    // LOG map between the Low/Peak percentile clips (MMT Intensity semantics; set at build).
    const float nlo = norm_lo_;
    if (nlo <= 0.0f || norm_hi_ <= nlo) return;
    const float inv_lr  = 1.0f / std::log(norm_hi_ / nlo);
    const float floor_t = std::max(0.0f, knobs_.floor_t);
    // LOG grid: bw is the bucket width in log-price → neighbours differ by a price RATIO of exp(bw),
    // a bucket's edges sit at center·exp(±bw/2), and "± eps" tests become relative (× (1 ∓ bw/4)).
    const double e_half  = std::exp(bw * 0.5);
    const float  e_lbw   = static_cast<float>(std::exp(bw));
    const float  r_eps_m = 1.0f - static_cast<float>(bw) * 0.25f;
    const float  r_eps_p = 1.0f + static_cast<float>(bw) * 0.25f;

    // Live-carve band in BUCKET-GRID coordinates (centers at exp(k·bw)): the first and last bucket
    // centers the per-bucket carve test would catch (± one bucket around the building candle's range).
    // Splitting merged runs on these bounds reproduces the pre-merge per-bucket carve exactly.
    double carve_glo = 0.0, carve_ghi = -1.0;
    if (bld_hi >= bld_lo && bld_lo > 0.0) {
        const double inv_lbw = 1.0 / bw;
        carve_glo = std::exp(std::ceil (std::log(bld_lo) * inv_lbw - 1.0) * bw);  // first carved center
        carve_ghi = std::exp(std::floor(std::log(bld_hi) * inv_lbw + 1.0) * bw);  // last carved center
    }
    const float glo_f = static_cast<float>(carve_glo), ghi_f = static_cast<float>(carve_ghi);

    // One rect per (sub-)run: [p_lo..p_hi] are bucket CENTERS, drawn ± half a bucket (ratio) tall.
    // Rows are clamped to knobs_.min_row_px tall (expanded symmetrically about their center) so
    // alt-tier rows on wide-span movers stay legible bands instead of sub-pixel hairlines (design
    // 2026-07-02 - replaces the ±1 deposit kernel on the render side).
    const float min_row_px = std::max(1.0f, knobs_.min_row_px);
    auto emit_rect = [&](float p_lo, float p_hi, float xa, float xb, ImU32 col) {
        if (p_hi < p_lo * r_eps_m || xb - xa < 1.0f) return;
        const float ya = ImPlot::PlotToPixels(ImPlotPoint(x_ref, static_cast<double>(p_hi) * e_half)).y;
        const float yb = ImPlot::PlotToPixels(ImPlotPoint(x_ref, static_cast<double>(p_lo) / e_half)).y;
        float y0 = std::min(ya, yb), y1 = std::max(ya, yb) + 1.0f;
        if (y1 - y0 < min_row_px) {
            const float yc = 0.5f * (y0 + y1);
            y0 = yc - 0.5f * min_row_px;
            y1 = yc + 0.5f * min_row_px;
        }
        dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), col);
    };

    // Pending fuel = its history run + the forward CASCADE right of the live edge (design `cascade`
    // spec): the projection drops to knobs_.cascade_alpha × the history alpha, and its tail steps down
    // at kCascadeTailFrac/Alpha - 88/95/100% of the projection length at 100/55/28% alpha (interim;
    // the texture-quad path replaces the steps with a linear fade).
    auto emit_pending = [&](float p_lo, float p_hi, float pxa, float pxb,
                            uint8_t cr, uint8_t cg, uint8_t cb, int ra) {
        emit_rect(p_lo, p_hi, pxa, std::min(pxb, x_live), IM_COL32(cr, cg, cb, ra));
        const float cs = std::max(pxa, x_live);   // cascade start
        if (pxb <= cs + 0.5f) return;
        const float len = pxb - cs;
        const float ca  = static_cast<float>(ra) * std::clamp(knobs_.cascade_alpha, 0.0f, 1.0f);
        float f0 = 0.0f;
        for (int s = 0; s < 3; ++s) {
            const float f1 = kCascadeTailFrac[s];
            emit_rect(p_lo, p_hi, cs + len * f0, cs + len * f1,
                      IM_COL32(cr, cg, cb, static_cast<int>(ca * kCascadeTailAlpha[s])));
            f0 = f1;
        }
    };

    // Clip to the plot area: partially-visible rows are kept by the Y-cull below and must be CLIPPED
    // at the plot edges, not painted over the time axis / readouts (the draw list alone doesn't clip).
    ImPlot::PushPlotClipRect();

    // Y-cull: segments sorted by price_lo → binary-search the bottom of the visible band, iterate up.
    // Merged runs span up to kMaxMergeRun buckets, so the scan starts that many RATIOS lower;
    // runs still entirely below the view are skipped in the loop.
    const double y_lo_margin = (y_min > 0.0)
        ? y_min * std::exp(-bw * (kMaxMergeRun + 1)) : y_min;
    const double y_hi_cut = (y_max > 0.0) ? y_max * std::exp(bw) : y_max;
    const double y_lo_cut = (y_min > 0.0) ? y_min * std::exp(-bw) : y_min;
    const LiqFieldSeg key{ static_cast<float>(y_lo_margin), 0.0f, 0.0f, 0, 0 };
    auto sit = std::lower_bound(segs_.begin(), segs_.end(), key,
                   [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
    for (; sit != segs_.end(); ++sit) {
        const LiqFieldSeg& sg = *sit;
        if (sg.price_lo > y_hi_cut) break;                      // above view → done
        if (sg.price_hi < y_lo_cut) continue;                   // merged run entirely below view
        const bool pending = (sg.end_ms == kSegPending);
        if (sg.start_ms > vx_max) continue;                     // X-cull: entirely right of view
        if (!pending && sg.end_ms < vx_min) continue;           // entirely left of view
        if (sg.intensity <= nlo) continue;                      // Intensity-Low clip → dark
        const float tl = std::min(std::log(sg.intensity / nlo) * inv_lr, 1.0f);
        const float tv = std::pow(tl, gamma);
        if (tv < floor_t) continue;                             // noise floor → keep background dark
        uint8_t cr, cg, cb;
        HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, tv, cr, cg, cb);
        // Design alpha curve (export JSON alphaCurve): alpha = opacity·(floor + (1−floor)·t^pow).
        // Dim fuel melts much further into the background than the old (0.55+0.45·t) linear blend
        // (t=0 → 0.072 vs 0.468) while magnets keep near-full presence.
        const int ra = static_cast<int>(static_cast<float>(alpha) *
            (knobs_.alpha_floor +
             (1.0f - knobs_.alpha_floor) * std::pow(tv, knobs_.alpha_pow)));
        const ImU32 col = IM_COL32(cr, cg, cb, ra);
        // NO-OVERLAP edges (2026-07-02b): a band starts at the DEPOSITING candle's RIGHT edge and (if
        // consumed) ends at the CONSUMING candle's LEFT edge. A level price traded through during
        // candle T must be dark across column T, so bands can never paint over the candles that
        // created or took them; churn consumed on the very next candle collapses into the inter-candle
        // gap (the faint stubs MMT also shows). Old edges (start-left/end-right) overlapped both.
        const float xa = time_to_x(sg.start_ms, tf_ms * 0.35);
        if (!pending) {
            emit_rect(sg.price_lo, sg.price_hi, xa, time_to_x(sg.end_ms, -tf_ms * 0.35), col);
            continue;
        }
        // Standing fuel: history runs [xa .. live edge]; beyond the live edge it projects forward by a
        // length ∝ its strength (MMT future cascade - strong magnets reach the right edge, weak fuel
        // is a stub, so the projection doubles as a price-anchored histogram). Carve where the
        // building candle is trading NOW; merged runs are split so the carve stays per-bucket exact.
        float x_pend = x_edge;
        if (knobs_.extend) {
            const float span = x_projection_end - x_live;
            if (span > 1.0f)
                x_pend = std::min(
                    x_projection_end, x_live + span * (0.15f + 0.85f * tv));
        }
        if (carve_ghi >= carve_glo && sg.price_hi >= glo_f * r_eps_m && sg.price_lo <= ghi_f * r_eps_p) {
            const float carve_xb = std::min(x_pend, time_to_x(bld_ms, -tf_ms * 0.35));
            if (sg.price_lo < glo_f * r_eps_m)
                emit_pending(sg.price_lo, std::min(sg.price_hi, glo_f / e_lbw), xa, x_pend, cr, cg, cb, ra);
            emit_rect(std::max(sg.price_lo, glo_f), std::min(sg.price_hi, ghi_f), xa, carve_xb, col);
            if (sg.price_hi > ghi_f * r_eps_p)
                emit_pending(std::max(sg.price_lo, ghi_f * e_lbw), sg.price_hi, xa, x_pend, cr, cg, cb, ra);
        } else {
            emit_pending(sg.price_lo, sg.price_hi, xa, x_pend, cr, cg, cb, ra);
        }
    }
    // 1px live-edge seam (design `cascade.seamPx/seamColor`) - the visual boundary between the
    // static history and the forward cascade projection.
    if (latest_ms > 0 && x_live > x_left + 1.0f && x_live < x_right - 1.0f)
        dl->AddLine(ImVec2(x_live, plot_pos.y), ImVec2(x_live, plot_pos.y + plot_size.y),
                    kCascadeSeam, 1.0f);
    ImPlot::PopPlotClipRect();
}

// WS2 - the Field as ONE LUT-shaded textured quad.
// RENDER-ONLY: the segment cache stays the source of truth. The cache is
// rasterized into an R8 log-grid texture on rebuild (keyed by rebuild_gen_),
// the live column (building-candle carve) + the standing-fuel 1xH texture
// upload per frame, and the fragment shader applies gamma → noise floor →
// Ember-K LUT → the design alpha curve, plus the forward CASCADE with the
// design's LINEAR fade over the final 12% (replacing the rect path's interim
// stepped tail). Returns false on any unsupported case so render() falls
// through to the rect path (the A/B fallback).
bool LiqFieldRenderer::render_textured(int64_t tf_ms) {
    auto& cm = ctx_.candle_mgr();
    const auto& candles = cm.candles();
    const bool has_bld = cm.has_building_candle();

    int64_t first_ms = 0;
    if (!candles.empty())  first_ms = candles.front().timestamp_ms;
    else if (has_bld)      first_ms = cm.building_candle().timestamp_ms;
    int64_t latest_ms = 0;
    if (has_bld)                latest_ms = cm.building_candle().timestamp_ms;
    else if (!candles.empty())  latest_ms = candles.back().timestamp_ms;
    if (first_ms <= 0 || latest_ms <= 0 || tf_ms <= 0) return false;

    LiqFieldTextureRenderer::RasterParams rp;
    rp.rebuild_gen = rebuild_gen_;
    rp.lbw       = bw_;
    rp.norm_lo   = norm_lo_;
    rp.norm_hi   = norm_hi_;
    rp.tf_ms     = tf_ms;
    rp.first_ms  = first_ms;
    rp.latest_ms = latest_ms;
    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    rp.view_min_ms = plot_limits.X.Min;
    rp.view_max_ms = plot_limits.X.Max;
    if (!tex_.ensure(segs_, rp)) return false;

    LiqFieldTextureRenderer::FrameParams fp;
    fp.latest_ms = latest_ms;
    if (has_bld) {
        // Live building-candle carve band - the same bucket-grid math as the
        // rect path + the Profile (first/last carved bucket ±1 around range).
        const auto& b = cm.building_candle();
        if (b.high >= b.low && b.low > 0.0) {
            const double inv_lbw = 1.0 / bw_;
            fp.carve_klo = static_cast<long long>(
                std::ceil (std::log(b.low)  * inv_lbw - 1.0));
            fp.carve_khi = static_cast<long long>(
                std::floor(std::log(b.high) * inv_lbw + 1.0));
        }
    }
    fp.extend        = knobs_.extend ? 1 : 0;
    fp.magnet_frac   = 0.06f;   // the rect path's x_edge forward magnet
    fp.max_projection_cols = LiqFieldTextureRenderer::kMaxProjectionCols;
    fp.gamma         = knobs_.gamma;
    fp.floor_t       = knobs_.floor_t;
    fp.alpha_floor   = knobs_.alpha_floor;
    fp.alpha_pow     = knobs_.alpha_pow;
    fp.opacity       = knobs_.opacity;
    fp.cascade_alpha = knobs_.cascade_alpha;
    fp.fade_frac     = 0.12f;   // design: linear fade over the final 12%
    fp.min_row_px    = knobs_.min_row_px;   // rect-path row clamp parity (§4)

    tex_.update_live(fp);
    const float x_live = tex_.render(fp);
    if (x_live < 0.0f) return true;   // valid raster; nothing visible this frame

    // 1px live-edge seam (design `cascade.seamPx/seamColor`) - same as the rect path.
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (x_live > plot_pos.x + 1.0f && x_live < plot_pos.x + plot_size.x - 1.0f) {
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddLine(
            ImVec2(x_live, plot_pos.y), ImVec2(x_live, plot_pos.y + plot_size.y),
            kCascadeSeam, 1.0f);
        ImPlot::PopPlotClipRect();
    }
    return true;
}
