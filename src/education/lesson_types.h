#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// lesson_types.h — parsed LessonDoc model (mirrors lessonSchema.ts v0.2.0)
//
// A lesson is a replay WINDOW [startMs,endMs] + an ordered list of timed
// annotations. Steps are beat | track | quiz, anchored on ABSOLUTE timestamps
// (unix ms) — timeframe-independent and archive-count-independent. Authored
// order is irrelevant — the runtime sorts by time.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <cstdint>

namespace edu {

// What the spotlight frames. Mirrors schema "Target" oneOf. Times are unix ms.
struct Target {
    enum class Type { Region, Band, El, Full } type = Type::Full;
    // region: time span (+ optional price band; if !hasPrice, derive from candle hi/lo)
    int64_t t0 = 0, t1 = 0;
    double pTop = 0.0, pBot = 0.0;
    // explicit price bounds present? true when the author drew them (region OR band).
    // Distinguishes "author set pTop/pBot" from the region zero-sentinel / band `below`
    // fallback — so a legitimately-authored band at price 0 is impossible anyway, but
    // we don't rely on a magic zero. When false: region derives from candle hi/lo, band
    // uses `below`.
    bool hasPrice = false;
    // band: frame a liquidation shelf — `below` price-points under the candle AT time t
    // (LEGACY fallback used only when !hasPrice; authored bands set pTop/pBot + hasPrice)
    int64_t t = 0;
    int below = 0;
    // el: a docked panel by CSS-ish id ("#cDomGrid", "#cIndi", ...)
    std::string el;
    // follow (track only): region with no explicit bounds = grow t0..current playhead
    bool follow = false;
};

enum class Kind : uint8_t { Beat, Track, Quiz };

struct QuizOption {
    std::string text;
    bool correct = false;
    std::string why;
};

struct Step {
    Kind kind = Kind::Beat;
    std::string title;
    std::string kicker;
    std::string body;
    std::string indicator;        // "oi"|"cvd"|"vol"|"funding"|"" — switch bottom pane
    Target target;

    // beat/quiz: single moment t. track: window [t0,t1]. All unix ms.
    int64_t t = 0;
    int64_t t0 = 0, t1 = 0;
    bool pause = true;            // beat: auto-pause
    float slowTo = 0.0f;          // track: optional reduced playback rate (0 = no change)
    // While this step is ACTIVE (card up for beat/quiz, window running for a
    // track), forward seeks clamp to the playhead and the ghost scrub preview
    // stops there too. Quizzes default locked (scrubbing ahead of a prediction
    // check reads the answer off the chart); exercise steps set it explicitly.
    bool lockForward = false;

    // quiz
    std::vector<QuizOption> options;

    // derived
    int n = 0;                    // 1-based step number
};

struct Source {
    std::string symbol;
    std::string venue;
    std::string event;
    int64_t startMs = 0;          // replay window start (unix ms)
    int64_t endMs = 0;            // replay window end (unix ms)
    std::string tf;
};

struct Lesson {
    std::string title;
    std::string course;
    std::string chapter;
    std::string description;      // short rail blurb (Image 2). Optional.
    Source source;
    std::vector<Step> steps;      // sorted by time
    bool valid = false;
};

} // namespace edu
