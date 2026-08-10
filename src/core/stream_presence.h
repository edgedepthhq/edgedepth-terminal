#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// stream_presence.h - which wire streams have ever delivered a frame.
//
// Several panels are driven by EdgeDepth's hosted analytics streams (VPIN,
// positioning, scanner scores, modelled liquidation levels). On a self-hosted
// raw-data feed those streams never arrive and the panels sit blank forever,
// which reads as broken. Entitlements cannot distinguish the cases: bare mode
// defaults to Pro. What is accurate is frame arrival itself: a stream that was
// subscribed a grace period ago and has never delivered one frame is absent,
// whether the cause is a self-hosted feed or a hosted outage, and the panel
// can say so instead of staying wordlessly empty.
//
// Threading: note_subscribed runs on the main thread (StreamManager's
// send_subscribe), note_frame on the data thread (route_message). Both sides
// are lock-free atomics; readers run every frame and cost two loads.
// ═══════════════════════════════════════════════════════════════════════════════
#include <atomic>
#include <chrono>
#include <cstdint>

class StreamPresence {
public:
    // Long enough for a WS connect, a subscribe round trip and the slowest
    // hosted analytics cadence; short enough that a self-hoster meets the
    // explanation on their first look at the panel, not after giving up.
    static constexpr int64_t kGraceMs = 25'000;

    // Session-global by design (mirrors how the data itself behaves: one wire,
    // one session), and reachable from render code that has no AppContext.
    static StreamPresence& instance() {
        static StreamPresence p;
        return p;
    }

    void note_subscribed(uint32_t stream) {
        if (stream >= kMaxStreams) return;
        int64_t expected = 0;
        first_sub_ms_[stream].compare_exchange_strong(expected, now_ms(),
                                                      std::memory_order_relaxed);
    }

    void note_frame(uint32_t stream) {
        if (stream >= kMaxStreams) return;
        seen_[stream].store(true, std::memory_order_relaxed);
    }

    bool seen(uint32_t stream) const {
        return stream < kMaxStreams && seen_[stream].load(std::memory_order_relaxed);
    }

    // True when the stream was subscribed at least kGraceMs ago and no frame
    // has EVER arrived on it. False while unsubscribed or inside the grace
    // window, so panels never flash the explanation during a normal boot.
    bool absent(uint32_t stream) const {
        if (stream >= kMaxStreams) return false;
        if (seen_[stream].load(std::memory_order_relaxed)) return false;
        const int64_t sub = first_sub_ms_[stream].load(std::memory_order_relaxed);
        return sub != 0 && now_ms() - sub >= kGraceMs;
    }

private:
    StreamPresence() = default;

    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    static constexpr uint32_t kMaxStreams = 64;
    std::atomic<int64_t> first_sub_ms_[kMaxStreams] {};
    std::atomic<bool>    seen_[kMaxStreams] {};
};
