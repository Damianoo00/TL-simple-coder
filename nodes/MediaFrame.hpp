#pragma once
#include "FFmpeg.hpp"

using AVFramePtr = std::shared_ptr<AVFrame>;

// Codec parameters describe the decoded format. The codec ID identifies the
// source codec; encoders choose their own output codec independently.
struct MediaFrame {
    using Type = MediaPacket::Type;
    Type type;
    int stream_id;
    std::shared_ptr<const MediaStream> config;
    AVFramePtr frame;

    static MediaFrame configFrame(int id, MediaStream stream) {
        return {Type::CONFIGFRAME, id, std::make_shared<const MediaStream>(std::move(stream)), nullptr};
    }
    static MediaFrame dataFrame(int id, AVFramePtr frame) {
        return {Type::DATAFRAME, id, nullptr, std::move(frame)};
    }
    static MediaFrame endFrame(int id) { return {Type::ENDFRAME, id, nullptr, nullptr}; }
};
