// ═══════════════════════════════════════════════════════════════════════════════
// recorder_glue.cpp - P1 "Share clip" recorder (see recorder_glue.h)
//
// ONE file owns every line of JS (EM_JS below) + the C++ state machine + the
// burned-in watermark. Conventions:
//   · JS state lives in Module['__edclip'] (module scope survives between calls)
//   · JS→C++ is ONLY _recorder_on_state(int,double) (EXPORTED_FUNCTIONS:
//     __recorder_on_state) - the transport button renders whatever the browser
//     reports, never what C++ hopes happened
//   · capture source is Module['canvas'] - the SAME lookup works bare
//     (serve_threaded.py) and embedded (/terminal in the web app); never
//     getElementById (the embed owns the element's id/placement).
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/recorder_glue.h"
#include "rendering/theme.h"
#include "rendering/layout.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES3/gl3.h>   // cam-bubble texture (export mode) - repo GL header
#endif

namespace {

ClipRecorder::State g_state     = ClipRecorder::State::Unsupported;  // until probe
bool                g_supported = false;
double              g_bytes     = 0.0;
double              g_start_now = 0.0;  // emscripten_get_now() at start (monotonic)
double              g_error_at  = 0.0;  // when Error was reported (for decay)
// Watermark line, built ONCE at start() - zero string work in the render loop.
char                g_badge[96] = {0};

// Focus layout (see header). Session-static default ON.
bool    g_focus_enabled = true;
// Deferred-start countdown: ⏺ sets 2, tick_and_render decrements once per frame
// and fires the JS start at 0. Two frames because the click is processed AFTER
// the current frame's chrome already rendered - frame N+1 is the first one laid
// out chrome-free, so capture must not begin before its composite.
int     g_start_pending = 0;
char    g_pend_sym[24]  = {0};
int64_t g_pend_ms       = 0;

constexpr double kErrorDecayMs = 4000.0;

// ── Export mode (CLIP_FACTORY P3-v1) ─────────────────────────────────────────
// True from export_start() until the recorder settles back to Idle/Error. While
// set, the P1 tick_and_render() is a no-op - export_tick_and_render() owns the
// cap/badge/cam-bubble for this mode. Shared g_state/g_bytes/g_start_now.
bool         g_export_mode      = false;
bool         g_export_has_audio = false;
unsigned int g_cam_tex          = 0;   // GL texture for the cam bubble (lazy, reused)

}  // namespace

#ifdef __EMSCRIPTEN__

// ═══════════════════════════════════════════════════════════════════════════════
// JS glue - ALL of it, EM_JS only (no --js-library)
// ═══════════════════════════════════════════════════════════════════════════════

// Probe once at boot. Negotiates the container/codec (vp9 → vp8 → webm → mp4),
// stashes the pick in Module['__edclip'], returns 0 when recording can't work
// (the button renders disabled). Returns 1+index of the pick for the boot log.
EM_JS(int, edclip_js_probe, (), {
    try {
        var canvas = Module['canvas'];
        if (typeof MediaRecorder === 'undefined' || !canvas ||
            typeof canvas.captureStream !== 'function' ||
            typeof MediaRecorder.isTypeSupported !== 'function') return 0;
        var picks = [
            ['video/webm;codecs=vp9', 'webm'],
            ['video/webm;codecs=vp8', 'webm'],
            ['video/webm',            'webm'],
            ['video/mp4',             'mp4']
        ];
        var st = Module['__edclip'] = {
            mime: null, ext: null, rec: null, stream: null,
            chunks: [], bytes: 0, fname: "", timer: 0
        };
        for (var i = 0; i < picks.length; i++) {
            if (MediaRecorder.isTypeSupported(picks[i][0])) {
                st.mime = picks[i][0];
                st.ext  = picks[i][1];
                return i + 1;
            }
        }
        return 0;
    } catch (e) {
        return 0;
    }
});

