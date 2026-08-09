#include "core/display_time_zone.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <emscripten.h>
#include <nlohmann/json.hpp>

namespace {

EM_JS(void, edtz_bootstrap, (), {
    if (Module.__edtz) return;
    const normalize = (raw) => {
        if (typeof raw !== 'string' || !raw.trim()) return null;
        const value = raw.trim();
        if (/^(UTC|GMT|Etc[/]UTC|Etc[/]GMT|Zulu)$/i.test(value)) return 'UTC';
        if (!value.includes('/')) return null;
        try {
            const zone = new Intl.DateTimeFormat('en-US', {timeZone: value})
                .resolvedOptions().timeZone;
            if (/^(UTC|GMT|Etc[/]UTC|Etc[/]GMT)$/i.test(zone)) return 'UTC';
            if (zone === 'Asia/Calcutta') return 'Asia/Kolkata';
            if (zone === 'Asia/Katmandu') return 'Asia/Kathmandu';
            return zone;
        } catch (_) { return null; }
    };
    const browserZone = () => normalize(Intl.DateTimeFormat().resolvedOptions().timeZone) || 'UTC';
    const parse = (value) => {
        try {
            const p = typeof value === 'string' ? JSON.parse(value) : value;
            if (!p || p.version !== 1 || !['local','utc','named'].includes(p.mode)) return null;
            let zone = p.mode === 'local' ? browserZone() : normalize(p.time_zone);
            if (p.mode === 'utc') zone = 'UTC';
            return zone ? {version: 1, mode: p.mode, time_zone: zone} : null;
        } catch (_) { return null; }
    };
    const cookie = () => {
        const prefix = 'edgedepth_time_zone_v1=';
        for (const segment of document.cookie.split(';')) {
            const value = segment.trim();
            if (value.startsWith(prefix)) {
                try { return decodeURIComponent(value.slice(prefix.length)); } catch (_) { return null; }
            }
        }
        return null;
    };
    let stored = null;
    try { stored = localStorage.getItem('edgedepth_time_zone_v1'); } catch (_) {}
    const pref = parse(window.__EDGEDEPTH_TIME_ZONE__) || parse(cookie()) || parse(stored) ||
        {version: 1, mode: browserZone() === 'UTC' ? 'utc' : 'local', time_zone: browserZone()};
    Module.__edtz = {
        normalize, browserZone, parse, pref,
        formatters: new Map(), offsetFormatters: new Map()
    };

    const notifyCpp = (candidate) => {
        const next = parse(candidate);
        if (!next) return;
        Module.__edtz.pref = next;
        if (Module.calledRun && typeof Module.__display_time_zone_changed === 'function') {
            try { Module.__display_time_zone_changed(); } catch (_) {}
        }
    };
    window.addEventListener('edgedepth:time-zone-preference', (event) => {
        if (event.detail && event.detail.kind === 'display') notifyCpp(event.detail.preference);
    });
    window.addEventListener('storage', (event) => {
        if (event.key === 'edgedepth_time_zone_v1' && event.newValue) notifyCpp(event.newValue);
    });
});

EM_JS(int, edtz_read_preference, (char* out, int cap), {
    const value = JSON.stringify(Module.__edtz.pref);
    if (lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

EM_JS(int, edtz_browser_zone, (char* out, int cap), {
    const value = Module.__edtz.browserZone();
    if (lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

EM_JS(int, edtz_validate_zone, (const char* raw, char* out, int cap), {
    const value = Module.__edtz.normalize(UTF8ToString(raw));
    if (!value || lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

EM_JS(void, edtz_persist_preference, (int mode, const char* zoneRaw), {
    const names = ['local', 'utc', 'named'];
    const modeName = names[mode] || 'utc';
    const requested = UTF8ToString(zoneRaw);
    const zone = modeName === 'local' ? Module.__edtz.browserZone()
        : modeName === 'utc' ? 'UTC' : Module.__edtz.normalize(requested);
    if (!zone) return;
    const pref = {version: 1, mode: modeName, time_zone: zone};
    Module.__edtz.pref = pref;
    window.__EDGEDEPTH_TIME_ZONE__ = pref;
    const raw = JSON.stringify(pref);
    try { localStorage.setItem('edgedepth_time_zone_v1', raw); } catch (_) {}
    const shared = /(^|[.])edgedepth[.]com$/i.test(location.hostname)
        ? '; Domain=.edgedepth.com' : String();
    const secure = location.protocol === 'https:' ? '; Secure' : String();
    document.cookie = 'edgedepth_time_zone_v1=' + encodeURIComponent(raw) +
        '; Path=/; Max-Age=31536000; SameSite=Lax' + shared + secure;
    window.dispatchEvent(new CustomEvent('edgedepth:time-zone-preference', {
        detail: {kind: 'display', preference: pref}
    }));
    window.dispatchEvent(new CustomEvent('edgedepth:time-zone-change', {detail: pref}));
});

EM_JS(int, edtz_supported_zones, (char* out, int cap), {
    let rawZones = [];
    try {
        rawZones = typeof Intl.supportedValuesOf === 'function'
            ? Intl.supportedValuesOf('timeZone') : [];
    } catch (_) {}
    // supportedValuesOf is normally canonical already, but engines have shipped
    // legacy aliases. Run every entry through the same normalizer used by manual
    // input, then de-duplicate so the picker exposes one row per IANA identity.
    const seen = new Set();
    const zones = [];
    for (const rawZone of ['UTC', ...rawZones]) {
        const zone = Module.__edtz.normalize(rawZone);
        if (zone && !seen.has(zone)) {
            seen.add(zone);
            zones.push(zone);
        }
    }
    zones.sort((left, right) => left === 'UTC' ? -1 : right === 'UTC' ? 1
        : left.localeCompare(right, 'en'));
    const value = JSON.stringify(zones);
    if (lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

EM_JS(int, edtz_format_epoch, (double epochMs, const char* zoneRaw, int style, char* out, int cap), {
    const zone = UTF8ToString(zoneRaw);
    const needsSeconds = style === 0 || style === 3 || style === 9;
    const key = zone + '|' + (needsSeconds ? 's' : 'm');
    let formatter = Module.__edtz.formatters.get(key);
    if (!formatter) {
        if (Module.__edtz.formatters.size >= 16) Module.__edtz.formatters.clear();
        formatter = new Intl.DateTimeFormat('en-US', {
            timeZone: zone, year: 'numeric', month: '2-digit', day: '2-digit',
            weekday: 'short', hour: '2-digit', minute: '2-digit',
            ...(needsSeconds ? {second: '2-digit'} : {}), hourCycle: 'h23'
        });
        Module.__edtz.formatters.set(key, formatter);
    }
    const parts = Object.create(null);
    for (const part of formatter.formatToParts(new Date(epochMs))) {
        if (part.type !== 'literal') parts[part.type] = part.value;
    }
    const y = parts.year, mo = parts.month, d = parts.day;
    const h = parts.hour, mi = parts.minute, s = parts.second || '00';
    const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    const monthName = months[Math.max(0, Number(mo) - 1)];
    let value = String();
    if (style === 0) value = `${h}:${mi}:${s}`;
    else if (style === 1) value = `${h}:${mi}`;
    else if (style === 2) value = `${y}-${mo}-${d} ${h}:${mi}`;
    else if (style === 3) value = `${y}-${mo}-${d} ${h}:${mi}:${s}`;
    else if (style === 4) value = `${mo}/${d} ${h}:${mi}`;
    else if (style === 5) value = `${y}-${mo}-${d}`;
    else if (style === 6) value = `${d} ${monthName} ${y}`;
    else if (style === 7) value = `${parts.weekday} ${mo}/${d}`;
    else if (style === 8) value = `${mo}/${d}`;
    else value = `${y}-${mo}-${d} ${h}:${mi}:${s}`;
    if (lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

EM_JS(int, edtz_offset_label, (double epochMs, const char* zoneRaw, char* out, int cap), {
    const zone = UTF8ToString(zoneRaw);
    let value = 'UTC+00:00';
    if (zone !== 'UTC') {
        try {
            let formatter = Module.__edtz.offsetFormatters.get(zone);
            if (!formatter) {
                if (Module.__edtz.offsetFormatters.size >= 64)
                    Module.__edtz.offsetFormatters.clear();
                formatter = new Intl.DateTimeFormat('en-US', {
                    timeZone: zone, timeZoneName: 'longOffset'
                });
                Module.__edtz.offsetFormatters.set(zone, formatter);
            }
            const part = formatter.formatToParts(new Date(epochMs))
                .find((p) => p.type === 'timeZoneName');
            if (part && part.value && part.value !== 'GMT' && part.value !== 'UTC') {
                value = part.value.replace(/^GMT/, 'UTC');
            }
        } catch (_) { return 0; }
    }
    if (lengthBytesUTF8(value) + 1 > cap) return 0;
    stringToUTF8(value, out, cap);
    return 1;
});

TimeZonePreferenceMode mode_from_string(const std::string& mode) {
    if (mode == "named") return TimeZonePreferenceMode::Named;
    if (mode == "utc") return TimeZonePreferenceMode::UTC;
    return TimeZonePreferenceMode::Local;
}

const char* mode_to_string(TimeZonePreferenceMode mode) {
    switch (mode) {
        case TimeZonePreferenceMode::Local: return "local";
        case TimeZonePreferenceMode::Named: return "named";
        case TimeZonePreferenceMode::UTC: return "utc";
    }
    return "utc";
}

uint64_t cache_hash(int64_t epoch_second, uint64_t generation, uint8_t style) {
    uint64_t x = static_cast<uint64_t>(epoch_second) ^ (generation * 0x9e3779b97f4a7c15ULL);
    x ^= static_cast<uint64_t>(style) * 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return x;
}

uint64_t zone_cache_hash(int64_t epoch_minute, const char* zone) {
    uint64_t hash = 1469598103934665603ULL ^ static_cast<uint64_t>(epoch_minute);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(zone); *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

DisplayTimeZone& DisplayTimeZone::instance() {
    static DisplayTimeZone service;
    return service;
}

void DisplayTimeZone::initialize() {
    if (initialized_) return;
    edtz_bootstrap();
    initialized_ = true;
    refresh_from_bridge();
    load_supported_zones();
}

void DisplayTimeZone::refresh_from_bridge() {
    char raw[512]{};
    if (!edtz_read_preference(raw, sizeof(raw))) return;
    try {
        const auto parsed = nlohmann::json::parse(raw);
        TimeZonePreference next;
        next.version = 1;
        next.mode = mode_from_string(parsed.value("mode", "utc"));
        next.time_zone = parsed.value("time_zone", "UTC");
        apply_preference(next, false);
    } catch (...) {
        apply_preference({1, TimeZonePreferenceMode::UTC, "UTC"}, false);
    }
}

bool DisplayTimeZone::apply_preference(const TimeZonePreference& next, bool persist) {
    if (preference_.mode == next.mode && preference_.time_zone == next.time_zone) return true;
    preference_ = next;
    generation_++;
    invalidate_caches();
    if (persist) {
        edtz_persist_preference(static_cast<int>(preference_.mode), preference_.time_zone.c_str());
    }
    return true;
}

bool DisplayTimeZone::validate_zone(const char* candidate, char* normalized,
                                    size_t normalized_size) const {
    if (!candidate || !*candidate || !normalized || normalized_size == 0) return false;
    return edtz_validate_zone(candidate, normalized, static_cast<int>(normalized_size)) != 0;
}

bool DisplayTimeZone::set_local() {
    char zone[128]{};
    if (!edtz_browser_zone(zone, sizeof(zone))) return false;
    return apply_preference({1, TimeZonePreferenceMode::Local, zone}, true);
}

bool DisplayTimeZone::set_utc() {
    return apply_preference({1, TimeZonePreferenceMode::UTC, "UTC"}, true);
}

bool DisplayTimeZone::set_named(const char* iana_name) {
    char normalized[128]{};
    if (!validate_zone(iana_name, normalized, sizeof(normalized))) return false;
    return apply_preference({1, TimeZonePreferenceMode::Named, normalized}, true);
}

void DisplayTimeZone::invalidate_caches() {
    for (auto& entry : format_cache_) {
        entry.epoch_second = std::numeric_limits<int64_t>::min();
    }
    for (auto& entry : offset_cache_) {
        entry.epoch_second = std::numeric_limits<int64_t>::min();
    }
    for (auto& entry : zone_offset_cache_) {
        entry.epoch_minute = std::numeric_limits<int64_t>::min();
        entry.zone[0] = '\0';
    }
}

bool DisplayTimeZone::format(int64_t epoch_ms, TimeZoneFormat style, char* out,
                             size_t out_size) const {
    if (!out || out_size == 0 || epoch_ms <= 0) return false;
    const int64_t epoch_second = epoch_ms / 1000;
    const uint8_t style_id = static_cast<uint8_t>(style);
    auto& entry = format_cache_[cache_hash(epoch_second, generation_, style_id) % format_cache_.size()];
    if (entry.epoch_second != epoch_second || entry.generation != generation_ ||
        entry.style != style) {
        if (!edtz_format_epoch(static_cast<double>(epoch_ms), preference_.time_zone.c_str(),
                               static_cast<int>(style), entry.value, sizeof(entry.value))) {
            return false;
        }
        entry.epoch_second = epoch_second;
        entry.generation = generation_;
        entry.style = style;
    }
    std::snprintf(out, out_size, "%s", entry.value);
    if (style == TimeZoneFormat::FullInspection) {
        char offset[20]{};
        if (offset_label(epoch_ms, offset, sizeof(offset))) {
            const size_t used = std::strlen(out);
            if (used + std::strlen(offset) + 2 < out_size) {
                std::snprintf(out + used, out_size - used, " %s", offset);
            }
        }
    }
    return true;
}

bool DisplayTimeZone::offset_label(int64_t epoch_ms, char* out, size_t out_size) const {
    if (!out || out_size == 0 || epoch_ms <= 0) return false;
    const int64_t epoch_second = epoch_ms / 1000;
    auto& entry = offset_cache_[cache_hash(epoch_second, generation_, 0) % offset_cache_.size()];
    if (entry.epoch_second != epoch_second || entry.generation != generation_) {
        if (!edtz_offset_label(static_cast<double>(epoch_ms), preference_.time_zone.c_str(),
                               entry.value, sizeof(entry.value))) {
            return false;
        }
        entry.epoch_second = epoch_second;
        entry.generation = generation_;
    }
    std::snprintf(out, out_size, "%s", entry.value);
    return true;
}

bool DisplayTimeZone::offset_label_for_zone(int64_t epoch_ms, const char* iana_name, char* out,
                                            size_t out_size) const {
    if (!out || out_size == 0 || !iana_name || !*iana_name || epoch_ms <= 0) return false;
    const int64_t epoch_minute = epoch_ms / 60000;
    auto& entry = zone_offset_cache_[
        zone_cache_hash(epoch_minute, iana_name) % zone_offset_cache_.size()];
    if (entry.epoch_minute != epoch_minute || std::strcmp(entry.zone, iana_name) != 0) {
        if (!edtz_offset_label(static_cast<double>(epoch_ms), iana_name,
                               entry.value, sizeof(entry.value))) {
            return false;
        }
        entry.epoch_minute = epoch_minute;
        std::snprintf(entry.zone, sizeof(entry.zone), "%s", iana_name);
    }
    std::snprintf(out, out_size, "%s", entry.value);
    return true;
}

void DisplayTimeZone::visible_label(int64_t epoch_ms, bool compact, char* out,
                                    size_t out_size) const {
    char offset[20] = "UTC+00:00";
    offset_label(epoch_ms, offset, sizeof(offset));
    if (compact) {
        char short_offset[20]{};
        std::snprintf(short_offset, sizeof(short_offset), "%s", offset);
        const size_t n = std::strlen(short_offset);
        if (n >= 6 && std::strcmp(short_offset + n - 3, ":00") == 0) short_offset[n - 3] = '\0';
        std::snprintf(out, out_size, "%s", short_offset);
    } else {
        std::snprintf(out, out_size, "%s \xc2\xb7 %s", preference_.time_zone.c_str(), offset);
    }
}

void DisplayTimeZone::load_supported_zones() {
    static char raw[32768]{};
    supported_zones_.clear();
    if (!edtz_supported_zones(raw, sizeof(raw))) {
        supported_zones_.push_back("UTC");
        return;
    }
    try {
        const auto parsed = nlohmann::json::parse(raw);
        for (const auto& value : parsed) {
            if (value.is_string()) supported_zones_.push_back(value.get<std::string>());
        }
        std::sort(supported_zones_.begin(), supported_zones_.end());
        supported_zones_.erase(
            std::unique(supported_zones_.begin(), supported_zones_.end()),
            supported_zones_.end());
        const auto utc = std::find(supported_zones_.begin(), supported_zones_.end(), "UTC");
        if (utc == supported_zones_.end()) {
            supported_zones_.insert(supported_zones_.begin(), "UTC");
        } else if (utc != supported_zones_.begin()) {
            std::rotate(supported_zones_.begin(), utc, utc + 1);
        }
    } catch (...) {
        // Intl.supportedValuesOf is optional. Manual validated entry still works.
        supported_zones_.clear();
        supported_zones_.push_back("UTC");
    }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE
void _display_time_zone_changed() {
    DisplayTimeZone::instance().refresh_from_bridge();
}
}
