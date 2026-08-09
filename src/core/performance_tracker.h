#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <cstdio>

class PerformanceTracker {
public:
    struct Metrics {
        uint64_t total_calls = 0;
        uint64_t total_microseconds = 0;
        uint64_t min_microseconds = UINT64_MAX;
        uint64_t max_microseconds = 0;

        double avg_microseconds() const {
            return total_calls > 0 ? (double)total_microseconds / total_calls : 0.0;
        }
    };

    class Timer {
    public:
        Timer(const std::string& name)
            : name_(name),
              start_(std::chrono::high_resolution_clock::now()) {}

        ~Timer() {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start_).count();
            PerformanceTracker::instance().record(name_, duration);
        }

    private:
        std::string name_;
        std::chrono::high_resolution_clock::time_point start_;
    };

    // ✅ Singleton access
    static PerformanceTracker& instance() {
        static PerformanceTracker tracker;
        return tracker;
    }

    void record(const std::string& name, uint64_t microseconds) {
        auto& m = metrics_[name];
        m.total_calls++;
        m.total_microseconds += microseconds;
        m.min_microseconds = std::min(m.min_microseconds, microseconds);
        m.max_microseconds = std::max(m.max_microseconds, microseconds);
    }

    void print_metrics(const std::string&) {}

    void print_all() {}

    const Metrics* get_metrics(const std::string& name) const {
        auto it = metrics_.find(name);
        return it != metrics_.end() ? &it->second : nullptr;
    }

private:
    PerformanceTracker() = default;
    std::unordered_map<std::string, Metrics> metrics_;
};

// ✅ Convenience macro using singleton
#define PERF_TIMER(name) PerformanceTracker::Timer _perf_timer_##__LINE__(name)