// Start: capture Module['canvas'] at 30fps with a ~1s timeslice. 30, not 60:
// each captured frame costs a full-backing-store GPU readback + a software VP9
// encode, and at 60 that dragged the render loop from 170 to ~35 FPS (2026-07-05
// regression). 30fps halves both costs, social clips are 30fps anyway, and
// quality/frame is unchanged because the bitrate formula scales with fps.
// Bitrate is explicit - the ~2.5Mbps MediaRecorder default smears candles/text.
// We ask for ~0.1 bits/pixel/frame of the BACKING store (hiDPI canvases carry
// the full backing resolution, which is exactly what captureStream records),
// clamped to 8-20 Mbps. Filename = edgedepth_{symbol}_{date}.{ext} - date_str is
// derived from the REPLAY DATA position (UTC), never wall clock; ext is the
// negotiated container from the probe, never hardcoded.
EM_JS(int, edclip_js_start, (const char* symbol_lower, const char* date_str), {
    try {
        var st = Module['__edclip'];
        if (!st || !st.mime || st.rec) return 0;
        var canvas = Module['canvas'];
        var fps = 30;  // see header comment - 60 tanked render FPS (readback+encode)
        var bps = canvas.width * canvas.height * 0.1 * fps;
        bps = Math.max(8e6, Math.min(20e6, bps));
        var stream = canvas.captureStream(fps);
        var rec = new MediaRecorder(stream, {
            mimeType: st.mime,
            videoBitsPerSecond: bps
        });
        st.rec = rec;
        st.stream = stream;
        st.chunks = [];
        st.bytes = 0;
        st.fname = 'edgedepth_' + UTF8ToString(symbol_lower) + '_' +
                   UTF8ToString(date_str) + '.' + st.ext;
        rec.ondataavailable = function(e) {
            if (e.data && e.data.size > 0) {
                st.chunks.push(e.data);
                st.bytes += e.data.size;
            }
            // Guard: a late chunk after cleanup must not resurrect REC state.
            if (st.rec === rec) __recorder_on_state(1, st.bytes);
        };
        rec.onerror = function(e) {
            console.warn('[ClipRecorder] MediaRecorder error', e);
            try { stream.getTracks().forEach(function(t) { t.stop(); }); } catch (_) {}
            if (st.timer) { clearTimeout(st.timer); st.timer = 0; }
            st.rec = null; st.chunks = []; st.bytes = 0;
            __recorder_on_state(3, 0);
        };
        rec.onstop = function() {
            try {
                if (st.timer) { clearTimeout(st.timer); st.timer = 0; }
                __recorder_on_state(2, st.bytes);
                var blob = new Blob(st.chunks, { type: st.mime.split(';')[0] });
                var url = URL.createObjectURL(blob);
                var a = document.createElement('a');
                a.href = url;
                a.download = st.fname;
                document.body.appendChild(a);
                a.click();
                a.remove();
                setTimeout(function() { URL.revokeObjectURL(url); }, 5000);
                try { stream.getTracks().forEach(function(t) { t.stop(); }); } catch (_) {}
                st.rec = null; st.chunks = []; st.bytes = 0;
                __recorder_on_state(0, 0);
            } catch (e) {
                console.warn('[ClipRecorder] download failed', e);
                st.rec = null; st.chunks = []; st.bytes = 0;
                __recorder_on_state(3, 0);
            }
        };
        rec.start(1000);  // ~1s timeslice → steady byte counter, bounded chunks
        // Belt-and-braces: the C++ 3:00 timer is authoritative; this fires only
        // if the wasm side stalls (hidden-tab rAF throttling etc.). +5s grace.
        st.timer = setTimeout(function() {
            if (st.rec === rec && rec.state !== 'inactive') rec.stop();
        }, 185000);
        __recorder_on_state(1, 0);
        return 1;
    } catch (e) {
        console.warn('[ClipRecorder] start failed', e);
        __recorder_on_state(3, 0);
        return 0;
    }
});

// Stop → onstop → identical blob/download path for user stop AND the 3:00 cap.
EM_JS(void, edclip_js_stop, (), {
    try {
        var st = Module['__edclip'];
        if (!st || !st.rec) return;
        if (st.timer) { clearTimeout(st.timer); st.timer = 0; }
        if (st.rec.state !== 'inactive') st.rec.stop();
    } catch (e) {
        console.warn('[ClipRecorder] stop failed', e);
        __recorder_on_state(3, 0);
    }
});

// ═══════════════════════════════════════════════════════════════════════════════
// Export mode JS (CLIP_FACTORY P3-v1) - same Module['__edclip'] slot, second
// entry point. Probes its OWN container ladder (audio-aware) per export; the P1
// boot probe/pick (st.mime/st.ext) is never touched.
// ═══════════════════════════════════════════════════════════════════════════════

