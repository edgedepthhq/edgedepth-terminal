#include "performance_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include "core/url_router.h"
#include "imgui.h"
#include "rendering/theme.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace PerformanceDiagnostics {
namespace {

bool query_has_perf(std::string_view query) {
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);
    while (!query.empty()) {
        const std::size_t separator = query.find('&');
        const std::string_view token = query.substr(0, separator);
        if (token == "perf=1") return true;
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return false;
}

void section_label(const char* label) {
    ImGui::PushFont(Theme::Fonts::label());
    ImGui::TextColored(Theme::Tokens::TX3, "%s", label);
    ImGui::PopFont();
}

void render_duration_stats(const FrameTimeTracker::Stats& stats) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    ImGui::TextColored(Theme::Tokens::TX1,
                       "AVG %.2f  P50 %.2f  P95 %.2f ms",
                       stats.avg, stats.p50, stats.p95);
    ImGui::TextColored(Theme::Tokens::TX2, "P99 %.2f  MAX %.2f ms",
                       stats.p99, stats.max);
    ImGui::PopFont();
}

void render_cpu_stats(const FrameProfiler::SampleStats& stats) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    ImGui::TextColored(Theme::Tokens::TX1,
                       "AVG %.2f  P50 %.2f  P95 %.2f ms",
                       stats.avg, stats.p50, stats.p95);
    ImGui::TextColored(Theme::Tokens::TX2, "P99 %.2f  MAX %.2f ms",
                       stats.p99, stats.max);
    ImGui::PopFont();
}

void render_queue_row(const char* label, const QueueDepthSnapshot& depth) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(Theme::Tokens::TX2, "%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%llu", static_cast<unsigned long long>(depth.current));
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%llu", static_cast<unsigned long long>(depth.high_water));
}

const FrameProfiler::DisplayRow* find_section(const FrameProfiler::Snapshot& snapshot,
                                              const char* name) {
    for (int i = 0; i < snapshot.section_count; ++i) {
        const auto& row = snapshot.sections[static_cast<std::size_t>(i)];
        if (row.name != nullptr && std::string_view(row.name) == name) return &row;
    }
    return nullptr;
}

}  // namespace

bool enabled() {
    static const bool is_enabled = query_has_perf(url_get_current_search());
    return is_enabled;
}

