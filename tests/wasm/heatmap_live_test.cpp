#include <GLES3/gl3.h>
#include <array>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>
#include "imgui.h"
#include "implot.h"
#include "pb/messages.pb.h"
// Exercise the renderer's rebuild boundary without an ImGui frame or GL context.
#define private public
#include "rendering/shader_heatmap_renderer.h"
#undef private
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <memory>
#include "ui/realtime_dom_frame.h"

static GLuint bound = 0, next_id = 0;
static std::unordered_map<GLuint, std::vector<float>> metadata;
extern "C" {
void glGenTextures(GLsizei n, GLuint* ids) { while(n--) *ids++ = ++next_id; }
void glBindTexture(GLenum, GLuint id) { bound = id; }
void glTexParameteri(GLenum, GLenum, GLint) {}
void glDeleteTextures(GLsizei, const GLuint*) {}
void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum, const void* data) {
    if (format == GL_RGBA && h == 1) metadata[bound] = std::vector<float>((const float*)data, (const float*)data + w*4);
}
void glTexSubImage2D(GLenum, GLint, GLint x, GLint, GLsizei w, GLsizei h, GLenum format, GLenum, const void* data) {
    if (format == GL_RGBA && h == 1) std::copy_n((const float*)data,w*4,metadata[bound].begin()+x*4);
}
}
int main() {
    auto renderer = std::make_unique<ShaderHeatmapRenderer>();
    auto& r = *renderer;
    {
        auto fixture = std::make_unique<ShaderHeatmapRenderer>();
        auto& r = *fixture;
        // The production column builder and DOM must agree on TUT and BTC bucket sums,
        // including decimal boundaries at every fidelity.
        for (const auto [tick_size, first_tick] : {std::pair{0.00001, 19000}, std::pair{0.1, 788600}, std::pair{0.01, 7886000}})
        for (int mult : {1, 2, 5, 10, 20, 50, 100, 500, 2000}) {
            r.clear(); r.configure_realtime(tick_size); r.set_bucket_multiplier(mult);
            std::unordered_map<double, float> levels;
            for (int tick = first_tick; tick < first_tick + 200; ++tick) levels[tick * tick_size] = float(tick % 7 + 1);
            r.finalize_column(1000, levels, true, (first_tick + 100) * tick_size);
            r.sync_gpu_from_timeline();
            RealtimeDOMFrame frame;
            frame.native_tick = r.get_native_bucket_size(); frame.bucket_ticks = r.get_bucket_multiplier();
            r.finalize_column(1100, levels, false, (first_tick + 100) * tick_size);
            assert(r.column_meta_[1].values == r.column_meta_[0].values);
            assert(r.texture_grouping() == 1);
            const auto& meta = r.column_meta_[0];
            for (int row = 0; row < meta.num_rows; ++row) {
                const double lower = meta.price_min + row * meta.price_step;
                const auto bucket = frame.bucket_index(lower);
                double dom = 0, heatmap = 0;
                for (const auto& [price, qty] : levels) if (frame.bucket_index(price) == bucket) dom += qty;
                heatmap = meta.values[row];
                assert(dom == heatmap);
                assert(r.get_value_at_price_and_time(lower, 1000) == heatmap);
                // Float GPU offsets must stay well below a screen pixel even
                // at 100 pixels/native tick, for BTC 0.1 and 0.01 tick fixtures.
                const double gpu_lower = r.gpu_price_origin_ + metadata[r.meta_texture_][0] +
                    row * double(float(meta.price_step));
                assert(std::abs(gpu_lower - lower) / meta.price_step * 100 < 0.01);
                assert(std::abs(frame.bucket_center(bucket) - (lower + frame.bucket_size() * 0.5)) < 1e-10);
            }
            r.set_bucket_multiplier(1);
            r.sync_gpu_from_timeline();
            double restored = 0, original = 0;
            for (float qty : r.column_meta_[0].values) restored += qty;
            for (const auto& [price, qty] : levels) original += qty;
            assert(restored == original);
        }
    }
    r.native_bucket_size_ = 1;
    const std::unordered_map<double,float> history{{100,2},{101,3}};
    const std::unordered_map<double,float> live{{100,17},{101,29}};
    r.finalize_column(60000,history);
    r.finalize_column(120000,history);
    r.update_live_column(185000,live); // Arrives while history grid is dirty.
    r.sync_gpu_from_timeline();
    auto check_live = [&](int col) {
        assert(r.ring_count_ > col);
        assert(metadata[r.meta_texture_][col*4+3] == 2);
        assert(metadata[r.meta_texture_][col*4+2] == 29);
    };
    check_live(2);
    for(int i=0;i<12;++i) { // Repeated backfills between live book updates.
        r.gpu_dirty_=true;
        r.sync_gpu_from_timeline();
        check_live(2);
    }
    r.finalize_column(180000,history); // Same grid slot must retain current depth.
    check_live(2);
    r.timeline_.erase(60000); // Origin moves after retention eviction.
    r.gpu_dirty_=true;
    r.sync_gpu_from_timeline();
    check_live(1);
    r.set_replay_cutoff_ms(130000); // Rewind must hide and discard the newer book.
    r.sync_gpu_from_timeline();
    assert(metadata[r.meta_texture_][7] == 0);
    r.set_replay_cutoff_ms(200000);
    r.sync_gpu_from_timeline();
    assert(metadata[r.meta_texture_][7] == 1); // History, not the cached future book.
    r.update_live_column(190000,live);
    check_live(1);
    // A deep book can have remote orders far outside the active market.
    // The bounded GPU row window must stay around the best bid/ask midpoint.
    const std::unordered_map<double,float> deep{{1,1},{100,17},{101,29},{100000,1}};
    r.update_live_column(191000, deep, 100.5);
    check_live(1);
    assert(r.get_value_at_price_and_time(100.25, 180000) == 17);
    assert(r.get_value_at_price_and_time(100.25, 240000) == 0); // No neighbour borrowing.
    auto changed = deep; changed[100] = 23;
    r.update_live_column(192000, changed, 100.5);
    assert(r.get_value_at_price_and_time(100.25, 180000) == 23); // Labels see the updated book.
    r.timeline_[60000] = history; // Backfill changes CPU origin before debounced sync.
    r.gpu_dirty_ = true;
    assert(r.get_value_at_price_and_time(100.25, 180000) == 23); // Old GPU grid still owns lookup.
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(100.25, 180000) == 23);
    r.clear();
    r.finalize_column(120000,history);
    r.sync_gpu_from_timeline();
    assert(r.live_price_qty_.empty());
    assert(metadata[r.meta_texture_][7] == 0);
    r.clear();
    r.set_replay_cutoff_ms(0);
    r.finalize_column(60000, history);
    r.finalize_column(120000, history);
    r.sync_gpu_from_timeline();
    r.update_live_column(185000, {{100, 31}}, 100);
    r.update_live_column(245000, {{100, 41}}, 100);
    // No observation at 300000: this minute must remain absent.
    r.update_live_column(365000, {{100, 61}}, 100);
    assert(r.get_value_at_price_and_time(100.25, 180000) == 31);
    for (int i = 0; i < 3; ++i) {
        r.gpu_dirty_ = true; // Navigation/backfill rebuild before history catches up.
        r.sync_gpu_from_timeline();
        assert(r.get_value_at_price_and_time(100.25, 180000) == 31);
        assert(r.get_value_at_price_and_time(100.25, 240000) == 41);
        assert(r.get_value_at_price_and_time(100.25, 300000) == 0);
        assert(r.has_missing_columns(180000, 360000));
        assert(!r.has_missing_columns(180000, 240000));
        assert(r.has_missing_columns(0, 60000));
        assert(!r.has_missing_columns(400000, 360000));
        assert(r.get_value_at_price_and_time(100.25, 360000) == 61);
    }
    r.finalize_column(180000, {{100, 7}});
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(100.25, 180000) == 7);
    r.timeline_.erase(60000);
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(100.25, 240000) == 41);
    r.set_replay_cutoff_ms(200000);
    r.sync_gpu_from_timeline();
    r.set_replay_cutoff_ms(400000);
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(100.25, 240000) == 0);
    r.clear();
    assert(r.observed_columns_.empty());
    r.set_replay_cutoff_ms(0);
    for (int i = 1; i <= 30; ++i) r.update_live_column(i * 60000, live);
    assert(r.observed_columns_.size() == 10);
    r.clear();
    r.set_replay_cutoff_ms(0);
    // Server returns the last original observation in each requested interval.
    // The source clock must survive, but column centers must be UTC candle opens.
    constexpr int64_t epoch = 1788753600000; // Aligned to 15m.
    for (int64_t tf : {300000LL, 60000LL, 900000LL, 60000LL}) {
        r.set_column_interval_ms(tf);
        r.clear();
        r.native_bucket_size_ = 1;
        const int64_t source = epoch + tf - 7000;
        r.finalize_column(source, history);
        r.finalize_column(source + 5 * tf, live); // Four missing intervals.
        r.sync_gpu_from_timeline();
        assert(r.time_step_ms_ == tf); // Sparse samples never stretch cells.
        assert(r.gpu_origin_ms_ == epoch);
        assert(r.ring_count_ == 6);
        assert(r.column_meta_[0].timestamp_ms == source);
        assert(r.get_value_at_price_and_time(100.25, epoch) == 2);
        assert(r.get_value_at_price_and_time(100.25, epoch + tf) == 0);
        assert(r.get_value_at_price_and_time(100.25, epoch + 5 * tf) == 17);
        assert(r.display_time_to_bucket(epoch - tf * 0.49) == epoch);
        assert(r.display_time_to_bucket(epoch + tf * 0.49) == epoch);
        assert(r.display_time_to_bucket(epoch + tf * 0.51) == epoch + tf);
        r.finalize_column(source + tf, {{100, 13}}); // Direct upload uses same origin.
        assert(r.get_value_at_price_and_time(100.25, epoch + tf) == 13);
        r.set_replay_cutoff_ms(source - 1);
        r.sync_gpu_from_timeline();
        assert(r.get_value_at_price_and_time(100.25, epoch) == 0); // No early evidence.
        r.set_replay_cutoff_ms(0);
    }
    r.set_column_interval_ms(60000);
    r.clear();
    r.native_bucket_size_ = 1;
    r.finalize_column(epoch, history);
    r.finalize_column(epoch + (r.RING_SIZE + 5LL) * 60000, live);
    r.sync_gpu_from_timeline();
    assert(r.time_step_ms_ == 60000); // Bounded GPU window never coarsens time.
    assert(r.ring_count_ == r.RING_SIZE);
    assert(r.get_value_at_price_and_time(100.25, epoch + (r.RING_SIZE + 5LL) * 60000) == 17);
    r.set_bucket_multiplier(2);
    assert(r.get_value_at_price_and_time(100.25, epoch + (r.RING_SIZE + 5LL) * 60000) == 46);
    r.set_bucket_multiplier(1);
    assert(r.get_value_at_price_and_time(100.25, epoch + (r.RING_SIZE + 5LL) * 60000) == 17);
    r.clear();
    r.configure_realtime(1);
    r.set_observation_clock_ms(epoch + 1000);
    r.finalize_column(epoch + 17, {{100, 2}}, true);
    r.finalize_column(epoch + 117, {{100, 3}});
    r.finalize_column(epoch + 417, {{100, 4}}, true);
    r.sync_gpu_from_timeline();
    assert(r.time_step_ms_ == 100);
    assert(r.column_meta_[0].timestamp_ms == epoch + 17);
    assert(std::abs(metadata[r.meta_texture_][3] - 5.17f) < 0.001f);
    assert(std::abs(metadata[r.meta_texture_][7] - 3.17f) < 0.001f);
    assert(metadata[r.meta_texture_][11] == 0); // Missing bin never filled.
    assert(r.timeline_.size() == 3);
    r.finalize_column(epoch + 150, {{100, 99}});
    assert(r.timeline_.size() == 3); // One observed state per temporal bin.
    r.invalidate_observation(epoch + 417);
    r.sync_gpu_from_timeline();
    assert(r.timeline_.size() == 2);
    // Event-driven source: quiet intervals are holds, not missing events.
    r.finalize_column(epoch + 617, {{100, 9}});
    r.sync_gpu_from_timeline();
    assert(r.timeline_.size() == 3); // Only actual observations retained.
    assert(metadata[r.meta_texture_][11] == 4); // 200ms uses prior book.
    assert(r.column_meta_[2].timestamp_ms == epoch + 117);
    assert(r.get_value_at_price_and_time(100.25, epoch + 350) == 3);
    r.set_observation_clock_ms(epoch + 2500);
    r.finalize_column(epoch + 1617, {{100, 12}}); // Once-per-second activity.
    assert(r.column_meta_[10].timestamp_ms == epoch + 617);
    r.finalize_column(epoch + 2017, {{100, 14}}, true);
    r.sync_gpu_from_timeline();
    assert(metadata[r.meta_texture_][18 * 4 + 3] == 0); // Broken sequence stays absent.
    r.set_observation_hold(epoch + 2500);
    assert(r.observation_hold_until_ms_ == epoch + 2500);
    const auto recorded_size = r.timeline_.size();
    assert(r.realtime_draw_until(epoch + 3500, true) == epoch + 3500);
    assert(r.realtime_draw_until(epoch + 3500, false) == epoch + 2500);
    assert(r.realtime_draw_until(epoch + 2000, true) == epoch + 2500);
    assert(r.timeline_.size() == recorded_size);
    assert(r.get_value_at_price_and_time(100.25, epoch + 3500) == 0);
    r.set_observation_clock_ms(epoch + 1900); // A newer retained column cannot project into a rewind.
    assert(r.realtime_draw_until(epoch + 3500, true) == epoch + 1900);
    r.set_observation_clock_ms(epoch + 2500);
    r.set_observation_hold(0);
    assert(r.observation_hold_until_ms_ == 0);
    assert(r.realtime_draw_until(epoch + 3500, true) == epoch + 2500); // Invalid/stale books never extend.
    r.set_observation_clock_ms(epoch + 1000000);
    for (int i = 5; i < 3200; ++i) r.finalize_column(epoch + i * 100 + 17, {{100, float(i)}});
    assert(r.timeline_.size() == 3000);
    r.sync_gpu_from_timeline();
    assert(r.time_step_ms_ == 100 && r.ring_count_ <= 3000);
    r.set_replay_cutoff_ms(epoch + 130017);
    r.sync_gpu_from_timeline();
    for (const auto& column : r.column_meta_)
        if (column.num_rows) assert(column.timestamp_ms <= epoch + 130017);
    // Small-tick RT with distant resting orders must retain the active book
    // through initial batch upload, direct arrival and retention-origin rebuild.
    r.clear();
    r.configure_realtime(0.0000001);
    r.set_observation_clock_ms(epoch + 400000);
    const std::unordered_map<double, float> small_tick{
        {0.00001, 1}, {0.0010299, 230000}, {0.0010300, 340000}, {0.02, 1}};
    r.finalize_column(epoch + 17, small_tick, true, 0.00102995);
    r.sync_gpu_from_timeline();
    assert(r.timeline_.at(epoch + 17).at(0.0010299) == 230000);
    assert(r.get_value_at_price_and_time(0.00102995, epoch + 50) == 230000);
    r.finalize_column(epoch + 1017, small_tick, false, 0.00102995);
    assert(r.get_value_at_price_and_time(0.00102995, epoch + 1050) == 230000);
    assert(r.get_value_at_price_and_time(0.00102995, epoch + 550) == 230000);
    r.gpu_dirty_ = true; // A delayed batch remains CPU-owned until rebuild.
    for (int i = 2; i < 335; ++i)
        r.finalize_column(epoch + i * 1000 + 17, small_tick, false, 0.00102995);
    r.sync_gpu_from_timeline();
    assert(r.gpu_origin_ms_ > epoch);
    assert(r.observation_centers_.size() == r.timeline_.size());
    for (const auto& [ts, levels] : r.timeline_) {
        assert(levels.at(0.0010299) == 230000);
        assert(r.get_value_at_price_and_time(0.00102995, ts + 33) == 230000);
    }
    r.set_replay_cutoff_ms(epoch + 100050);
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(0.00102995, epoch + 99050) == 230000);
    assert(r.get_value_at_price_and_time(0.00102995, epoch + 101050) == 0);
    r.clear();
    assert(r.observation_centers_.empty());
    r.configure_realtime(1);
    r.set_observation_clock_ms(epoch + 1000);
    r.finalize_column(epoch + 17, {{99, 20}, {100, 30}, {101, 40}, {450, 1000000}}, true, 100);
    r.sync_gpu_from_timeline();
    assert(r.realtime_normalization(98, 102) == 40); // Remote wall cannot dim active rows.
    const auto unchanged = r.timeline_.at(epoch + 17);
    r.set_bucket_multiplier(2);
    r.sync_gpu_from_timeline();
    assert(r.realtime_normalization(98, 102) == 70); // Percentile of grouped values 20,70.
    assert(r.timeline_.at(epoch + 17) == unchanged); // Brightness never mutates source volume.
    // New liquidity and a changing price viewport cannot recolor past cells.
    const float peak = r.realtime_normalization(98, 102);
    r.set_observation_clock_ms(epoch + 5000);
    r.finalize_column(epoch + 4017, {{100, 900000}}, false, 104);
    r.sync_gpu_from_timeline();
    assert(r.realtime_normalization(0, 1000) == peak);
    r.recalibrate_realtime_colors();
    assert(r.realtime_normalization(100, 102) == 70); // Brief wall does not dominate recalibration.
    // A persistent wall occupies more than 2% of a narrow grouped view.
    // It must stay large without turning ordinary 5-25M rows nearly black.
    r.clear(); r.configure_realtime(1); r.set_bucket_multiplier(1);
    std::unordered_map<double, float> wall_book;
    for (int i = 0; i < 40; ++i) wall_book[100 + i] = (5 + i % 21) * 1000000.0f;
    wall_book[120] = 205300000;
    for (int i = 0; i < 64; ++i)
        r.finalize_column(epoch + i * 100, wall_book, i == 0, 120);
    r.set_observation_clock_ms(epoch + 6500);
    r.sync_gpu_from_timeline();
    const auto wall_peak = r.realtime_normalization(100, 140);
    assert(wall_peak >= 20000000 && wall_peak <= 25000000);
    assert(r.get_value_at_price_and_time(120.5, epoch + 3000) == 205300000);
    r.set_observation_clock_ms(epoch + 7000);
    wall_book[120] = 500000000;
    r.finalize_column(epoch + 6900, wall_book, false, 120);
    r.sync_gpu_from_timeline();
    assert(r.realtime_normalization(100, 140) == wall_peak);
    // Surviving columns must preserve both metadata and uploaded values across
    // direct arrivals, rebuilds and retirement of their original predecessor.
    for (int mult : {1, 2, 5, 10, 20}) {
        r.clear(); r.configure_realtime(0.0000001); r.set_bucket_multiplier(mult);
        r.set_observation_clock_ms(epoch + 400000);
        r.finalize_column(epoch + 17, small_tick, true, 0.00102005);
        r.sync_gpu_from_timeline();
        r.finalize_column(epoch + 1017, small_tick, false, 0.00102995);
        const auto before = r.column_meta_[r.find_column_for_time(epoch + 1050)];
        r.gpu_dirty_ = true; r.sync_gpu_from_timeline();
        auto after = r.column_meta_[r.find_column_for_time(epoch + 1050)];
        assert(before.price_min == after.price_min && before.values == after.values);
        r.timeline_.erase(epoch + 17); r.gpu_dirty_ = true; r.sync_gpu_from_timeline();
        after = r.column_meta_[r.find_column_for_time(epoch + 1050)];
        assert(before.price_min == after.price_min && before.values == after.values);
    }
    r.clear(); r.configure_realtime(0.1); r.set_observation_clock_ms(epoch + 1000000);
    r.finalize_column(epoch + 17, {{100, 3}}, true, 100);
    r.sync_gpu_from_timeline();
    const auto origin = r.gpu_origin_ms_;
    r.finalize_column(epoch + 300017, {{100, 7}}, true, 100);
    assert(!r.gpu_dirty_ && r.gpu_origin_ms_ == origin);
    assert(r.get_value_at_price_and_time(100.05, epoch + 50) == 0);
    assert(r.get_value_at_price_and_time(100.05, epoch + 300050) == 7);
    // Exhaust the spare texture span: only then rebase, preserving the sample.
    r.finalize_column(epoch + 819217, {{100, 11}}, true, 100);
    assert(r.gpu_dirty_);
    r.sync_gpu_from_timeline();
    assert(r.get_value_at_price_and_time(100.05, epoch + 819250) == 11);
    r.clear();
    r.configure_realtime(1);r.set_bucket_multiplier(5);
    r.set_observation_clock_ms(epoch+1800000);
    r.finalize_column(epoch+17,{{100,10}},true,100);r.sync_gpu_from_timeline();
    const float history_peak=r.realtime_normalization(95,105);
    r.set_realtime_warm(true);r.clear_realtime_view(1000);
    for(int i=0;i<1800;++i)r.finalize_column(epoch+i*1000+917,{{100,10}},i==0,100);
    r.sync_gpu_from_timeline();
    assert(r.timeline_.size()==1800 && r.ring_count_==1800);
    assert(r.realtime_normalization(95,105)==history_peak && r.realtime_warm());
    assert(r.get_value_at_price_and_time(100.5,epoch+950)==10);
    r.clear_realtime_view(100);r.finalize_column(epoch+17,{{100,10}},true,100);
    r.sync_gpu_from_timeline();
    assert(r.realtime_normalization(95,105)==history_peak && r.realtime_warm());
    assert(r.get_value_at_price_and_time(100.5,epoch+50)==10);
    std::puts("PASS: live depth survives rebuilds, finalization and origin changes; rewind/clear discard it");
}