// Begin the export capture. Returns 0 = can't record, 1 = video-only, 2 = A/V.
//
// Reads window.__EDGEDEPTH_EXPORT_MEDIA__ = { ctx: AudioContext, stream:
// MediaStream } (stashed by StudioShell inside the Export click - the
// AudioContext is constructed synchronously in the user gesture so it boots
// 'running'; getUserMedia already carries the permission). Absent/denied media
// ⇒ silent video export. The narration mic routes through a WebAudio graph -
// MediaStreamSource → MediaStreamAudioDestinationNode → addTrack() onto the
// canvas captureStream - so a stored narration track can later replace the mic
// by swapping the source node ONLY (P3-v2/P2 reuse this path unchanged).
//
// The cam track (if any) feeds a detached muted <video> (st.exCam) that
// edclip_js_cam_upload() samples into a GL texture each frame - the bubble is
// composited IN-RENDER because captureStream sees only the canvas.
EM_JS(int, edclip_js_export_begin, (const char* slug, const char* date_str), {
    try {
        var canvas = Module['canvas'];
        if (typeof MediaRecorder === 'undefined' || !canvas ||
            typeof canvas.captureStream !== 'function' ||
            typeof MediaRecorder.isTypeSupported !== 'function') return 0;
        // Boot probe may have bailed before creating the slot (unsupported for
        // P1 means unsupported here too, but keep the slot logic self-sufficient).
        var st = Module['__edclip'];
        if (!st) {
            st = Module['__edclip'] = {
                mime: null, ext: null, rec: null, stream: null,
                chunks: [], bytes: 0, fname: "", timer: 0
            };
        }
        if (st.rec) return 0;  // P1 clip or another export already running

        var media = window.__EDGEDEPTH_EXPORT_MEDIA__ || null;
        var mediaStream = media && media.stream ? media.stream : null;
        var audioTracks = mediaStream ? mediaStream.getAudioTracks() : [];
        var videoTracks = mediaStream ? mediaStream.getVideoTracks() : [];
        var wantAudio = audioTracks.length > 0;

        // Container ladder - WITH audio codecs when a mic track is present
        // (mimeType must name both codecs or Chrome records silent video).
        var picks = wantAudio
            ? [['video/webm;codecs=vp9,opus', 'webm'],
               ['video/webm;codecs=vp8,opus', 'webm'],
               ['video/webm',                 'webm'],
               ['video/mp4',                  'mp4']]
            : [['video/webm;codecs=vp9', 'webm'],
               ['video/webm;codecs=vp8', 'webm'],
               ['video/webm',            'webm'],
               ['video/mp4',             'mp4']];
        var mime = null, ext = null;
        for (var i = 0; i < picks.length; i++) {
            if (MediaRecorder.isTypeSupported(picks[i][0])) {
                mime = picks[i][0]; ext = picks[i][1]; break;
            }
        }
        if (!mime) return 0;

        var fps = 30;  // P1 lesson: 60 dragged the render loop 170→~35 FPS
        var bps = canvas.width * canvas.height * 0.1 * fps;
        bps = Math.max(8e6, Math.min(20e6, bps));

        var stream = canvas.captureStream(fps);

        // Narration: mic → WebAudio graph → destination-node track → capture.
        var gotAudio = false;
        if (wantAudio && media.ctx) {
            try {
                if (media.ctx.state === 'suspended') media.ctx.resume();
                var srcNode  = media.ctx.createMediaStreamSource(mediaStream);
                var destNode = media.ctx.createMediaStreamDestination();
                srcNode.connect(destNode);
                var at = destNode.stream.getAudioTracks();
                if (at.length > 0) { stream.addTrack(at[0]); gotAudio = true; }
                st.exAudioSrc = srcNode;   // keep the graph alive for the take
                st.exAudioDst = destNode;
            } catch (ae) {
                console.warn('[ClipRecorder] audio graph failed - exporting silent', ae);
            }
        }

        // Cam: detached muted <video> the per-frame GL upload samples.
        if (videoTracks.length > 0) {
            var v = document.createElement('video');
            v.muted = true;
            v.playsInline = true;
            v.srcObject = new MediaStream([videoTracks[0]]);
            var p = v.play();
            if (p && p.catch) p.catch(function(e) {
                console.warn('[ClipRecorder] cam video play() rejected', e);
            });
            st.exCam = v;
        }

        var opts = { mimeType: mime, videoBitsPerSecond: bps };
        if (gotAudio) opts.audioBitsPerSecond = 128000;
        var rec = new MediaRecorder(stream, opts);
        st.rec = rec;
        st.stream = stream;
        st.chunks = [];
        st.bytes = 0;
        st.export = 1;
        st.fname = 'edgedepth_' + UTF8ToString(slug) + '_' +
                   UTF8ToString(date_str) + '.' + ext;

        var cleanupMedia = function() {
            try { stream.getTracks().forEach(function(t) { t.stop(); }); } catch (_) {}
            try {
                if (mediaStream) mediaStream.getTracks().forEach(function(t) { t.stop(); });
            } catch (_) {}
            try { if (media && media.ctx) media.ctx.close(); } catch (_) {}
            try { delete window.__EDGEDEPTH_EXPORT_MEDIA__; } catch (_) {}
            st.exCam = null; st.exAudioSrc = null; st.exAudioDst = null;
            st.export = 0;
        };

        rec.ondataavailable = function(e) {
            if (e.data && e.data.size > 0) {
                st.chunks.push(e.data);
                st.bytes += e.data.size;
            }
            if (st.rec === rec) __recorder_on_state(1, st.bytes);
        };
        rec.onerror = function(e) {
            console.warn('[ClipRecorder] export MediaRecorder error', e);
            if (st.timer) { clearTimeout(st.timer); st.timer = 0; }
            cleanupMedia();
            st.rec = null; st.chunks = []; st.bytes = 0;
            __recorder_on_state(3, 0);
        };
        rec.onstop = function() {
            try {
                if (st.timer) { clearTimeout(st.timer); st.timer = 0; }
                __recorder_on_state(2, st.bytes);
                var blob = new Blob(st.chunks, { type: mime.split(';')[0] });
                var url = URL.createObjectURL(blob);
                var a = document.createElement('a');
                a.href = url;
                a.download = st.fname;
                document.body.appendChild(a);
                a.click();
                a.remove();
                setTimeout(function() { URL.revokeObjectURL(url); }, 5000);
                cleanupMedia();
                st.rec = null; st.chunks = []; st.bytes = 0;
                __recorder_on_state(0, 0);
            } catch (e) {
                console.warn('[ClipRecorder] export download failed', e);
                cleanupMedia();
                st.rec = null; st.chunks = []; st.bytes = 0;
                __recorder_on_state(3, 0);
            }
        };
        rec.start(1000);
        // Belt-and-braces past the 15:00 C++ cap (hidden-tab rAF stall etc.).
        st.timer = setTimeout(function() {
            if (st.rec === rec && rec.state !== 'inactive') rec.stop();
        }, 905000);
        __recorder_on_state(1, 0);
        return gotAudio ? 2 : 1;
    } catch (e) {
        console.warn('[ClipRecorder] export start failed', e);
        __recorder_on_state(3, 0);
        return 0;
    }
});

