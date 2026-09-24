#pragma once

#include "core/queue_metrics.h"
#include "types/frame_profiler.h"

namespace PerformanceDiagnostics {

// Evaluated once from the browser query string. Only the exact perf=1 token
// enables the contributor surface.
bool enabled();

// One line describing the presentation cadence decision (focus, pointer,
// other windows, refresh estimate), set by main.cpp each poll.
void set_cadence_note(const char* note);

void render(FrameProfiler& profiler,
            const FrameTimeTracker& presentation_intervals,
            const QueueBacklogSnapshot& queues);

}  // namespace PerformanceDiagnostics
