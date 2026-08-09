#pragma once
#include <string>
#include <vector>
#include "messages.pb.h"

namespace MessageParser {

    // Decompression result
    struct DecompressionResult {
        bool success = false;
        std::string data;
        uint64_t decompressed_size = 0;
    };

    struct ParseResult {
        bool success = false;
        pb::WSPayload payload;
    };
    DecompressionResult decompress_zstd(const std::string& data);
    ParseResult parse_payload(const std::string& data);
}