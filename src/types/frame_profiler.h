#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Fixed-size ring used for presentation intervals and CPU samples. Percentiles
// use the nearest-rank definition: rank = ceil(p * N), with one-based ranks.
// Empty trackers return an all-zero Stats value with sample_count == 0.
template <std::size_t Capacity>
class FixedSampleTracker {
public:
    static_assert(Capacity > 0);
    static constexpr std::size_t HISTORY_SIZE = Capacity;

    struct Stats {
        double avg = 0.0;
        double p50 = 0.0;
        double p95 = 0.0;
        double p99 = 0.0;
        double max = 0.0;
        std::size_t sample_count = 0;

        [[nodiscard]] bool has_samples() const { return sample_count > 0; }
    };

    // Invalid and negative durations are ignored so one bad browser callback
    // cannot introduce NaN or infinity into every later snapshot.
    bool record(double sample_ms) {
        if (!std::isfinite(sample_ms) || sample_ms < 0.0) return false;
        history_[write_pos_] = sample_ms;
        write_pos_ = (write_pos_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
        return true;
    }

    // Sorts a fixed-size stack copy. Call sparingly, such as once per second.
    [[nodiscard]] Stats compute() const {
        if (count_ == 0) return {};

        std::array<double, Capacity> sorted{};
        std::copy_n(history_.begin(), count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));

        // Incremental mean avoids overflowing an intermediate sum for large,
        // but still finite, duration samples.
        double avg = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            avg += (sorted[i] - avg) / static_cast<double>(i + 1);
        }

        return Stats{
            .avg = avg,
            .p50 = sorted[nearest_rank_index(count_, 50)],
            .p95 = sorted[nearest_rank_index(count_, 95)],
            .p99 = sorted[nearest_rank_index(count_, 99)],
            .max = sorted[count_ - 1],
            .sample_count = count_,
        };
    }

    void clear() {
        write_pos_ = 0;
        count_ = 0;
    }

    [[nodiscard]] std::size_t count() const { return count_; }
    [[nodiscard]] const double* data() const { return history_.data(); }
    [[nodiscard]] std::size_t write_pos() const { return write_pos_; }

private:
    static std::size_t nearest_rank_index(std::size_t count, std::size_t percentile) {
        const std::size_t rank = (count * percentile + 99) / 100;
        return rank > 0 ? rank - 1 : 0;
    }

    std::array<double, Capacity> history_{};
    std::size_t write_pos_ = 0;
    std::size_t count_ = 0;
};

// Presentation intervals are main-loop entry to main-loop entry. They include
// browser scheduling and idle time, so they are not CPU or GPU frame duration.
// The window is explicitly the last 300 callbacks.
using FrameTimeTracker = FixedSampleTracker<300>;
inline FrameTimeTracker g_frame_tracker;

/**
 * Main-thread frame profiler.
 *
 * Section values and CPU frame samples accumulate into an explicit wall-clock
 * reporting window of at least one second. The last completed window is copied
 * into Snapshot and remains stable while the next window accumulates.
 *
 * CPU frame time is main_loop entry through update, ImGui rendering, GL command
 * submission, and SDL swap return. It does not claim GPU completion time.
 */
