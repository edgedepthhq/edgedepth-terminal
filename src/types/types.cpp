#include "types.h"
#include <nlohmann/json.hpp>

namespace Terminal {
    std::string create_subscribe_message(
        const std::string& exchange,
        const std::string& symbol,
        Stream stream,
        int64_t timeframe
    ) {
        nlohmann::json msg = {
            {"method", "subscribe"},
            {"data", {
                    {"pair", {
                        {"exchange", exchange},
                        {"symbol", symbol}
                    }},
                    {"stream", static_cast<uint32_t>(stream)},
                    {"timeframe", timeframe}
            }}
        };
        return msg.dump();
    }

    std::string create_unsubscribe_message(
        const std::string& exchange,
        const std::string& symbol,
        Stream stream,
        int64_t timeframe
    ) {
        nlohmann::json msg = {
            {"method", "unsubscribe"},
            {"data", {
                    {"pair", {
                        {"exchange", exchange},
                        {"symbol", symbol}
                    }},
                    {"stream", static_cast<uint32_t>(stream)},
                    {"timeframe", timeframe}
            }}
        };
        return msg.dump();
    }
}