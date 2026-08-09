// ═══════════════════════════════════════════════════════════════════════════════
// education_boot.cpp — see education_boot.h
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/education_boot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__

// C callback invoked from the JS XHR when the LessonDoc fetch completes.
extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void _education_on_lesson_fetch(const char* json_str, int len) {
        EducationBoot::instance().on_fetch(json_str, static_cast<size_t>(len));
    }
}

void EducationBoot::detect() {
    // Read window.__EDGEDEPTH_LESSON__ into a malloc'd C string of the form
    // "<mode>\n<docUrl>" (docUrl may be empty for studio), or 0 if the global is
    // absent. Two fields, newline-separated, so one EM_ASM_PTR returns both.
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var cfg = window.__EDGEDEPTH_LESSON__;
            if (!cfg) return 0;
            var mode = String(cfg.mode || (cfg.docUrl ? "lesson" : ""));
            var url = String(cfg.docUrl || "");
            var sym = String(cfg.symbol || "");   // studio: the picked symbol
            if (!mode && !url) return 0;
            // Three newline-separated fields: mode \n docUrl \n studioSymbol
            var s = mode + '\n' + url + '\n' + sym;
            var len = lengthBytesUTF8(s);
            var buf = _malloc(len + 1);
            stringToUTF8(s, buf, len + 1);
            return buf;
        } catch (e) {
            console.warn('EducationBoot: failed reading __EDGEDEPTH_LESSON__:', e);
            return 0;
        }
    }));

    if (raw) {
        std::string blob(raw);
        free(raw);
        // Split on newlines into [mode, docUrl, studioSymbol].
        const auto nl1 = blob.find('\n');
        const std::string mode = (nl1 == std::string::npos) ? blob : blob.substr(0, nl1);
        std::string rest = (nl1 == std::string::npos) ? "" : blob.substr(nl1 + 1);
        const auto nl2 = rest.find('\n');
        doc_url_ = (nl2 == std::string::npos) ? rest : rest.substr(0, nl2);
        studio_symbol_ = (nl2 == std::string::npos) ? "" : rest.substr(nl2 + 1);
        // Normalize the studio symbol to lowercase (the rest of the client expects it).
        std::transform(studio_symbol_.begin(), studio_symbol_.end(), studio_symbol_.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (mode == "studio") {
            mode_ = Mode::Studio;
        } else if (mode == "lesson" || !doc_url_.empty()) {
            mode_ = Mode::Lesson;
        } else {
            mode_ = Mode::None;
        }
    }

    // No lesson/studio global → maybe this is an archived market-event replay.
    // The web sets window.__EDGEDEPTH_EVENT__ = {sessionType:'archive', eventId,
    // symbol, seekToStart} before the glue loads (mirrors __EDGEDEPTH_LESSON__).
    // Three newline-separated fields: eventId \n symbol \n seekToStart(1|0).
    if (mode_ == Mode::None) {
        char* eraw = reinterpret_cast<char*>(EM_ASM_PTR({
            try {
                var ev = window.__EDGEDEPTH_EVENT__;
                if (!ev || !ev.eventId) return 0;
                // Six newline-separated fields: eventId \n symbol \n seekToStart \n
                // startMs \n endMs \n seekToMs. The window (epoch ms) lets EventRuntime
                // report a stable transport window before the backend's first status
                // arrives; seekToMs (0 = none) is the ?event=…&t=… deep-link target.
                var s = String(ev.eventId) + '\n' + String(ev.symbol || "") + '\n' +
                        (ev.seekToStart === false ? '0' : '1') + '\n' +
                        String(ev.startMs || 0) + '\n' + String(ev.endMs || 0) + '\n' +
                        String(ev.seekToMs || 0);
                var len = lengthBytesUTF8(s);
                var buf = _malloc(len + 1);
                stringToUTF8(s, buf, len + 1);
                return buf;
            } catch (e) {
                console.warn('EducationBoot: failed reading __EDGEDEPTH_EVENT__:', e);
                return 0;
            }
        }));
        if (eraw) {
            std::string blob(eraw);
            free(eraw);
            // Split into up to 6 newline-separated fields: eventId, symbol, seek,
            // startMs, endMs, seekToMs (epoch ms; absent → 0).
            std::string parts[6];
            {
                size_t pos = 0;
                for (int i = 0; i < 6; ++i) {
                    const auto nl = blob.find('\n', pos);
                    if (nl == std::string::npos) { parts[i] = blob.substr(pos); break; }
                    parts[i] = blob.substr(pos, nl - pos);
                    pos = nl + 1;
                }
            }
            event_id_     = parts[0];
            event_symbol_ = parts[1];
            event_seek_to_start_ = (parts[2] != "0");
            event_start_ms_ = parts[3].empty() ? 0 : std::strtoll(parts[3].c_str(), nullptr, 10);
            event_end_ms_   = parts[4].empty() ? 0 : std::strtoll(parts[4].c_str(), nullptr, 10);
            event_seek_to_ms_ = parts[5].empty() ? 0 : std::strtoll(parts[5].c_str(), nullptr, 10);
            // Normalize symbol to lowercase (the rest of the client expects it).
            std::transform(event_symbol_.begin(), event_symbol_.end(), event_symbol_.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (!event_id_.empty()) mode_ = Mode::Event;
        }
    }

    // No lesson/studio/event global → maybe a CDN-served pack replay
    // (Hot Replay Path B). Host page: window.__EDGEDEPTH_PACK__ = {url, symbol,
    // embedded}; standalone dev: ?pack=<url>&packsym=<symbol> query params.
    if (mode_ == Mode::None) {
        char* praw = reinterpret_cast<char*>(EM_ASM_PTR({
            try {
                var url = "";
                var sym = "";
                var emb = "0";
                var skt = "0";
                var pk = window.__EDGEDEPTH_PACK__;
                if (pk && pk.url) {
                    url = String(pk.url);
                    sym = String(pk.symbol || "");
                    // Default STANDALONE (native transport in-canvas). A React
                    // chrome that owns the transport (Phase 2) opts in with
                    // embedded: true.
                    emb = (pk.embedded === true) ? "1" : "0";
                    // Start-at / deep-link target (epoch ms, 0 = none): the /demo
                    // route bakes the catalog startAtMs or its ?t= override here.
                    skt = String(pk.seekToMs || 0);
                } else {
                    var qs = new URLSearchParams(window.location.search);
                    var qurl = qs.get('pack');
                    if (qurl) {
                        url = String(qurl);
                        sym = String(qs.get('packsym') || "");
                        emb = "0";  // dev boot keeps the native transport
                        skt = String(qs.get('packt') || 0);  // dev deep-link (epoch ms)
                    }
                }
                if (!url) return 0;
                var s = url + '\n' + sym + '\n' + emb + '\n' + skt;
                var len = lengthBytesUTF8(s);
                var buf = _malloc(len + 1);
                stringToUTF8(s, buf, len + 1);
                return buf;
            } catch (e) {
                console.warn('EducationBoot: failed reading __EDGEDEPTH_PACK__:', e);
                return 0;
            }
        }));
        if (praw) {
            std::string blob(praw);
            free(praw);
            std::string parts[4];
            {
                size_t pos = 0;
                for (int i = 0; i < 4; ++i) {
                    const auto nl = blob.find('\n', pos);
                    if (nl == std::string::npos) { parts[i] = blob.substr(pos); break; }
                    parts[i] = blob.substr(pos, nl - pos);
                    pos = nl + 1;
                }
            }
            pack_url_ = parts[0];
            pack_symbol_ = parts[1];
            pack_embedded_ = (parts[2] == "1");
            pack_seek_to_ms_ = parts[3].empty() ? 0 : std::strtoll(parts[3].c_str(), nullptr, 10);
            std::transform(pack_symbol_.begin(), pack_symbol_.end(), pack_symbol_.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (!pack_url_.empty()) mode_ = Mode::Pack;
        }
    }

    // Recorder script (CLIP_FACTORY P2) — read LAST and INDEPENDENTLY of the
    // mode chain above: the render harness boots a normal event/pack replay
    // (those globals, exactly as today) and ADDITIONALLY injects
    // window.__EDGEDEPTH_RECORDER__ = <RecorderScript>. The script is
    // stringified whole; main() hands it to edu::RecorderRuntime::load().
    {
        char* rraw = reinterpret_cast<char*>(EM_ASM_PTR({
            try {
                var rc = window.__EDGEDEPTH_RECORDER__;
                if (!rc) {
                    // Dev boot (mirrors ?pack=): a URL-encoded RecorderScript in
                    // the ?recorder= query param, so standalone serve_threaded
                    // testing needs no pre-load global injection.
                    var q = new URLSearchParams(window.location.search).get('recorder');
                    if (q) { try { rc = JSON.parse(q); } catch (pe) { rc = null; } }
                }
                if (!rc || !rc.shots || !rc.shots.length) return 0;
                var s = JSON.stringify(rc);
                var len = lengthBytesUTF8(s);
                var buf = _malloc(len + 1);
                stringToUTF8(s, buf, len + 1);
                return buf;
            } catch (e) {
                console.warn('EducationBoot: failed reading __EDGEDEPTH_RECORDER__:', e);
                return 0;
            }
        }));
        if (rraw) {
            recorder_json_.assign(rraw);
            free(rraw);
        }
    }
}

void EducationBoot::fetch_lesson(std::function<void()> on_ready) {
    // Only lesson mode fetches a doc. Studio has no doc (window comes from the
    // picker), so fetch is a no-op there.
    if (mode_ != Mode::Lesson || doc_url_.empty()) {
        if (on_ready) on_ready();
        return;
    }
    on_ready_ = std::move(on_ready);

    // Credentialed XHR — withCredentials sends the Payload auth cookie, so the
    // /api/lesson-doc paywall gates the data exactly as it gates the page.
    // Mirrors SymbolRegistry::fetch_metadata (no -sFETCH, no Asyncify).
    EM_ASM({
        var url = UTF8ToString($0);
        var xhr = new XMLHttpRequest();
        xhr.open('GET', url, true);
        xhr.withCredentials = true;  // send auth cookie for the paywall check
        xhr.onload = function() {
            if (xhr.status === 200) {
                var text = xhr.responseText;
                var len = lengthBytesUTF8(text);
                var buf = _malloc(len + 1);
                stringToUTF8(text, buf, len + 1);
                __education_on_lesson_fetch(buf, len);
                _free(buf);
            } else {
                // 401 anon / 403 free / 404 missing — surface, don't crash.
                console.warn('EducationBoot: lesson fetch failed, status', xhr.status);
                __education_on_lesson_fetch(0, 0);
            }
        };
        xhr.onerror = function() {
            console.warn('EducationBoot: lesson fetch network error');
            __education_on_lesson_fetch(0, 0);
        };
        xhr.send();
    }, doc_url_.c_str());
}

void EducationBoot::on_fetch(const char* json_str, size_t len) {
    if (!json_str || len == 0) {
        lesson_ready_ = false;
        if (on_ready_) on_ready_();
        return;
    }
    lesson_json_.assign(json_str, len);
    lesson_ready_ = true;
    if (on_ready_) on_ready_();
}

#else
// ── Non-Emscripten stub ──────────────────────────────────────────────────────
void EducationBoot::detect() {
    mode_ = Mode::None;
}
void EducationBoot::fetch_lesson(std::function<void()> on_ready) {
    if (on_ready) on_ready();
}
void EducationBoot::on_fetch(const char*, size_t) {}
#endif
