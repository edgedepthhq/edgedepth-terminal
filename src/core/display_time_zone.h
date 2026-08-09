#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

enum class TimeZonePreferenceMode : uint8_t { Local, UTC, Named };

enum class TimeZoneFormat : uint8_t {
    TimeSeconds,
    TimeMinutes,
    DateTimeMinutes,
    DateTimeSeconds,
    MonthDayTime,
    DateOnly,
    ReplayDate,
    WeekdayMonthDay,
    MonthDay,
    FullInspection,
};

struct TimeZonePreference {
    uint8_t version = 1;
    TimeZonePreferenceMode mode = TimeZonePreferenceMode::Local;
    std::string time_zone = "UTC";
};

// Display-only IANA time-zone authority for the WASM terminal. Epochs remain
// untouched. Browser Intl supplies named-zone and historical DST behavior.
class DisplayTimeZone {
public:
    static DisplayTimeZone& instance();

    void initialize();
    void refresh_from_bridge();

    [[nodiscard]] const TimeZonePreference& preference() const { return preference_; }
    [[nodiscard]] const char* zone_name() const { return preference_.time_zone.c_str(); }
    [[nodiscard]] TimeZonePreferenceMode mode() const { return preference_.mode; }
    [[nodiscard]] uint64_t generation() const { return generation_; }

    bool set_local();
    bool set_utc();
    bool set_named(const char* iana_name);
    bool validate_zone(const char* candidate, char* normalized, size_t normalized_size) const;

    bool format(int64_t epoch_ms, TimeZoneFormat style, char* out, size_t out_size) const;
    bool offset_label(int64_t epoch_ms, char* out, size_t out_size) const;
    bool offset_label_for_zone(int64_t epoch_ms, const char* iana_name, char* out,
                               size_t out_size) const;
    void visible_label(int64_t epoch_ms, bool compact, char* out, size_t out_size) const;

    [[nodiscard]] const std::vector<std::string>& supported_zones() const {
        return supported_zones_;
    }

private:
    DisplayTimeZone() = default;

    struct FormatCacheEntry {
        int64_t epoch_second = std::numeric_limits<int64_t>::min();
        uint64_t generation = 0;
        TimeZoneFormat style = TimeZoneFormat::TimeSeconds;
        char value[64]{};
    };
    struct OffsetCacheEntry {
        int64_t epoch_second = std::numeric_limits<int64_t>::min();
        uint64_t generation = 0;
        char value[20]{};
    };
    struct ZoneOffsetCacheEntry {
        int64_t epoch_minute = std::numeric_limits<int64_t>::min();
        char zone[64]{};
        char value[20]{};
    };

    bool apply_preference(const TimeZonePreference& next, bool persist);
    void load_supported_zones();
    void invalidate_caches();

    bool initialized_ = false;
    TimeZonePreference preference_{};
    uint64_t generation_ = 1;
    std::vector<std::string> supported_zones_;
    mutable std::array<FormatCacheEntry, 2048> format_cache_{};
    mutable std::array<OffsetCacheEntry, 256> offset_cache_{};
    mutable std::array<ZoneOffsetCacheEntry, 128> zone_offset_cache_{};
};
