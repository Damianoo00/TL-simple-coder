#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <charconv>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

inline int logMediaResult(int result, const std::string& operation) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        std::fprintf(stderr, "%s: ffmpeg_code=%d: %s\n", operation.c_str(), result, message);
    }
    return result;
}

struct MediaStream {
    std::shared_ptr<AVCodecParameters> codec;
    AVRational time_base;
    AVRational frame_rate{0, 1};
};
using AVPacketPtr = std::shared_ptr<AVPacket>;

inline AVPacketPtr makeAVPacket() {
    auto packet = av_packet_alloc();
    if (!packet) throw std::bad_alloc();
    return AVPacketPtr(packet, [](AVPacket* p) { av_packet_free(&p); });
}

struct MediaPacket {
    enum class Type { CONFIGFRAME, DATAFRAME, ENDFRAME };

    Type type;
    int stream_id;
    std::shared_ptr<const MediaStream> config;
    AVPacketPtr packet;

    static MediaPacket configFrame(int id, MediaStream stream) {
        return {Type::CONFIGFRAME, id, std::make_shared<const MediaStream>(std::move(stream)), nullptr};
    }
    static MediaPacket dataFrame(int id, AVPacketPtr packet) {
        return {Type::DATAFRAME, id, nullptr, std::move(packet)};
    }
    static MediaPacket endFrame(int id) {
        return {Type::ENDFRAME, id, nullptr, nullptr};
    }
};

inline std::size_t mediaPortCount(const std::unordered_map<std::string, std::string>& config,
                                  std::size_t fallback = 2) {
    const auto entry = config.find("ports");
    if (entry == config.end()) return fallback;
    std::size_t count = 0;
    const auto& value = entry->second;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || count == 0)
        throw std::invalid_argument("ports must be a positive integer");
    return count;
}

// Leaves count unchanged on invalid input.
inline bool parseMediaPortCount(const std::unordered_map<std::string, std::string>& config,
                                std::size_t& count) {
    const auto entry = config.find("ports");
    if (entry == config.end()) return count > 0;
    std::size_t parsed = 0;
    const auto& value = entry->second;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !parsed) return false;
    count = parsed;
    return true;
}
