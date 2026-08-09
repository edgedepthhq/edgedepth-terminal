#pragma once

#include <chrono>
#include <string>
#include <cstdio>
#include <algorithm>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// FrameTimeTracker — Ring buffer of frame times with percentile computation
//
// Records the last N frame times and computes P50/P95/P99/max on demand.
// Used by the performance overlay to detect jitter and validate threading gains.
// ═══════════════════════════════════════════════════════════════════════════════

class FrameTimeTracker {
public:
    static constexpr int HISTORY_SIZE = 300;  // ~5 seconds at 60fps

    void record(float frame_time_ms) {
        history_[write_pos_] = frame_time_ms;
        write_pos_ = (write_pos_ + 1) % HISTORY_SIZE;
        if (count_ < HISTORY_SIZE) count_++;
    }

    // Compute percentiles — sorts a copy, so call sparingly (once/sec)
    struct Stats {
        float avg;
        float p50;
        float p95;
        float p99;
        float max;
    };

    Stats compute() const {
        if (count_ == 0) return {0, 0, 0, 0, 0};

        // Copy into scratch buffer and sort
        float sorted[HISTORY_SIZE];
        std::memcpy(sorted, history_, count_ * sizeof(float));
        std::sort(sorted, sorted + count_);

        float sum = 0;
        for (int i = 0; i < count_; i++) sum += sorted[i];

        return Stats{
            .avg = sum / count_,
            .p50 = sorted[count_ / 2],
            .p95 = sorted[(int)(count_ * 0.95f)],
            .p99 = sorted[(int)(count_ * 0.99f)],
            .max = sorted[count_ - 1]
        };
    }

    int count() const { return count_; }
    const float* data() const { return history_; }
    int write_pos() const { return write_pos_; }

private:
    float history_[HISTORY_SIZE] = {};
    int write_pos_ = 0;
    int count_ = 0;
};

inline FrameTimeTracker g_frame_tracker;

/**
 * Frame profiler for identifying bottlenecks (v2 — 2026-07-05 FPS session).
 *
 * - Nesting-safe: each section tracks its own in-flight start, so
 *   begin("Chart") … begin("LiqField") … end("LiqField") … end("Chart") works.
 * - Per-frame section values fold into a 1s window; the completed window is
 *   snapshotted (sorted by avg ms) for the perf overlay's breakdown table.
 * - add_count(name, n) attaches item counts to a section (dispatches drained,
 *   drip messages fed) — shown as N/f in the overlay + console.
 * - Spike log: any frame whose measured CPU time exceeds spike_threshold_ms
 *   prints ONE console line with that frame's top sections. Rate-limited.
 *   This is the tool for catching the replay 90-FPS dips in the act.
 *
 * All calls are main-thread only (widgets render on the main thread; the data
 * thread must NOT touch g_profiler — it has PerformanceTracker instead).
 */
class FrameProfiler {
public:
    static constexpr int MAX_SECTIONS = 48;

    struct Section {
        const char* name = nullptr;
        // Current frame
        double frame_ms = 0.0;
        int    frame_calls = 0;
        long   frame_count = 0;    // attached item count (dispatches etc.)
        std::chrono::steady_clock::time_point inflight_start{};
        bool   inflight = false;
        // Accumulating 1s window
        double win_ms = 0.0;
        int    win_calls = 0;
        long   win_count = 0;
        double win_max_frame_ms = 0.0;   // worst single-frame total for this section
    };

    // Snapshot row for the overlay (last completed 1s window, sorted desc by avg)
    struct DisplayRow {
        const char* name = nullptr;
        float avg_ms = 0.0f;         // avg ms per frame across the window
        float max_ms = 0.0f;         // worst single frame in the window
        float calls_per_frame = 0.0f;
        float count_per_frame = 0.0f; // attached item count avg (0 if unused)
    };

    void begin(const char* name) {
        Section& s = find_or_create(name);
        s.inflight_start = std::chrono::steady_clock::now();
        s.inflight = true;
    }

    void end(const char* name) {
        const auto now = std::chrono::steady_clock::now();
        Section& s = find_or_create(name);
        if (!s.inflight) return;
        s.inflight = false;
        s.frame_ms += std::chrono::duration<double, std::milli>(now - s.inflight_start).count();
        s.frame_calls++;
    }

    // Attach an item count to a section for this frame (e.g. dispatches drained).
    void add_count(const char* name, long n) {
        if (n <= 0) return;
        find_or_create(name).frame_count += n;
    }