// Release an UNCONSUMED media stash (export cancelled before capture start).
// While an export capture runs, its own cleanup owns the media - skip.
EM_JS(void, edclip_js_export_release_media, (), {
    try {
        var st = Module['__edclip'];
        if (st && st.rec && st.export) return;   // capture owns it
        var media = window.__EDGEDEPTH_EXPORT_MEDIA__;
        if (!media) return;
        try {
            if (media.stream) media.stream.getTracks().forEach(function(t) { t.stop(); });
        } catch (_) {}
        try { if (media.ctx) media.ctx.close(); } catch (_) {}
        delete window.__EDGEDEPTH_EXPORT_MEDIA__;
    } catch (_) {}
});

// Cam frame geometry: (videoWidth<<16)|videoHeight once decodable, else 0.
EM_JS(int, edclip_js_cam_dims, (), {
    var st = Module['__edclip'];
    var v = st && st.exCam;
    if (!v || v.readyState < 2 || !v.videoWidth || !v.videoHeight) return 0;
    return ((v.videoWidth & 0xffff) << 16) | (v.videoHeight & 0xffff);
});

// Upload the current cam frame into the GL texture `tex` (a C++-side
// glGenTextures handle → GL.textures[tex] in Emscripten's GL layer). Runs on
// the main thread between NewFrame and Render - no draw is in flight, so the
// transient TEXTURE_2D binding can't corrupt anyone (every renderer in this
// codebase re-binds before drawing). Returns 1 on upload.
EM_JS(int, edclip_js_cam_upload, (int tex), {
    try {
        var st = Module['__edclip'];
        var v = st && st.exCam;
        if (!v || v.readyState < 2 || !v.videoWidth) return 0;
        var glTex = GL.textures[tex];
        if (!glTex) return 0;
        GLctx.bindTexture(GLctx.TEXTURE_2D, glTex);
        GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA,
                         GLctx.UNSIGNED_BYTE, v);
        return 1;
    } catch (e) {
        return 0;
    }
});

