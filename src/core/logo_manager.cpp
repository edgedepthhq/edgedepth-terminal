// ═══════════════════════════════════════════════════════════════════════════════
// logo_manager.cpp - see logo_manager.h.
//
// Browser-decode → GL-texture, mirroring recorder_glue's cam-upload path:
//   1. logo_js_load(url)   starts an <img crossOrigin=anonymous> (R2 CORS).
//   2. logo_js_state(url)  polls 0=loading / 1=ready / 2=failed.
//   3. C++ glGenTextures + params, then logo_js_upload(url,tex) binds
//      GL.textures[tex] and does texImage2D(<img>). One-time per key.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/logo_manager.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES3/gl3.h>
#endif

// ─── URL base + key normalization ─────────────────────────────────────────────
namespace {

// Coin/exchange PNG root = the URL prefix BEFORE "/coins/<stem>.png" (no trailing
// slash, no /coins). For a dedicated R2 bucket served at a custom domain root,
// this is just the domain. Overridable at boot via window.__EDGEDEPTH_LOGO_BASE__
// (TerminalEmbed injects the real R2 custom domain); compiled default below.
// Layout: <base>/coins/<stem>.png, <base>/exchanges/<id>.png
const char* kLogoBaseDefault = "https://logos.edgedepth.com";

std::string g_logo_base;  // resolved once

#ifdef __EMSCRIPTEN__
// Read window.__EDGEDEPTH_LOGO_BASE__ (malloc'd C string) or "".
EM_JS(char*, logo_js_base, (), {
    var b = (typeof window !== 'undefined' && window.__EDGEDEPTH_LOGO_BASE__) || '';
    var n = lengthBytesUTF8(b) + 1;
    var p = _malloc(n);
    stringToUTF8(b, p, n);
    return p;
});
#endif

const std::string& logo_base() {
    if (g_logo_base.empty()) {
        g_logo_base = kLogoBaseDefault;
#ifdef __EMSCRIPTEN__
        char* p = logo_js_base();
        if (p) {
            if (p[0]) g_logo_base = p;
            free(p);
        }
#endif
        // strip a trailing slash if the override carried one
        if (!g_logo_base.empty() && g_logo_base.back() == '/') g_logo_base.pop_back();
    }
    return g_logo_base;
}

// Mirror of the web app's coinIconBase() (symbolCatalog.ts): lowercase, strip a
// trailing quote asset, strip 1000x / 1m / 1b listing multipliers. Applied to
// SymbolMetadata.base_asset so client + web resolve the SAME R2 filename.
std::string coin_icon_base(const std::string& base_asset) {
    std::string s;
    s.reserve(base_asset.size());
    for (char c : base_asset) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // strip trailing quote (defensive - base_asset usually has none)
    static const char* quotes[] = {"usdt", "usdc", "busd", "fdusd", "usd"};
    for (const char* q : quotes) {
        const size_t ql = std::char_traits<char>::length(q);
        if (s.size() > ql && s.compare(s.size() - ql, ql, q) == 0) { s.erase(s.size() - ql); break; }
    }
    // strip leading 1000... (one or more zeros after 100)
    if (s.rfind("1000", 0) == 0) {
        size_t i = 3;                       // "100" then run of zeros
        while (i < s.size() && s[i] == '0') ++i;
        s.erase(0, i);
    } else if ((s.rfind("1m", 0) == 0 || s.rfind("1b", 0) == 0) && s.size() > 2 &&
               std::isalpha(static_cast<unsigned char>(s[2]))) {
        s.erase(0, 2);                      // 1m<coin> / 1b<coin>
    }
    return s;
}

// Exchange id ("binancef" | "hl") -> R2 filename stem.
std::string exchange_stem(const std::string& id) {
    if (id == "binancef" || id == "binance") return "binance";
    if (id == "hl" || id == "hyperliquid")  return "hyperliquid";
    return id;  // future venues: file named by id
}

}  // namespace

// ─── EM_JS decode bridge (state in Module['__edlogo']) ────────────────────────
#ifdef __EMSCRIPTEN__

EM_JS(void, logo_js_load, (const char* url_ptr), {
    var url = UTF8ToString(url_ptr);
    var m = Module['__edlogo'] || (Module['__edlogo'] = { imgs: {} });
    if (m.imgs[url]) return;                        // already loading/loaded
    var img = new Image();
    img.crossOrigin = 'anonymous';                  // R2 must send ACAO → untainted
    var rec = { img: img, state: 0, w: 0, h: 0 };
    m.imgs[url] = rec;
    img.onload  = function() {
        rec.w = img.naturalWidth; rec.h = img.naturalHeight;
        rec.state = (rec.w > 0 && rec.h > 0) ? 1 : 2;
    };
    img.onerror = function() { rec.state = 2; };
    img.src = url;
});

EM_JS(int, logo_js_state, (const char* url_ptr), {
    var m = Module['__edlogo']; if (!m) return 0;
    var rec = m.imgs[UTF8ToString(url_ptr)];
    return rec ? rec.state : 0;                      // 0 loading / 1 ready / 2 failed
});

// Bind the C++ glGenTextures handle and upload the decoded <img>. Runs on the
// main thread between NewFrame and Render (no draw in flight; every renderer
// re-binds). Returns 1 on success.
EM_JS(int, logo_js_upload, (const char* url_ptr, int tex), {
    try {
        var m = Module['__edlogo']; if (!m) return 0;
        var rec = m.imgs[UTF8ToString(url_ptr)];
        if (!rec || rec.state !== 1) return 0;
        var glTex = GL.textures[tex];
        if (!glTex) return 0;
        GLctx.bindTexture(GLctx.TEXTURE_2D, glTex);
        GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA,
                         GLctx.UNSIGNED_BYTE, rec.img);
        return 1;
    } catch (e) { return 0; }
});

