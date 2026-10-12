#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <memory>
#include <stdexcept>
#include <string>

inline void checkMedia(int result, const std::string& operation) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(operation + ": " + message);
    }
}

struct MediaStream {
    std::shared_ptr<AVCodecParameters> codec;
    AVRational time_base;
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