void render(FrameProfiler& profiler,
            const FrameTimeTracker& presentation_intervals,
            const QueueBacklogSnapshot& queues) {
    profiler.begin("PerfUI");

    const FrameProfiler::Snapshot& snapshot = profiler.snapshot();
    struct PresentationCache {
        std::uint64_t profiler_generation = std::numeric_limits<std::uint64_t>::max();
        FrameTimeTracker::Stats stats{};
        int scheduler_mode = -1;
        int scheduler_value = 0;
    };
    static PresentationCache presentation_cache;
    if (presentation_cache.profiler_generation != snapshot.generation) {
        presentation_cache.profiler_generation = snapshot.generation;
        presentation_cache.stats = presentation_intervals.compute();
#ifdef __EMSCRIPTEN__
        emscripten_get_main_loop_timing(&presentation_cache.scheduler_mode,
                                        &presentation_cache.scheduler_value);
#endif
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::clamp(460.0f, 320.0f,
                                   std::max(320.0f, viewport->Size.x - 24.0f));
    const float height = std::clamp(820.0f, 260.0f,
                                    std::max(260.0f, viewport->Size.y - 24.0f));
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x - 12.0f, viewport->Pos.y + 12.0f),
        ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD1);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
    const bool visible = ImGui::Begin("Performance diagnostics##perf", nullptr, flags);
    if (visible) {
        const auto& presentation = presentation_cache.stats;

        section_label("PRESENTATION CADENCE");
        const float imgui_fps = ImGui::GetIO().Framerate;
        if (std::isfinite(imgui_fps) && imgui_fps > 0.0f) {
            ImGui::PushFont(Theme::Fonts::mono());
            ImGui::TextColored(Theme::Tokens::BRAND_TX, "%.1f FPS", imgui_fps);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(Theme::Tokens::TX3, "ImGui-smoothed browser cadence");
        } else {
            ImGui::TextColored(Theme::Tokens::TX3, "Collecting presentation callbacks...");
        }

        if (presentation_cache.scheduler_mode == 0) {
            ImGui::TextColored(Theme::Tokens::TX2, "Scheduler: setTimeout (%d ms)",
                               presentation_cache.scheduler_value);
        } else if (presentation_cache.scheduler_mode == 1) {
            ImGui::TextColored(Theme::Tokens::TX2,
                               "Scheduler: requestAnimationFrame (every %d callback%s)",
                               presentation_cache.scheduler_value,
                               presentation_cache.scheduler_value == 1 ? "" : "s");
        } else if (presentation_cache.scheduler_mode == 2) {
            ImGui::TextColored(Theme::Tokens::TX2, "Scheduler: setImmediate");
        }

        if (presentation.has_samples()) {
            render_duration_stats(presentation);
            ImGui::TextColored(
                Theme::Tokens::TX3, "Entry-to-entry, browser scheduling/idle included (%zu/%zu callbacks)",
                presentation.sample_count, FrameTimeTracker::HISTORY_SIZE);
        } else {
            ImGui::TextColored(Theme::Tokens::TX3, "No complete callback interval yet");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        section_label("MAIN-THREAD CPU");
        if (snapshot.ready && snapshot.cpu.has_samples()) {
            render_cpu_stats(snapshot.cpu);
            ImGui::TextColored(Theme::Tokens::TX3,
                               "Completed %.2fs window, %zu CPU samples",
                               snapshot.window_seconds, snapshot.cpu.sample_count);

            if (presentation.has_samples() && presentation.p50 > 0.0) {
                const double headroom_ms = presentation.p50 - snapshot.cpu.p95;
                const double headroom_pct = 100.0 * headroom_ms / presentation.p50;
                const ImVec4& color = headroom_ms >= 0.0
                    ? Theme::Tokens::UP
                    : Theme::Tokens::DOWN;
                ImGui::PushFont(Theme::Fonts::mono_sm());
                ImGui::TextColored(color, "CPU headroom vs present P50: %+.2f ms (%+.0f%%)",
                                   headroom_ms, headroom_pct);
                ImGui::PopFont();
            } else {
                ImGui::TextColored(Theme::Tokens::TX3,
                                   "CPU headroom waits for presentation samples");
            }

            ImGui::TextColored(
                Theme::Tokens::TX2, "CPU spikes > %.1f ms: %llu window, %llu since boot",
                profiler.spike_threshold_ms(),
                static_cast<unsigned long long>(snapshot.cpu_spikes),
                static_cast<unsigned long long>(snapshot.total_cpu_spikes));
        } else {
            ImGui::TextColored(Theme::Tokens::TX3,
                               "Collecting the first completed CPU window...");
        }
        ImGui::TextColored(Theme::Tokens::TX3,
                           "CPU ends after GL submission and swap return. No GPU timer query.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        section_label("QUEUE BACKLOG");
        ImGui::PushFont(Theme::Fonts::mono_sm());
        if (ImGui::BeginTable("##perf_queues", 3,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("High", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableHeadersRow();
            render_queue_row("Raw inbound", queues.inbound);
            render_queue_row("Pending dispatch", queues.pending_dispatch);
            render_queue_row("Main carry", queues.carry);
            render_queue_row("Total", queues.total);
            ImGui::EndTable();
        }
        ImGui::PopFont();
        ImGui::TextColored(Theme::Tokens::TX3, "High-water counts are lifetime values");

        ImGui::Spacing();
        section_label("WORK COUNTS");
        const FrameProfiler::DisplayRow* dispatch = find_section(snapshot, "Dispatch");
        const FrameProfiler::DisplayRow* drip = find_section(snapshot, "Drip");
        ImGui::PushFont(Theme::Fonts::mono_sm());
        ImGui::TextColored(
            Theme::Tokens::TX2, "Dispatch %llu/window (%.2f/frame)",
            static_cast<unsigned long long>(dispatch ? dispatch->count_total : 0),
            dispatch ? dispatch->count_per_frame : 0.0);
        ImGui::TextColored(
            Theme::Tokens::TX2, "Replay drip %llu/window (%.2f/frame)",
            static_cast<unsigned long long>(drip ? drip->count_total : 0),
            drip ? drip->count_per_frame : 0.0);
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        section_label("SECTION CPU");
        ImGui::TextColored(Theme::Tokens::TX3,
                           "Per-frame average and worst frame; nested rows can overlap");
        ImGui::PushFont(Theme::Fonts::mono_sm());
        if (snapshot.ready && snapshot.section_count > 0 &&
            ImGui::BeginTable("##perf_sections", 4,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("Avg ms", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Items/f", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < snapshot.section_count; ++i) {
                const auto& row = snapshot.sections[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(Theme::Tokens::TX2, "%s", row.name ? row.name : "-");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", row.avg_ms);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", row.max_ms);
                ImGui::TableSetColumnIndex(3);
                if (row.count_total > 0) ImGui::Text("%.2f", row.count_per_frame);
                else ImGui::TextColored(Theme::Tokens::TX4, "-");
            }
            ImGui::EndTable();
        } else if (!snapshot.ready) {
            ImGui::TextColored(Theme::Tokens::TX3, "Collecting section timings...");
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    profiler.end("PerfUI");
}

}  // namespace PerformanceDiagnostics
