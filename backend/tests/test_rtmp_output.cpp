#include "nodes/RTMPOutputNode.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
class Source : public NodeBase {
public:
    Source() { addOutput<MediaPacket>(); }
    int process() override { return Finished; }
    void send(MediaPacket packet) { require(output<MediaPacket>()->push(std::move(packet)), "push failed"); }
    void close() { output<MediaPacket>()->close(); }
};
MediaStream stream(AVMediaType type, AVCodecID id) {
    auto* raw = avcodec_parameters_alloc();
    if (!raw) throw std::bad_alloc();
    std::shared_ptr<AVCodecParameters> codec(raw, [](auto* p) { avcodec_parameters_free(&p); });
    codec->codec_type = type;
    codec->codec_id = id;
    codec->width = codec->height = 16;
    return {std::move(codec), {1, 25}};
}
struct Fixture {
    Source source;
    RTMPOutputNode sink;
    Fixture() {
        require(sink.configure({{"url", "rtmp://127.0.0.1:1/live/test"}}) == NodeBase::Success, "configure failed");
        source.connect(sink);
    }
    int send(MediaPacket packet) { source.send(std::move(packet)); return sink.process(); }
    void rejects(MediaPacket packet, int result) {
        require(send(std::move(packet)) == result, "wrong error code");
        require(sink.process() == result, "error was not terminal");
    }
    void config(int id = 3) {
        require(send(MediaPacket::configFrame(id, stream(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264))) == 0,
                "valid H264 config rejected");
    }
};
}

int main() {
    RTMPOutputNode standalone;
    require(standalone.configure({}) == NodeBase::InvalidConfiguration, "missing URL accepted");
    require(standalone.configure({{"url", "https://example.org"}}) == NodeBase::InvalidConfiguration,
            "non-RTMP URL accepted");
    require(standalone.configure({{"url", "rtmp://127.0.0.1:1/live/test"}, {"realtime", "maybe"}}) ==
            NodeBase::InvalidConfiguration, "invalid pacing accepted");
    require(standalone.configure({{"url", "rtmp://127.0.0.1:1/live/test"}}) == 0,
            "reconfiguration failed");
    Fixture f;
    f.rejects(MediaPacket::endFrame(-1), NodeBase::InvalidStreamId);
    Fixture before;
    before.rejects(MediaPacket::dataFrame(3, makeAVPacket()), NodeBase::FrameBeforeConfig);
    Fixture unsupported;
    unsupported.rejects(MediaPacket::configFrame(3, stream(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4)),
                        NodeBase::InvalidConfigFrame);
    Fixture duplicate;
    duplicate.config();
    duplicate.rejects(MediaPacket::configFrame(3, stream(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264)),
                      NodeBase::DuplicateStreamId);
    Fixture invalid;
    invalid.config();
    invalid.rejects(MediaPacket::dataFrame(3, nullptr), NodeBase::InvalidDataFrame);
    Fixture eof;
    eof.config();
    eof.rejects(MediaPacket::endFrame(4), NodeBase::FrameBeforeConfig);
    Fixture cancelled;
    cancelled.config();
    cancelled.source.close();
    require(cancelled.sink.process() == NodeBase::Finished, "closed queue did not cancel");
}
