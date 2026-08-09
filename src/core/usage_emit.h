#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// usage_emit.h - C++→JS product-usage emit (design §2/§3).
//
// Mirrors education/transport_emit.h's dispatch mechanic, but for PRODUCT usage
// rather than transport state: build a JSON `detail` object and dispatch a window
// CustomEvent 'edgedepth:usage'. The web usageBridge listens for it, attaches
// {user (server-derived), anonId, sessionId}, and fans out to Umami (aggregate)
// + POST /api/usage (per-user). C++ never touches window.umami directly - one
// enrichment point (design §8).
//
// Detail shape the bridge understands:
//   { event, mode?, symbol?, eventRef?, dedupeKey?, umami?, heartbeatFor?, props? }
//   - `dedupeKey` : client idempotency key (unique index) so retries never double-count.
//   - `umami`     : also fire the Umami aggregate event (default true).
//   - `heartbeatFor`: present ONLY on '*_heartbeat' events. The bridge does NOT
//     row a heartbeat; it holds it as the PENDING final event named here (keyed by
//     dedupeKey) and materializes it on tab-close, so watch time survives an unload
//     with no clean end. A later clean end with the same dedupeKey dedupes it.
//
// Fire-and-forget; no-op off-Emscripten. Reads are one-shot boot enrichment only.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace usage {

// Dispatch a fully-built detail JSON string as CustomEvent('edgedepth:usage').
inline void dispatch_detail(const std::string& detail_json) {
#ifdef __EMSCRIPTEN__
    EM_ASM(
        {
            try {
                var d = JSON.parse(UTF8ToString($0));
                window.dispatchEvent(new CustomEvent('edgedepth:usage', { detail: d }));
            } catch (e) {
                console.warn('usage dispatch: bad payload', e);
            }
        },
        detail_json.c_str());
#else
    (void)detail_json;
#endif
}

// One-shot read of a window string global for boot enrichment (plan, referrer).
// `global_name` is a bare identifier on window, e.g. "__EDGEDEPTH_TIER__". Returns
// "" when unset. Not for hot paths.
inline std::string read_window_string(const char* global_name) {
#ifdef __EMSCRIPTEN__
    char* p = (char*)EM_ASM_PTR(
        {
            try {
                var v = window[UTF8ToString($0)];
                if (v === undefined || v === null) return 0;
                v = String(v);
                var len = lengthBytesUTF8(v) + 1;
                var buf = _malloc(len);
                stringToUTF8(v, buf, len);
                return buf;
            } catch (e) {
                return 0;
            }
        },
        global_name);
    if (!p) return "";
    std::string s(p);
    free(p);
    return s;
#else
    (void)global_name;
    return "";
#endif
}

// document.referrer (or "") for session attribution.
inline std::string read_referrer() {
#ifdef __EMSCRIPTEN__
    char* p = (char*)EM_ASM_PTR({
        try {
            var v = String(document.referrer);
            var len = lengthBytesUTF8(v) + 1;
            var buf = _malloc(len);
            stringToUTF8(v, buf, len);
            return buf;
        } catch (e) { return 0; }
    });
    if (!p) return "";
    std::string s(p);
    free(p);
    return s;
#else
    return "";
#endif
}

// document.visibilityState === 'visible'. Cheap; safe to poll per frame while a
// replay is active (the rAF loop is throttled when hidden anyway).
inline bool is_document_visible() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return (document.visibilityState === 'visible') ? 1 : 0; }) != 0;
#else
    return true;
#endif
}

}  // namespace usage
