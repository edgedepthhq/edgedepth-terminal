#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// education_boot.h - Education-layer startup seam (Guided Replay / Course Studio)
//
// The terminal binary is ONE canvas with three host chromes (bare Terminal,
// Guided Replay, Course Studio). When the client is embedded in the EdgeDepth
// web app for a lesson, the host page sets a config global BEFORE the glue loads:
//
//     window.__EDGEDEPTH_LESSON__ = { docUrl: "/api/lesson-doc/1", mode: "lesson" };
//
// This module reads that global at startup and, if present, fetches the LessonDoc
// from docUrl with credentials (so Payload's auth cookie rides along and the
// paywall still gates the *data*, not just the page). The fetched JSON is the
// exact contract shape produced by toLessonDoc.ts / lesson-schema.json.
//
// Fetch strategy mirrors SymbolRegistry::fetch_metadata - EM_ASM + XMLHttpRequest
// + _malloc/stringToUTF8 -> extern "C" callback -> _free. No -sFETCH, no Asyncify.
//
// V1 SCOPE: read global, fetch doc, stash it, log it. The beat/track replay
// runtime (see edgedepth-education/IMGUI-NOTES.md) consumes EducationBoot::lesson_json()
// in a later session.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <functional>
#include <cstdint>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

class EducationBoot {
public:
    static EducationBoot& instance() {
        static EducationBoot inst;
        return inst;
    }

    // The host chrome we're embedded in. Lesson = Guided Replay player (gate loop
    // + cards, doc fetched at boot). Studio = Course Authoring (free scrub + draw
    // overlay, NO doc - the replay window is chosen later in the picker and pushed
    // in via __studio_cmd_*). None = standalone live terminal.
    enum class Mode { None, Lesson, Studio, Event, Pack };

    // True when the host page embedded us in a React chrome that OWNS the shell +
    // transport and drives the data via replay - lesson, studio, OR archived event.
    // Drives the SHARED embedded path: replay-driven candles (skip the live initial
    // load), NO native shell/topbar, NO native ImGui control bar (React renders the
    // transport OUTSIDE the canvas), and the focused single-symbol widget set (no
    // Watchlist). Event mode now takes this path too - its archive replay is driven
    // by the React EventReplayBar (eventBridge → EventRuntime), mirroring the lesson
    // player - so it must hide the native chrome + skip the live candle load exactly
    // like lesson/studio. (Was lesson||studio only, back when event booted the bare
    // terminal + native control bar; that's the chrome we just replaced.)
    // Pack mode is embedded only when the host global says so (the /demo route's
    // React chrome); a ?pack= dev boot keeps the native transport.
    bool is_embedded() const {
        return mode_ == Mode::Lesson || mode_ == Mode::Studio || mode_ == Mode::Event ||
               (mode_ == Mode::Pack && pack_embedded_);
    }
    Mode mode() const { return mode_; }
    bool is_lesson() const { return mode_ == Mode::Lesson; }
    bool is_studio() const { return mode_ == Mode::Studio; }

    // Event = an archived market-event replay (public /events → /terminal?event=<id>).
    // Set from window.__EDGEDEPTH_EVENT__ = {sessionType:'archive', eventId, symbol,
    // seekToStart, startMs, endMs}. is_embedded() (above) is now TRUE for event, so
    // the React EventReplay chrome owns the shell + transport; EventRuntime mirrors
    // the archive session's transport state to it (CustomEvent 'edgedepth:event') and
    // applies its seek/pause/speed commands. The replay itself is auto-started by
    // main.cpp maybe_start_event_replay().
    bool is_event() const { return mode_ == Mode::Event; }
    const std::string& event_id() const { return event_id_; }
    const std::string& event_symbol() const { return event_symbol_; }
    bool event_seek_to_start() const { return event_seek_to_start_; }
    // The event window (epoch ms), passed from the web (the EventRecord carries
    // start_time/end_time) so EventRuntime reports a STABLE transport window from
    // frame one - the backend's archive window only arrives with the first status.
    int64_t event_start_ms() const { return event_start_ms_; }
    int64_t event_end_ms() const { return event_end_ms_; }
    // Deep-link timestamp (epoch ms; 0 = none) from ?event=…&t=… - a key-moment
    // link on the public event page (CLIP_FACTORY_PLAN Appendix A #5). Applied by
    // EventRuntime as ONE deliberate ReplayManager seek once the seeded book is
    // live (active && !loading), then never again - the user owns the transport.
    int64_t event_seek_to_ms() const { return event_seek_to_ms_; }

