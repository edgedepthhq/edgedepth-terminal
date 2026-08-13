#pragma once

#include "core/queue_metrics.h"
#include "types/frame_profiler.h"

namespace PerformanceDiagnostics {

// Evaluated once from the browser query string. Only the exact perf=1 token
// enables the contributor surface.
bool enabled();

void render(FrameProfiler& profiler,
            const FrameTimeTracker& presentation_intervals,
            const QueueBacklogSnapshot& queues);

}  // namespace PerformanceDiagnostics
