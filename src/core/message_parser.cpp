#include "message_parser.h"
#include "performance_tracker.h"
#include <zstd.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>


namespace MessageParser {
    // thread_local ensures we have one context per thread that is reused. ZSTD_createDCtx() is called only once per thread.
    static thread_local ZSTD_DCtx* dctx = ZSTD_createDCtx();
    DecompressionResult decompress_zstd(const std::string& data) {
        DecompressionResult result;
        if (data.size() < 4) {
            result.success = true;
            result.data = data;
            result.decompressed_size = data.size();
            return result;
        }
        const bool is_zstd =
            static_cast<unsigned char>(data[0]) == 0x28 &&
            static_cast<unsigned char>(data[1]) == 0xB5 &&
            static_cast<unsigned char>(data[2]) == 0x2F &&
            static_cast<unsigned char>(data[3]) == 0xFD;
        if (!is_zstd) {
            result.success = true;
            result.data = data;
            result.decompressed_size = data.size();
            return result;
        }
        unsigned long long frame_size = ZSTD_getFrameContentSize(data.data(), data.size());
        // Handle unknown/error with safe estimate
        if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN ||
            frame_size == ZSTD_CONTENTSIZE_ERROR) {
            // Protobuf + WSPayload wrapper typically compresses 2-5x
            // For safety, use 10x the compressed size as upper bound
            frame_size = data.size() * 10;
            // Cap at reasonable maximum (10MB)
            if (frame_size > 10 * 1024 * 1024) {
                frame_size = 10 * 1024 * 1024;
            }
        }
        result.data.resize(static_cast<size_t>(frame_size));
        const size_t decompressed = ZSTD_decompressDCtx(
            dctx,
        result.data.data(),
        frame_size,
        data.data(),
        data.size()
        );
        // const size_t decompressed = ZSTD_decompress(
        //     result.data.data(),
        //     frame_size,
        //     data.data(),
        //     data.size()
        // );
        if (ZSTD_isError(decompressed)) {
            return result;
        }
        // Resize to actual decompressed size
        result.data.resize(decompressed);
        result.success = true;
        result.decompressed_size = decompressed;
        return result;
    }
    ParseResult parse_payload(const std::string& data) {
        ParseResult result;
        const size_t size = data.size();
        // Strategy: Small messages use the fast default parser. Large messages (Heatmaps) use the high-limit parser.
        if (size < 1024 * 1024) { // < 1MB
            result.success = result.payload.ParseFromString(data);
        } else {
            google::protobuf::io::ArrayInputStream input_stream(data.data(), size);
            google::protobuf::io::CodedInputStream coded_input(&input_stream);
            coded_input.SetTotalBytesLimit(512 * 1024 * 1024);
            result.success = result.payload.ParseFromCodedStream(&coded_input);
        }
        return result;
    }
}
