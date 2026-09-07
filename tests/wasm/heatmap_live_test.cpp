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
    std::puts("PASS: live depth survives rebuilds, finalization and origin changes; rewind/clear discard it");
}