    // frame_cpu_ms: the CPU time of THIS frame (main_loop entry → end), used for
    // spike attribution. Pass 0 to skip spike detection for the frame.
    void end_frame(double frame_cpu_ms = 0.0) {
        frame_count_++;
        (void)frame_cpu_ms;

        // ── Fold frame values into the window
        for (int i = 0; i < section_count_; i++) {
            Section& s = sections_[i];
            s.win_ms    += s.frame_ms;
            s.win_calls += s.frame_calls;
            s.win_count += s.frame_count;
            if (s.frame_ms > s.win_max_frame_ms) s.win_max_frame_ms = s.frame_ms;
            s.frame_ms = 0.0;
            s.frame_calls = 0;
            s.frame_count = 0;
        }

        // ── Window rollover (1s): snapshot for the overlay (+ optional console)
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_print_time_).count();
        if (elapsed >= 1.0) {
            const int frames = frame_count_ > 0 ? frame_count_ : 1;

            // Build sorted display snapshot
            int order[MAX_SECTIONS];
            for (int i = 0; i < section_count_; i++) order[i] = i;
            std::sort(order, order + section_count_, [this](int a, int b) {
                return sections_[a].win_ms > sections_[b].win_ms;
            });
            display_count_ = 0;
            for (int i = 0; i < section_count_ && display_count_ < MAX_SECTIONS; i++) {
                const Section& s = sections_[order[i]];
                if (s.win_calls == 0 && s.win_count == 0) continue;
                DisplayRow& r = display_[display_count_++];
                r.name = s.name;
                r.avg_ms = static_cast<float>(s.win_ms / frames);
                r.max_ms = static_cast<float>(s.win_max_frame_ms);
                r.calls_per_frame = static_cast<float>(s.win_calls) / static_cast<float>(frames);
                r.count_per_frame = static_cast<float>(s.win_count) / static_cast<float>(frames);
            }
            display_fps_ = static_cast<float>(frames / elapsed);
            display_spikes_ = spikes_in_window_;
            spikes_in_window_ = 0;

            // Reset window
            for (int i = 0; i < section_count_; i++) {
                sections_[i].win_ms = 0.0;
                sections_[i].win_calls = 0;
                sections_[i].win_count = 0;
                sections_[i].win_max_frame_ms = 0.0;
            }
            frame_count_ = 0;
            last_print_time_ = now;
        }
    }

    // ── Overlay accessors (last completed window) ────────────────────────────
    int display_count() const { return display_count_; }
    const DisplayRow& display(int i) const { return display_[i]; }
    float display_fps() const { return display_fps_; }
    int display_spikes() const { return display_spikes_; }

    // Console dump is OFF by default. Flip via the Perf overlay checkbox.
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // Spike logging is ON by default — one console line per >threshold frame,
    // rate-limited to 5/s. The replay-dip diagnosis tool.
    void set_spike_log(bool on) { spike_log_ = on; }
    bool spike_log() const { return spike_log_; }
    void set_spike_threshold_ms(double ms) { spike_threshold_ms_ = ms; }
    double spike_threshold_ms() const { return spike_threshold_ms_; }

private:
    Section& find_or_create(const char* name) {
        // Pointer-compare fast path (string literals dedupe within a TU),
        // strcmp fallback (same literal from another TU).
        for (int i = 0; i < section_count_; i++) {
            if (sections_[i].name == name) return sections_[i];
        }
        for (int i = 0; i < section_count_; i++) {
            if (std::strcmp(sections_[i].name, name) == 0) return sections_[i];
        }
        if (section_count_ < MAX_SECTIONS) {
            sections_[section_count_].name = name;
            return sections_[section_count_++];
        }
        return sections_[MAX_SECTIONS - 1];  // overflow: dump into the last slot
    }

    Section sections_[MAX_SECTIONS];
    int section_count_ = 0;
    int frame_count_ = 0;
    bool enabled_ = false;
    bool spike_log_ = true;
    double spike_threshold_ms_ = 12.0;
    int spikes_in_window_ = 0;

    DisplayRow display_[MAX_SECTIONS];
    int display_count_ = 0;
    float display_fps_ = 0.0f;
    int display_spikes_ = 0;

    std::chrono::steady_clock::time_point last_print_time_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_spike_log_{};
};

// Global instance for easy access
inline FrameProfiler g_profiler;

// Convenience macros
#define PROFILE_BEGIN(name) g_profiler.begin(name)
#define PROFILE_END(name) g_profiler.end(name)
#define PROFILE_FRAME_END() g_profiler.end_frame()

// RAII section scope — safe with early returns. Usage:
//   { ProfileScope _ps("LiqField"); render_liq_dense_field(); }
struct ProfileScope {
    const char* name;
    explicit ProfileScope(const char* n) : name(n) { g_profiler.begin(name); }
    ~ProfileScope() { g_profiler.end(name); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};