// ═══════════════════════════════════════════════════════════════════════════════
// JS→C++ - the ONE exported callback (CMakeLists: __recorder_on_state)
// ═══════════════════════════════════════════════════════════════════════════════

extern "C" EMSCRIPTEN_KEEPALIVE void _recorder_on_state(int state, double bytes) {
    g_bytes = bytes;
    switch (state) {
        case 0: g_state = ClipRecorder::State::Idle;      break;
        case 1: g_state = ClipRecorder::State::Recording; break;
        case 2: g_state = ClipRecorder::State::Stopped;   break;
        case 3:
            g_state    = ClipRecorder::State::Error;
            g_error_at = emscripten_get_now();
            break;
        default: break;  // 4/unknown never sent by the glue
    }
}

#endif  // __EMSCRIPTEN__

// ═══════════════════════════════════════════════════════════════════════════════
// C++ API
// ═══════════════════════════════════════════════════════════════════════════════

namespace ClipRecorder {

void probe_support() {
#ifdef __EMSCRIPTEN__
    const int pick = edclip_js_probe();
    g_supported = pick > 0;
    g_state = g_supported ? State::Idle : State::Unsupported;
#else
    g_supported = false;
    g_state = State::Unsupported;
#endif
}

bool  supported()    { return g_supported; }
State state()        { return g_state; }
bool  is_recording() { return g_state == State::Recording; }
double bytes()       { return g_bytes; }

bool focus_enabled() { return g_focus_enabled; }
void set_focus_enabled(bool on) {
    // Locked while recording (or a start is pending) - no mid-clip layout jump.
    if (g_state == State::Recording || g_start_pending > 0) return;
    g_focus_enabled = on;
}
bool focus_active() {
    return g_focus_enabled && (g_start_pending > 0 || g_state == State::Recording);
}

double elapsed_ms() {
#ifdef __EMSCRIPTEN__
    return g_state == State::Recording ? emscripten_get_now() - g_start_now : 0.0;
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
// The actual JS start - filename/watermark build + edclip_js_start. Split from
// start() so the focus path can defer it behind the chrome-hide (see header).
static void do_start_js(const char* symbol_upper, int64_t replay_data_ms) {
    // Filename symbol (lowercase) + REPLAY-DATA timestamps (UTC). Wall clock
    // never appears anywhere - the filename matches the burned watermark date.
    char sym_lower[24];
    size_t n = 0;
    for (; symbol_upper[n] != '\0' && n < sizeof(sym_lower) - 1; ++n) {
        const char c = symbol_upper[n];
        sym_lower[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    sym_lower[n] = '\0';

    const time_t s = static_cast<time_t>(replay_data_ms / 1000);
    struct tm tmv;
    gmtime_r(&s, &tmv);

    char date_str[20];
    snprintf(date_str, sizeof(date_str), "%04d%02d%02d_%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);

    // Watermark line, built once (no per-frame string work).
    snprintf(g_badge, sizeof(g_badge),
             "EDGEDEPTH \xc2\xb7 %s \xc2\xb7 REPLAY %04d-%02d-%02d",
             symbol_upper, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

    if (edclip_js_start(sym_lower, date_str)) {
        // The synchronous __recorder_on_state(1,0) inside the glue already set
        // Recording - here we only anchor the monotonic elapsed/cap base.
        g_start_now = emscripten_get_now();
    }
}
#endif  // __EMSCRIPTEN__

void start(const char* symbol_upper, int64_t replay_data_ms) {
#ifdef __EMSCRIPTEN__
    if (!g_supported) return;
    if (g_state == State::Recording || g_state == State::Stopped) return;
    if (!symbol_upper || !symbol_upper[0] || replay_data_ms <= 0) return;

    if (g_focus_enabled) {
        // Defer: hide the chrome first (focus_active() flips this instant), start
        // capturing two frames later when a clean composite exists.
        size_t n = 0;
        for (; symbol_upper[n] != '\0' && n < sizeof(g_pend_sym) - 1; ++n)
            g_pend_sym[n] = symbol_upper[n];
        g_pend_sym[n]   = '\0';
        g_pend_ms       = replay_data_ms;
        g_start_pending = 2;
    } else {
        do_start_js(symbol_upper, replay_data_ms);
    }
#else
    (void)symbol_upper;
    (void)replay_data_ms;
#endif
}

void stop() {
#ifdef __EMSCRIPTEN__
    if (g_state == State::Recording) edclip_js_stop();
#endif
}

void tick_and_render(bool replay_active) {
#ifdef __EMSCRIPTEN__
    // Export mode owns its own tick (export_tick_and_render) - cap, badge and
    // stop rules differ. Bail so the P1 path can't double-render or 3:00-cap a
    // 15:00 export. P1 behavior outside export mode is byte-identical.
    if (g_export_mode) return;
    // Deferred ⏺ (focus path): count down, then fire the JS start against the
    // first chrome-free composite. Cancel if the session died in the window.
    if (g_start_pending > 0) {
        if (!replay_active || !g_supported ||
            g_state == State::Recording || g_state == State::Stopped) {
            g_start_pending = 0;
        } else if (--g_start_pending == 0) {
            do_start_js(g_pend_sym, g_pend_ms);
        }
    }
    // Error badge decays back to Idle so the button recovers without a reload.
    if (g_state == State::Error &&
        emscripten_get_now() - g_error_at > kErrorDecayMs) {
        g_state = State::Idle;
    }
    if (g_state != State::Recording) return;

    const double elapsed = emscripten_get_now() - g_start_now;

    // AUTHORITATIVE 3:00 cap - takes the identical stop/download path as ⏹.
    if (elapsed >= kMaxClipMs) {
        stop();
        return;
    }
    // Replay session ended/errored under a live recording → finish the clip.
    if (!replay_active) {
        stop();
        return;
    }

    // ── Watermark badge - FOREGROUND draw list: always on top of every widget,
    //    so it's composited into canvas.captureStream frames (the whole point).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    const int total_s = static_cast<int>(elapsed / 1000.0);
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", total_s / 60, total_s % 60);

    ImFont* f_rec   = Theme::Fonts::label();        // "REC" micro-label
    ImFont* f_clock = Theme::Fonts::mono();         // mm:ss numerics
    ImFont* f_line  = Theme::Fonts::ui_semibold();  // EDGEDEPTH · SYMBOL · DATE

    // Measure with each font pushed (repo pattern - ImFont::FontSize is gone in
    // this ImGui; CalcTextSize + the font-less AddText read the pushed font).
    ImGui::PushFont(f_rec);
    const ImVec2 rec_sz = ImGui::CalcTextSize("REC");
    ImGui::PopFont();
    ImGui::PushFont(f_clock);
    const ImVec2 clk_sz = ImGui::CalcTextSize(clock);
    ImGui::PopFont();
    ImGui::PushFont(f_line);
    const ImVec2 line_sz = ImGui::CalcTextSize(g_badge);
    ImGui::PopFont();

    const float pad_x = 10.0f, pad_y = 6.0f;
    const float dot_r = 3.5f, gap = 7.0f, sep_gap = 9.0f;
    float inner_h = rec_sz.y;
    if (clk_sz.y  > inner_h) inner_h = clk_sz.y;
    if (line_sz.y > inner_h) inner_h = line_sz.y;

    const float w = pad_x + dot_r * 2.0f + gap + rec_sz.x + gap + clk_sz.x +
                    sep_gap + 1.0f + sep_gap + line_sz.x + pad_x;
    const float h = pad_y * 2.0f + inner_h;

    // Top-right, tucked under the app-shell chrome (top_reserve is 0 when the
    // shell is off, so bare + embedded placements both come out right).
    const float x = io.DisplaySize.x - w - 12.0f;
    const float y = LayoutManager::top_reserve + 10.0f;

    const ImVec2 p0(x, y), p1(x + w, y + h);
    dl->AddRectFilled(p0, p1, Theme::u32(Theme::Tokens::BASE, 0.88f), Theme::Radius::R3);
    dl->AddRect(p0, p1, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R3, 0, 1.0f);

    const float cy = y + h * 0.5f;
    float cx = x + pad_x + dot_r;

    // REC dot - classic gentle blink; floor keeps it legible in every frame.
    const float pulse = 0.35f + 0.65f *
        (0.5f + 0.5f * sinf(static_cast<float>(ImGui::GetTime()) * 4.0f));
    dl->AddCircleFilled(ImVec2(cx, cy), dot_r, Theme::u32(Theme::Tokens::DOWN, pulse));
    cx += dot_r + gap;

    ImGui::PushFont(f_rec);
    dl->AddText(ImVec2(cx, cy - rec_sz.y * 0.5f),
                Theme::u32(Theme::Tokens::DOWN), "REC");
    ImGui::PopFont();
    cx += rec_sz.x + gap;

    ImGui::PushFont(f_clock);
    dl->AddText(ImVec2(cx, cy - clk_sz.y * 0.5f),
                Theme::u32(Theme::Tokens::TX1), clock);
    ImGui::PopFont();
    cx += clk_sz.x + sep_gap;

    dl->AddLine(ImVec2(cx, y + pad_y), ImVec2(cx, y + h - pad_y),
                Theme::u32(Theme::Tokens::BD2), 1.0f);
    cx += 1.0f + sep_gap;

    ImGui::PushFont(f_line);
    dl->AddText(ImVec2(cx, cy - line_sz.y * 0.5f),
                Theme::u32(Theme::Tokens::TX1, 0.95f), g_badge);
    ImGui::PopFont();
#else
    (void)replay_active;
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
// Export mode (CLIP_FACTORY P3-v1) - see recorder_glue.h
// ═══════════════════════════════════════════════════════════════════════════════

bool export_active() { return g_export_mode; }

double export_ms() {
#ifdef __EMSCRIPTEN__
    return (g_export_mode && g_state == State::Recording)
               ? emscripten_get_now() - g_start_now : 0.0;
#else
    return 0.0;
#endif
}

bool export_has_audio() { return g_export_mode && g_export_has_audio; }

void export_start(const char* slug, const char* symbol_upper, int64_t lesson_start_ms) {
#ifdef __EMSCRIPTEN__
    if (g_export_mode) return;                       // one export at a time
    if (g_state == State::Recording || g_state == State::Stopped) return;  // P1 clip live
    if (g_start_pending > 0) return;                 // P1 deferred start in flight
    if (!slug || !symbol_upper || !symbol_upper[0] || lesson_start_ms <= 0) return;

    // Sanitize the slug for the filename: [a-z0-9-], collapse the rest to '-'.
    char slug_clean[48];
    size_t o = 0;
    bool last_dash = true;  // swallow leading dashes
    for (size_t i = 0; slug[i] != '\0' && o < sizeof(slug_clean) - 1; ++i) {
        char c = slug[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) { slug_clean[o++] = c; last_dash = false; }
        else if (!last_dash) { slug_clean[o++] = '-'; last_dash = true; }
    }
    while (o > 0 && slug_clean[o - 1] == '-') --o;   // trim trailing dash
    if (o == 0) { slug_clean[o++] = 'l'; slug_clean[o++] = 'e'; slug_clean[o++] = 's';
                  slug_clean[o++] = 's'; slug_clean[o++] = 'o'; slug_clean[o++] = 'n'; }
    slug_clean[o] = '\0';

    // Filename timestamp + badge date = the lesson window START (replay DATA
    // date, UTC) - the P1 rule, wall clock never appears anywhere.
    const time_t s = static_cast<time_t>(lesson_start_ms / 1000);
    struct tm tmv;
    gmtime_r(&s, &tmv);

    char date_str[20];
    snprintf(date_str, sizeof(date_str), "%04d%02d%02d_%02d%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);

    snprintf(g_badge, sizeof(g_badge),
             "EDGEDEPTH \xc2\xb7 %s \xc2\xb7 REPLAY %04d-%02d-%02d",
             symbol_upper, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

    const int r = edclip_js_export_begin(slug_clean, date_str);
    if (r > 0) {
        g_export_mode      = true;
        g_export_has_audio = (r == 2);
        g_start_now        = emscripten_get_now();
    }
#else
    (void)slug; (void)symbol_upper; (void)lesson_start_ms;
#endif
}

void export_stop() {
#ifdef __EMSCRIPTEN__
    if (g_export_mode && g_state == State::Recording) edclip_js_stop();
#endif
}

void export_release_media() {
#ifdef __EMSCRIPTEN__
    edclip_js_export_release_media();
#endif
}

void export_tick_and_render(bool session_alive) {
#ifdef __EMSCRIPTEN__
    if (!g_export_mode) return;

    // Error decays back to Idle exactly like P1; either terminal state ends
    // export mode (onstop/onerror already cleaned the JS side up).
    if (g_state == State::Error &&
        emscripten_get_now() - g_error_at > kErrorDecayMs) {
        g_state = State::Idle;
    }
    if (g_state == State::Idle || g_state == State::Error) {
        if (g_state == State::Idle) g_export_mode = false;
        if (g_state == State::Error) { /* hold mode until decay so UI shows it */ }
        if (!g_export_mode) g_export_has_audio = false;
        return;
    }
    if (g_state != State::Recording) return;  // Stopped = saving; badge off

    const double elapsed = emscripten_get_now() - g_start_now;

    // AUTHORITATIVE 15:00 ceiling - identical stop/download path as ⏹/P1.
    if (elapsed >= kMaxExportMs) { export_stop(); return; }
    // Replay session died under the export → finish + download what we have.
    if (!session_alive) { export_stop(); return; }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    // ── Badge - the EDGEDEPTH line only. No REC dot, no elapsed timer: this is
    //    a produced video, not a live share-clip (P3-v1 policy decision).
    ImFont* f_line = Theme::Fonts::ui_semibold();
    ImGui::PushFont(f_line);
    const ImVec2 line_sz = ImGui::CalcTextSize(g_badge);
    ImGui::PopFont();

    const float pad_x = 10.0f, pad_y = 6.0f;
    const float w = pad_x + line_sz.x + pad_x;
    const float h = pad_y * 2.0f + line_sz.y;
    // Embedded studio has no app shell → top_reserve is 0; same placement rule
    // as P1 keeps bare/embedded consistent anyway.
    const float x = io.DisplaySize.x - w - 12.0f;
    const float y = LayoutManager::top_reserve + 10.0f;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      Theme::u32(Theme::Tokens::BASE, 0.88f), Theme::Radius::R3);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                Theme::u32(Theme::Tokens::BD2), Theme::Radius::R3, 0, 1.0f);
    ImGui::PushFont(f_line);
    dl->AddText(ImVec2(x + pad_x, y + pad_y),
                Theme::u32(Theme::Tokens::TX1, 0.95f), g_badge);
    ImGui::PopFont();

    // ── Cam bubble - bottom-right circle, composited IN-RENDER so the capture
    //    sees it. Per-frame video→GL upload (tiny vs the full-canvas readback
    //    the capture itself already does at 30fps).
    const int dims = edclip_js_cam_dims();
    if (dims != 0) {
        if (g_cam_tex == 0) {
            glGenTextures(1, &g_cam_tex);
            glBindTexture(GL_TEXTURE_2D, g_cam_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (edclip_js_cam_upload(static_cast<int>(g_cam_tex))) {
            const int vw = (dims >> 16) & 0xffff;
            const int vh = dims & 0xffff;

            float d = std::min(io.DisplaySize.x, io.DisplaySize.y) * 0.20f;
            d = std::clamp(d, 110.0f, 240.0f);
            const float margin = 18.0f;
            const ImVec2 c(io.DisplaySize.x - margin - d * 0.5f,
                           io.DisplaySize.y - margin - d * 0.5f);
            const ImVec2 p0(c.x - d * 0.5f, c.y - d * 0.5f);
            const ImVec2 p1(c.x + d * 0.5f, c.y + d * 0.5f);

            // Center-crop the video to a square via UVs so the circle isn't
            // squashed (cam feeds are 16:9 or 4:3, the bubble is 1:1).
            ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
            if (vw > vh) {
                const float m = (1.0f - static_cast<float>(vh) / static_cast<float>(vw)) * 0.5f;
                uv0.x = m; uv1.x = 1.0f - m;
            } else if (vh > vw) {
                const float m = (1.0f - static_cast<float>(vw) / static_cast<float>(vh)) * 0.5f;
                uv0.y = m; uv1.y = 1.0f - m;
            }

            dl->AddImageRounded((ImTextureID)(uintptr_t)g_cam_tex, p0, p1,
                                uv0, uv1, IM_COL32_WHITE, d * 0.5f);
            dl->AddCircle(c, d * 0.5f, Theme::u32(Theme::Tokens::BD2), 0, 2.0f);
        }
    }
#else
    (void)session_alive;
#endif
}

}  // namespace ClipRecorder