class FrameProfiler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int MAX_SECTIONS = 48;
    static constexpr std::size_t CPU_HISTORY_SIZE = 2048;
    using CpuTracker = FixedSampleTracker<CPU_HISTORY_SIZE>;
    using SampleStats = CpuTracker::Stats;

    struct Section {
        const char* name = nullptr;

        // Current frame.
        double frame_ms = 0.0;
        int frame_calls = 0;
        std::uint64_t frame_count = 0;
        Clock::time_point inflight_start{};
        bool inflight = false;

        // Accumulating reporting window.
        double window_ms = 0.0;
        std::uint64_t window_calls = 0;
        std::uint64_t window_count = 0;
        double window_max_frame_ms = 0.0;
    };

    struct DisplayRow {
        const char* name = nullptr;
        double avg_ms = 0.0;              // Average per frame, including zero-cost frames.
        double max_ms = 0.0;              // Worst single-frame total in the window.
        double calls_per_frame = 0.0;
        double count_per_frame = 0.0;     // Attached item count average.
        std::uint64_t count_total = 0;    // Attached items in the completed window.
    };

    struct Snapshot {
        bool ready = false;
        std::uint64_t generation = 0;
        double window_seconds = 0.0;
        std::uint64_t frame_count = 0;
        double loop_hz = 0.0;
        SampleStats cpu{};
        std::uint64_t cpu_spikes = 0;
        std::uint64_t total_cpu_spikes = 0;
        std::array<DisplayRow, MAX_SECTIONS> sections{};
        int section_count = 0;
    };

    FrameProfiler() = default;

    // Deterministic-clock constructor for native tests.
    explicit FrameProfiler(Clock::time_point window_start)
        : window_start_(window_start), window_started_(true) {}

    void begin(const char* name) {
        Section& section = find_or_create(name);
        section.inflight_start = Clock::now();
        section.inflight = true;
    }

    void end(const char* name) {
        const auto now = Clock::now();
        Section& section = find_or_create(name);
        if (!section.inflight) return;
        section.inflight = false;
        section.frame_ms +=
            std::chrono::duration<double, std::milli>(now - section.inflight_start).count();
        ++section.frame_calls;
    }

    // Attach a processed-item count to a section for this frame.
    void add_count(const char* name, long count) {
        if (count <= 0) return;
        find_or_create(name).frame_count += static_cast<std::uint64_t>(count);
    }

    void end_frame(double frame_cpu_ms = 0.0) {
        end_frame_at(frame_cpu_ms, Clock::now());
    }

    // Public for deterministic native tests. Runtime callers should use
    // end_frame(), normally through ProfileFrameScope.
    void end_frame_at(double frame_cpu_ms, Clock::time_point now) {
        ++frame_count_;

        // A zero value is the legacy "not supplied" sentinel used by
        // PROFILE_FRAME_END(). Real main-loop durations are strictly positive.
        const bool cpu_sample_valid = frame_cpu_ms > 0.0 && cpu_samples_.record(frame_cpu_ms);
        if (cpu_sample_valid && frame_cpu_ms > spike_threshold_ms_) {
            ++spikes_in_window_;
            ++total_spikes_;
            maybe_log_spike(frame_cpu_ms, now);
        }

        fold_frame_sections();

        // Static construction can happen well before the browser starts the
        // first callback. Start the first reporting window at the first frame
        // end so startup delay cannot create a one-sample completed snapshot.
        if (!window_started_) {
            window_start_ = now;
            window_started_ = true;
            return;
        }

        const double elapsed = std::chrono::duration<double>(now - window_start_).count();
        if (elapsed >= 1.0) rollover(elapsed, now);
    }

    [[nodiscard]] const Snapshot& snapshot() const { return snapshot_; }

    // Compatibility accessors for existing and contributor-side callers.
    [[nodiscard]] int display_count() const { return snapshot_.section_count; }
    [[nodiscard]] const DisplayRow& display(int index) const {
        return snapshot_.sections[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] float display_fps() const { return static_cast<float>(snapshot_.loop_hz); }
    [[nodiscard]] std::uint64_t display_spikes() const { return snapshot_.cpu_spikes; }

    // CPU spike diagnostics are disabled by default. When explicitly enabled,
    // they are emitted to stdout and rate-limited to at most four lines/second.
    void set_spike_log(bool enabled) { spike_log_ = enabled; }
    [[nodiscard]] bool spike_log() const { return spike_log_; }

    void set_spike_threshold_ms(double threshold_ms) {
        if (std::isfinite(threshold_ms) && threshold_ms >= 0.0) {
            spike_threshold_ms_ = threshold_ms;
        }
    }
    [[nodiscard]] double spike_threshold_ms() const { return spike_threshold_ms_; }
    [[nodiscard]] std::uint64_t total_spikes() const { return total_spikes_; }

private:
    void fold_frame_sections() {
        for (int i = 0; i < section_count_; ++i) {
            Section& section = sections_[static_cast<std::size_t>(i)];
            section.window_ms += section.frame_ms;
            section.window_calls += static_cast<std::uint64_t>(section.frame_calls);
            section.window_count += section.frame_count;
            section.window_max_frame_ms =
                std::max(section.window_max_frame_ms, section.frame_ms);
            section.frame_ms = 0.0;
            section.frame_calls = 0;
            section.frame_count = 0;
        }
    }

    void rollover(double elapsed, Clock::time_point now) {
        Snapshot next{};
        next.ready = true;
        next.generation = snapshot_.generation + 1;
        next.window_seconds = elapsed;
        next.frame_count = frame_count_;
        next.loop_hz = elapsed > 0.0 ? static_cast<double>(frame_count_) / elapsed : 0.0;
        next.cpu = cpu_samples_.compute();
        next.cpu_spikes = spikes_in_window_;
        next.total_cpu_spikes = total_spikes_;

        std::array<int, MAX_SECTIONS> order{};
        for (int i = 0; i < section_count_; ++i) {
            order[static_cast<std::size_t>(i)] = i;
        }
        std::sort(order.begin(), order.begin() + section_count_, [this](int lhs, int rhs) {
            return sections_[static_cast<std::size_t>(lhs)].window_ms >
                   sections_[static_cast<std::size_t>(rhs)].window_ms;
        });

        const double frames = frame_count_ > 0 ? static_cast<double>(frame_count_) : 1.0;
        for (int i = 0; i < section_count_ && next.section_count < MAX_SECTIONS; ++i) {
            const Section& section =
                sections_[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])];
            if (section.window_calls == 0 && section.window_count == 0) continue;

            DisplayRow& row = next.sections[static_cast<std::size_t>(next.section_count++)];
            row.name = section.name;
            row.avg_ms = section.window_ms / frames;
            row.max_ms = section.window_max_frame_ms;
            row.calls_per_frame = static_cast<double>(section.window_calls) / frames;
            row.count_per_frame = static_cast<double>(section.window_count) / frames;
            row.count_total = section.window_count;
        }

        snapshot_ = next;

        for (int i = 0; i < section_count_; ++i) {
            Section& section = sections_[static_cast<std::size_t>(i)];
            section.window_ms = 0.0;
            section.window_calls = 0;
            section.window_count = 0;
            section.window_max_frame_ms = 0.0;
        }
        cpu_samples_.clear();
        frame_count_ = 0;
        spikes_in_window_ = 0;
        window_start_ = now;
    }

    void maybe_log_spike(double frame_cpu_ms, Clock::time_point now) {
        if (!spike_log_) return;
        if (last_spike_log_ != Clock::time_point{} &&
            now - last_spike_log_ < std::chrono::milliseconds(250)) {
            return;
        }
        last_spike_log_ = now;

        std::array<int, MAX_SECTIONS> order{};
        for (int i = 0; i < section_count_; ++i) {
            order[static_cast<std::size_t>(i)] = i;
        }
        std::sort(order.begin(), order.begin() + section_count_, [this](int lhs, int rhs) {
            return sections_[static_cast<std::size_t>(lhs)].frame_ms >
                   sections_[static_cast<std::size_t>(rhs)].frame_ms;
        });

        char line[768];
        int used = std::snprintf(line, sizeof(line), "[Perf] CPU spike %.2f ms", frame_cpu_ms);
        if (used < 0) return;
        std::size_t offset = std::min<std::size_t>(static_cast<std::size_t>(used), sizeof(line) - 1);
        int included = 0;
        for (int i = 0; i < section_count_ && included < 5; ++i) {
            const Section& section =
                sections_[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])];
            if (section.frame_ms <= 0.0 || section.name == nullptr) continue;
            const int wrote = std::snprintf(line + offset, sizeof(line) - offset,
                                            " | %s %.2f", section.name, section.frame_ms);
            if (wrote < 0) break;
            if (static_cast<std::size_t>(wrote) >= sizeof(line) - offset) {
                offset = sizeof(line) - 1;
                break;
            }
            offset += static_cast<std::size_t>(wrote);
            ++included;
        }
        line[offset] = '\0';
        std::printf("%s\n", line);
    }

    Section& find_or_create(const char* name) {
        // Pointer comparison is the common string-literal fast path. strcmp
        // keeps identical labels from different translation units deduplicated.
        for (int i = 0; i < section_count_; ++i) {
            if (sections_[static_cast<std::size_t>(i)].name == name) {
                return sections_[static_cast<std::size_t>(i)];
            }
        }
        for (int i = 0; i < section_count_; ++i) {
            if (std::strcmp(sections_[static_cast<std::size_t>(i)].name, name) == 0) {
                return sections_[static_cast<std::size_t>(i)];
            }
        }
        if (section_count_ < MAX_SECTIONS) {
            Section& created = sections_[static_cast<std::size_t>(section_count_++)];
            created.name = name;
            return created;
        }
        return sections_[MAX_SECTIONS - 1];
    }

    std::array<Section, MAX_SECTIONS> sections_{};
    int section_count_ = 0;
    CpuTracker cpu_samples_{};
    std::uint64_t frame_count_ = 0;
    std::uint64_t spikes_in_window_ = 0;
    std::uint64_t total_spikes_ = 0;

    bool spike_log_ = false;
    double spike_threshold_ms_ = 12.0;
    Clock::time_point last_spike_log_{};

    Snapshot snapshot_{};
    Clock::time_point window_start_{};
    bool window_started_ = false;
};