// Drop the decoded <img> so the browser can free it (called on LRU eviction).
EM_JS(void, logo_js_release, (const char* url_ptr), {
    var m = Module['__edlogo']; if (!m) return;
    delete m.imgs[UTF8ToString(url_ptr)];
});

#else  // non-emscripten stubs (native builds don't load logos)
static void logo_js_load(const char*) {}
static int  logo_js_state(const char*) { return 2; }
static int  logo_js_upload(const char*, int) { return 0; }
static void logo_js_release(const char*) {}
#endif

// ─── Monogram fallback ────────────────────────────────────────────────────────
void draw_logo_monogram(ImDrawList* dl, const std::string& base_asset, ImVec2 p, float size) {
    // Hashed hue from the ticker so a coin's placeholder colour is stable.
    uint32_t h = 2166136261u;                        // FNV-1a
    for (char c : base_asset) { h ^= static_cast<unsigned char>(std::toupper((unsigned char)c)); h *= 16777619u; }
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    const ImU32 bg = ImColor::HSV(hue, 0.48f, 0.42f);
    const ImU32 fg = ImColor::HSV(hue, 0.16f, 0.96f);

    const float r = size * 0.5f;
    const ImVec2 c(p.x + r, p.y + r);
    dl->AddCircleFilled(c, r, bg, 0);

    // Up to 3 uppercase initials, centered, scaled to fit the circle.
    char label[4] = {0};
    int n = 0;
    for (char ch : base_asset) {
        if (n >= 3) break;
        label[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (n == 0) return;
    ImFont* font = ImGui::GetFont();
    const float want = size * (n >= 3 ? 0.42f : 0.52f);
    const float px   = std::max(8.0f, want);
    const ImVec2 ts  = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
    dl->AddText(font, px, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), fg, label);
}

// ─── LogoManager ──────────────────────────────────────────────────────────────
LogoManager& LogoManager::instance() {
    static LogoManager s;
    return s;
}

LogoManager::Entry& LogoManager::ensure(const std::string& key, const std::string& url, bool permanent) {
    auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    Entry e;
    e.url = url;
    e.permanent = permanent;
    e.last_used = frame_;
    logo_js_load(url.c_str());
    return entries_.emplace(key, std::move(e)).first->second;
}

ImTextureID LogoManager::coin(const std::string& base_asset) {
    const std::string stem = coin_icon_base(base_asset);
    if (stem.empty()) return 0;
    const std::string key = "c:" + stem;
    Entry& e = ensure(key, logo_base() + "/coins/" + stem + ".png", false);
    e.last_used = frame_;
    return e.state == State::Ready ? e.tex : 0;
}

ImTextureID LogoManager::exchange(const std::string& exchange_id) {
    const std::string stem = exchange_stem(exchange_id);
    const std::string key = "e:" + stem;
    Entry& e = ensure(key, logo_base() + "/exchanges/" + stem + ".png", true);
    e.last_used = frame_;
    return e.state == State::Ready ? e.tex : 0;
}

void LogoManager::draw_coin(ImDrawList* dl, const std::string& base_asset, ImVec2 p, float size) {
    ImTextureID tex = coin(base_asset);
    if (tex) {
        const float r = size * 0.5f;
        dl->AddImageRounded(tex, p, ImVec2(p.x + size, p.y + size),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
    } else {
        draw_logo_monogram(dl, base_asset, p, size);
    }
}

void LogoManager::draw_exchange(ImDrawList* dl, const std::string& exchange_id, ImVec2 p, float size) {
    ImTextureID tex = exchange(exchange_id);
    if (tex) {
        dl->AddImage(tex, p, ImVec2(p.x + size, p.y + size));
    }
    // No fallback glyph - exchange call sites keep their text label regardless.
}

void LogoManager::tick() {
    ++frame_;
#ifdef __EMSCRIPTEN__
    for (auto& [key, e] : entries_) {
        if (e.state != State::Loading) continue;
        const int st = logo_js_state(e.url.c_str());
        if (st == 1) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (logo_js_upload(e.url.c_str(), static_cast<int>(tex))) {
                e.tex   = (ImTextureID)(uintptr_t)tex;
                e.state = State::Ready;
            } else {
                glDeleteTextures(1, &tex);
                e.state = State::Failed;
            }
        } else if (st == 2) {
            e.state = State::Failed;
        }
    }
#endif
    evict_lru();
}

void LogoManager::evict_lru() {
#ifdef __EMSCRIPTEN__
    // Count live coin entries; evict the oldest until back under cap.
    size_t coins = 0;
    for (auto& [k, e] : entries_) if (!e.permanent) ++coins;
    while (coins > kCoinCap) {
        auto victim = entries_.end();
        uint32_t oldest = frame_;  // anything used this frame is spared
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.permanent) continue;
            if (it->second.last_used < oldest) { oldest = it->second.last_used; victim = it; }
        }
        if (victim == entries_.end()) break;  // everything used this frame; keep them
        if (victim->second.tex) {
            GLuint t = (GLuint)(uintptr_t)victim->second.tex;
            glDeleteTextures(1, &t);
        }
        logo_js_release(victim->second.url.c_str());
        entries_.erase(victim);
        --coins;
    }
#endif
}