    // Pack = a CDN-served .edpack replay (Hot Replay Path B) - the box is out
    // of the loop entirely: no WebSocket, no session POST, no token. Set from
    // window.__EDGEDEPTH_PACK__ = {url, symbol, embedded} (the /demo route) or
    // the ?pack=<url>&packsym=<symbol> query params (standalone dev boots with
    // the native transport). The replay is auto-started by main.cpp
    // maybe_start_pack_replay() via ReplayManager::request_pack_replay.
    bool is_pack() const { return mode_ == Mode::Pack; }
    const std::string& pack_url() const { return pack_url_; }
    const std::string& pack_symbol() const { return pack_symbol_; }

    // Pack deep-link/start-at timestamp (epoch ms; 0 = none): pk.seekToMs from
    // __EDGEDEPTH_PACK__ (the /demo route bakes the catalog's startAtMs or the
    // ?t= deep link into it) or ?packt=<ms> on the standalone dev boot. Applied
    // by EventRuntime's one-shot deep-link path, same as event_seek_to_ms().
    int64_t pack_seek_to_ms() const { return pack_seek_to_ms_; }

    // Recorder script (CLIP_FACTORY P2) - ORTHOGONAL to the mode, not a mode:
    // the render harness boots a normal event/pack replay and ADDITIONALLY
    // injects window.__EDGEDEPTH_RECORDER__ = <RecorderScript> before the glue
    // loads. detect() stashes the stringified script here; main() feeds it to
    // edu::RecorderRuntime::load(), which then drives the session's transport.
    bool has_recorder_script() const { return !recorder_json_.empty(); }
    const std::string& recorder_json() const { return recorder_json_; }

    // The lesson doc URL the host asked us to load (empty if not embedded).
    const std::string& doc_url() const { return doc_url_; }

    // The resolved LessonDoc JSON (empty until the fetch completes).
    const std::string& lesson_json() const { return lesson_json_; }
    bool lesson_ready() const { return lesson_ready_; }

    // Studio mode: the symbol the picker chose (lowercase, e.g. "1000cheemsusdt"),
    // read from the studio global at detect() so the terminal can build its widgets
    // + dock layout for the RIGHT symbol at boot (the client has no in-place
    // symbol-switch - a different symbol means a fresh mount). Empty if not studio
    // or no symbol was provided.
    const std::string& studio_symbol() const { return studio_symbol_; }

    // Read window.__EDGEDEPTH_LESSON__ once. Safe to call when not embedded
    // (leaves mode_ == None). Call early in main(), before url_push.
    void detect();

    // Kick the credentialed fetch of doc_url_. on_ready fires on the main thread
    // when the LessonDoc JSON has arrived and been stashed.
    void fetch_lesson(std::function<void()> on_ready = nullptr);

    // Called from the JS XHR callback. Not for direct use.
    void on_fetch(const char* json_str, size_t len);

private:
    EducationBoot() = default;
    Mode mode_ = Mode::None;
    bool lesson_ready_ = false;
    std::string doc_url_;
    std::string lesson_json_;
    std::string studio_symbol_;
    std::string event_id_;
    std::string event_symbol_;
    std::string pack_url_;
    std::string pack_symbol_;
    std::string recorder_json_;  // stringified __EDGEDEPTH_RECORDER__ (or empty)
    bool pack_embedded_ = false;
    int64_t pack_seek_to_ms_ = 0;  // pack start-at / deep-link (0 = none)
    bool event_seek_to_start_ = true;
    int64_t event_start_ms_ = 0;
    int64_t event_end_ms_ = 0;
    int64_t event_seek_to_ms_ = 0;  // deep-link &t= (0 = none)
    std::function<void()> on_ready_;
};