inline FrameProfiler g_profiler;

#define PROFILE_BEGIN(name) g_profiler.begin(name)
#define PROFILE_END(name) g_profiler.end(name)
#define PROFILE_FRAME_END() g_profiler.end_frame()

// Ensures every main-loop exit records CPU duration, including early exits
// caused by a temporarily invalid or zero-sized browser viewport.
class ProfileFrameScope {
public:
    explicit ProfileFrameScope(FrameProfiler& profiler,
                               FrameProfiler::Clock::time_point start =
                                   FrameProfiler::Clock::now())
        : profiler_(profiler), start_(start) {}

    ~ProfileFrameScope() { finish(); }

    void finish() {
        if (finished_) return;
        finished_ = true;
        const double cpu_ms = std::chrono::duration<double, std::milli>(
            FrameProfiler::Clock::now() - start_).count();
        profiler_.end_frame(cpu_ms);
    }

    ProfileFrameScope(const ProfileFrameScope&) = delete;
    ProfileFrameScope& operator=(const ProfileFrameScope&) = delete;

private:
    FrameProfiler& profiler_;
    FrameProfiler::Clock::time_point start_;
    bool finished_ = false;
};

// RAII section scope, safe across early returns.
struct ProfileScope {
    const char* name;
    explicit ProfileScope(const char* section_name) : name(section_name) {
        g_profiler.begin(name);
    }
    ~ProfileScope() { g_profiler.end(name); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};
