#include "nodes/AudioDecoderNode.hpp"
#include "nodes/AudioEncoderNode.hpp"
#include "nodes/AudioResamplerNode.hpp"
#include "nodes/DemuxNode.hpp"
#include "nodes/MediaFrame.hpp"
#include "nodes/MuxerNode.hpp"
#include "nodes/VideoDecoderNode.hpp"
#include "nodes/VideoEncoderNode.hpp"
#include "nodes/VideoScalerNode.hpp"

#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {
void check(bool ok, const std::string& why) {
    if (!ok) throw std::logic_error(why);
}

template<class T> class Source final : public NodeBase {
public:
    Source() { addOutput<T>(); }
    int process() override { return Finished; }
    void send(T value) { check(output<T>()->push(std::move(value)), "source push failed"); }
    void close() { output<T>()->close(); }
};

template<class T> class Sink final : public NodeBase {
public:
    Sink() { addInput<T>(); }
    int process() override { return Finished; }
    T take() { auto value = input<T>()->pop(); check(value.has_value(), "missing output"); return std::move(*value); }
    void close() { input<T>()->close(); }
    void expectClosed() { check(!input<T>()->pop().has_value(), "unexpected output"); }
};

MediaStream stream(bool video, AVRational time_base = {1, 1000}) {
    auto* raw = avcodec_parameters_alloc();
    check(raw != nullptr, "parameters allocation failed");
    std::shared_ptr<AVCodecParameters> codec(raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    raw->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    raw->codec_id = video ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_PCM_F32LE;
    raw->format = video ? static_cast<int>(AV_PIX_FMT_YUV420P) : static_cast<int>(AV_SAMPLE_FMT_FLTP);
    raw->width = 32;
    raw->height = 24;
    raw->sample_rate = 48000;
    av_channel_layout_default(&raw->ch_layout, 1);
    return {std::move(codec), time_base, {25, 1}};
}

AVFramePtr videoFrame() {
    auto* raw = av_frame_alloc();
    check(raw != nullptr, "frame allocation failed");
    AVFramePtr value(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->format = AV_PIX_FMT_YUV420P;
    raw->width = 32;
    raw->height = 24;
    raw->pts = 0;
    check(av_frame_get_buffer(raw, 0) >= 0, "video buffer allocation failed");
    return value;
}

AVFramePtr audioFrame() {
    auto* raw = av_frame_alloc();
    check(raw != nullptr, "frame allocation failed");
    AVFramePtr value(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->format = AV_SAMPLE_FMT_FLTP;
    raw->sample_rate = 48000;
    raw->nb_samples = 32;
    raw->pts = 0;
    av_channel_layout_default(&raw->ch_layout, 1);
    check(av_frame_get_buffer(raw, 0) >= 0, "audio buffer allocation failed");
    return value;
}

using Options = std::unordered_map<std::string, std::string>;

template<class Node> void invalidOptions(const std::vector<Options>& cases, const char* name) {
    for (const auto& options : cases) {
        Node node;
        check(node.configure(options) == NodeBase::InvalidConfiguration, std::string(name) + " accepted invalid option");
        check(node.status() == NodeBase::InvalidConfiguration, std::string(name) + " lost option error");
        check(node.configure({}) == NodeBase::Success, std::string(name) + " did not recover after invalid option");
    }
}

template<class Node, class In, class Out>
void reject(In invalid, int expected, const char* name, bool after_config,
            std::function<In()> config) {
    Node node;
    Source<In> source;
    Sink<Out> sink;
    source.connect(node);
    node.connect(sink);
    if (after_config) {
        source.send(config());
        check(node.process() == NodeBase::Success, std::string(name) + " rejected valid configuration");
        if constexpr (std::is_same_v<In, MediaFrame>) {
            if constexpr (std::is_same_v<Out, MediaFrame> || std::is_same_v<Out, MediaPacket>)
                check(sink.take().type == Out::Type::CONFIGFRAME, "missing forwarded configuration");
        }
    }
    source.send(std::move(invalid));
    check(node.process() == expected, std::string(name) + " returned wrong error");
    check(node.status() == expected && node.process() == expected,
          std::string(name) + " error was not terminal");
}

void helpers() {
    std::size_t count = 7;
    check(mediaPortCount({}, 3) == 3 && parseMediaPortCount({}, count) && count == 7, "port fallback changed");
    for (const auto* value : {"", "0", "-1", "+2", " 2", "2 ", "2x", "184467440737095516160"}) {
        count = 7;
        check(!parseMediaPortCount({{"ports", value}}, count) && count == 7, "invalid port count mutated output");
        bool threw = false;
        try { (void)mediaPortCount({{"ports", value}}); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw, "mediaPortCount accepted invalid value");
    }
    check(parseMediaPortCount({{"ports", "1"}}, count) && count == 1, "minimum port count rejected");
    check(parseMediaPortCount({{"ports", "12"}}, count) && count == 12, "multi-digit port count rejected");
    auto packet = makeAVPacket();
    auto frame = videoFrame();
    auto pc = MediaPacket::configFrame(9, stream(true));
    auto pd = MediaPacket::dataFrame(9, packet);
    auto pe = MediaPacket::endFrame(9);
    auto fc = MediaFrame::configFrame(9, stream(true));
    auto fd = MediaFrame::dataFrame(9, frame);
    auto fe = MediaFrame::endFrame(9);
    check(pc.config && !pc.packet && pd.packet == packet && !pd.config && !pe.packet && !pe.config,
          "packet factories changed payload ownership");
    check(fc.config && !fc.frame && fd.frame == frame && !fd.config && !fe.frame && !fe.config,
          "frame factories changed payload ownership");
}

void options() {
    const std::vector<Options> numeric = {{{"threads", ""}}, {{"threads", "0"}},
        {{"threads", "-1"}}, {{"threads", "+2"}}, {{"threads", "2x"}},
        {{"threads", "2147483648"}}, {{"unknown", "1"}}};
    invalidOptions<VideoDecoderNode>(numeric, "video decoder");
    invalidOptions<AudioDecoderNode>(numeric, "audio decoder");
    const std::vector<Options> encoder = {{{"threads", "0"}}, {{"threads", "2147483648"}},
        {{"bit_rate", "0"}}, {{"bit_rate", "-1"}}, {{"bit_rate", "x"}},
        {{"bit_rate", "9223372036854775808"}}, {{"unknown", "1"}}};
    invalidOptions<VideoEncoderNode>(encoder, "video encoder");
    invalidOptions<AudioEncoderNode>(encoder, "audio encoder");
    invalidOptions<VideoEncoderNode>({{{"gop_size", "0"}}, {{"gop_size", "2147483648"}},
        {{"max_b_frames", "-1"}}, {{"max_b_frames", "17"}}, {{"preset", ""}},
        {{"tune", ""}}}, "video encoder");
    invalidOptions<AudioEncoderNode>({{{"gop_size", "12"}}, {{"max_b_frames", "1"}},
        {{"preset", "fast"}}}, "audio encoder");
    invalidOptions<VideoScalerNode>({{{"width", "-1"}}, {{"height", "0"}},
        {{"height", "2147483648"}}, {{"width", "1x"}}, {{"algorithm", ""}},
        {{"pixel_format", "unknown"}}, {{"unknown", "1"}}}, "scaler");
    invalidOptions<AudioResamplerNode>({{{"sample_rate", "0"}}, {{"sample_rate", "2147483648"}},
        {{"sample_rate", "2x"}}, {{"sample_format", "unknown"}},
        {{"channel_layout", "unknown"}}, {{"unknown", "1"}}}, "resampler");
}

template<class Node> void singleFrameProtocol(bool video, const char* name) {
    const auto config = [=] { return MediaFrame::configFrame(4, stream(video)); };
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::dataFrame(4, nullptr), NodeBase::FrameBeforeConfig, name, false, config);
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::endFrame(4), NodeBase::FrameBeforeConfig, name, false, config);
    auto missing = MediaFrame::configFrame(4, stream(video)); missing.config.reset();
    reject<Node, MediaFrame, MediaFrame>(std::move(missing), NodeBase::InvalidConfigFrame, name, false, config);
    auto invalid_stream = stream(video); invalid_stream.codec.reset();
    auto no_codec = MediaFrame::configFrame(4, std::move(invalid_stream));
    reject<Node, MediaFrame, MediaFrame>(std::move(no_codec), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::configFrame(-1, stream(video)), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, stream(video, {0, 1})), NodeBase::InvalidConfigFrame, name, false, config);
    auto with_frame = MediaFrame::configFrame(4, stream(video)); with_frame.frame = video ? videoFrame() : audioFrame();
    reject<Node, MediaFrame, MediaFrame>(std::move(with_frame), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaFrame>(config(), NodeBase::DuplicateStreamId, name, true, config);
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::endFrame(5), NodeBase::StreamIdChanged, name, true, config);
    auto bad_end = MediaFrame::endFrame(4); bad_end.frame = video ? videoFrame() : audioFrame();
    reject<Node, MediaFrame, MediaFrame>(std::move(bad_end), NodeBase::InvalidEndFrame, name, true, config);
    auto bad_data = MediaFrame::dataFrame(4, video ? videoFrame() : audioFrame()); bad_data.config = config().config;
    reject<Node, MediaFrame, MediaFrame>(std::move(bad_data), NodeBase::InvalidDataFrame, name, true, config);
    reject<Node, MediaFrame, MediaFrame>(MediaFrame::dataFrame(4, nullptr), NodeBase::InvalidDataFrame, name, true, config);
    auto unknown = MediaFrame::endFrame(4); unknown.type = static_cast<MediaFrame::Type>(99);
    reject<Node, MediaFrame, MediaFrame>(std::move(unknown), NodeBase::UnknownFrameType, name, true, config);
}

template<class Node> void encoderProtocol(bool video, const char* name) {
    const auto config = [=] { return MediaFrame::configFrame(4, stream(video)); };
    reject<Node, MediaFrame, MediaPacket>(MediaFrame::endFrame(4), NodeBase::FrameBeforeConfig, name, false, config);
    auto missing = config(); missing.config.reset();
    reject<Node, MediaFrame, MediaPacket>(std::move(missing), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaPacket>(MediaFrame::configFrame(-1, stream(video)), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaPacket>(MediaFrame::configFrame(4, stream(video, {1, 0})), NodeBase::InvalidConfigFrame, name, false, config);
    auto with_frame = config(); with_frame.frame = video ? videoFrame() : audioFrame();
    reject<Node, MediaFrame, MediaPacket>(std::move(with_frame), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaFrame, MediaPacket>(config(), NodeBase::DuplicateStreamId, name, true, config);
    reject<Node, MediaFrame, MediaPacket>(MediaFrame::endFrame(5), NodeBase::StreamIdChanged, name, true, config);
    auto end = MediaFrame::endFrame(4); end.config = config().config;
    reject<Node, MediaFrame, MediaPacket>(std::move(end), NodeBase::InvalidEndFrame, name, true, config);
    reject<Node, MediaFrame, MediaPacket>(MediaFrame::dataFrame(4, nullptr), NodeBase::InvalidDataFrame, name, true, config);
    auto data = MediaFrame::dataFrame(4, video ? videoFrame() : audioFrame()); data.config = config().config;
    reject<Node, MediaFrame, MediaPacket>(std::move(data), NodeBase::InvalidDataFrame, name, true, config);
    auto unknown = MediaFrame::endFrame(4); unknown.type = static_cast<MediaFrame::Type>(99);
    reject<Node, MediaFrame, MediaPacket>(std::move(unknown), NodeBase::UnknownFrameType, name, true, config);
}

template<class Node> void decoderProtocol(bool video, const char* name) {
    const auto config = [=] { return MediaPacket::configFrame(4, stream(video)); };
    reject<Node, MediaPacket, MediaFrame>(MediaPacket::endFrame(4), NodeBase::FrameBeforeConfig, name, false, config);
    auto missing = config(); missing.config.reset();
    reject<Node, MediaPacket, MediaFrame>(std::move(missing), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaPacket, MediaFrame>(MediaPacket::configFrame(-1, stream(video)), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaPacket, MediaFrame>(MediaPacket::configFrame(4, stream(video, {0, 1})), NodeBase::InvalidConfigFrame, name, false, config);
    auto with_packet = config(); with_packet.packet = makeAVPacket();
    reject<Node, MediaPacket, MediaFrame>(std::move(with_packet), NodeBase::InvalidConfigFrame, name, false, config);
    reject<Node, MediaPacket, MediaFrame>(config(), NodeBase::DuplicateStreamId, name, true, config);
    reject<Node, MediaPacket, MediaFrame>(MediaPacket::endFrame(5), NodeBase::StreamIdChanged, name, true, config);
    auto end = MediaPacket::endFrame(4); end.config = config().config;
    reject<Node, MediaPacket, MediaFrame>(std::move(end), NodeBase::InvalidEndFrame, name, true, config);
    reject<Node, MediaPacket, MediaFrame>(MediaPacket::dataFrame(4, nullptr), NodeBase::InvalidDataFrame, name, true, config);
    auto data = MediaPacket::dataFrame(4, makeAVPacket()); data.config = config().config;
    reject<Node, MediaPacket, MediaFrame>(std::move(data), NodeBase::InvalidDataFrame, name, true, config);
    auto unknown = MediaPacket::endFrame(4); unknown.type = static_cast<MediaPacket::Type>(99);
    reject<Node, MediaPacket, MediaFrame>(std::move(unknown), NodeBase::UnknownFrameType, name, true, config);
}

void demuxProtocol() {
    const auto config = [] { return MediaPacket::configFrame(4, stream(true)); };
    const auto test = [&](MediaPacket invalid, int expected, bool after_config) {
        DemuxNode node(1);
        Source<MediaPacket> source;
        Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        if (after_config) {
            source.send(config()); check(node.process() == NodeBase::Success, "demux config rejected");
            check(sink.take().stream_id == 4, "demux config not forwarded");
        }
        source.send(std::move(invalid));
        check(node.process() == expected && node.process() == expected, "demux error or terminal status wrong");
    };
    test(MediaPacket::endFrame(-1), NodeBase::InvalidStreamId, false);
    test(MediaPacket::endFrame(4), NodeBase::FrameBeforeConfig, false);
    auto missing = config(); missing.config.reset(); test(std::move(missing), NodeBase::InvalidConfigFrame, false);
    test(MediaPacket::configFrame(4, stream(true, {1, 0})), NodeBase::InvalidConfigFrame, false);
    auto with_packet = config(); with_packet.packet = makeAVPacket(); test(std::move(with_packet), NodeBase::InvalidConfigFrame, false);
    test(config(), NodeBase::DuplicateStreamId, true);
    test(MediaPacket::endFrame(5), NodeBase::FrameBeforeConfig, true);
    auto data = MediaPacket::dataFrame(4, makeAVPacket()); data.config = config().config;
    test(std::move(data), NodeBase::InvalidDataFrame, true);
    auto end = MediaPacket::endFrame(4); end.packet = makeAVPacket();
    test(std::move(end), NodeBase::InvalidEndFrame, true);
    auto unknown = MediaPacket::endFrame(4); unknown.type = static_cast<MediaPacket::Type>(99);
    test(std::move(unknown), NodeBase::UnknownFrameType, true);
    {
        DemuxNode node(1); Source<MediaPacket> source; Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        source.send(config()); check(node.process() == 0, "demux config failed"); (void)sink.take();
        source.send(MediaPacket::endFrame(4)); check(node.process() == NodeBase::Finished, "demux EOF failed");
        check(sink.take().type == MediaPacket::Type::ENDFRAME, "demux lost EOF");
        source.send(MediaPacket::endFrame(4));
        check(node.process() == NodeBase::FrameAfterEnd, "demux accepted frame after EOF");
    }
    {
        DemuxNode node(1); Source<MediaPacket> source; Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        source.send(config()); check(node.process() == 0, "demux config failed"); (void)sink.take();
        source.send(MediaPacket::configFrame(5, stream(true)));
        check(node.process() == NodeBase::InsufficientPorts, "demux accepted excess stream");
    }
}

void muxProtocol() {
    const auto config = [] { return MediaPacket::configFrame(4, stream(true)); };
    const auto test = [&](MediaPacket invalid, int expected, bool after_config) {
        MuxerNode node(1); Source<MediaPacket> source; Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        if (after_config) {
            source.send(config()); check(node.process() == 0, "mux config failed");
            check(sink.take().stream_id == 4, "mux config not forwarded");
        }
        source.send(std::move(invalid));
        check(node.process() == expected && node.process() == expected, "mux error or terminal status wrong");
    };
    auto missing = config(); missing.config.reset(); test(std::move(missing), NodeBase::InvalidConfigFrame, false);
    test(MediaPacket::configFrame(-1, stream(true)), NodeBase::InvalidConfigFrame, false);
    test(MediaPacket::configFrame(4, stream(true, {1, 0})), NodeBase::InvalidConfigFrame, false);
    auto with_packet = config(); with_packet.packet = makeAVPacket(); test(std::move(with_packet), NodeBase::InvalidConfigFrame, false);
    test(MediaPacket::endFrame(5), NodeBase::StreamIdChanged, true);
    test(config(), NodeBase::InvalidDataFrame, true);
    auto data = MediaPacket::dataFrame(4, makeAVPacket()); data.config = config().config;
    test(std::move(data), NodeBase::InvalidDataFrame, true);
    auto end = MediaPacket::endFrame(4); end.config = config().config;
    test(std::move(end), NodeBase::InvalidEndFrame, true);
    auto unknown = MediaPacket::endFrame(4); unknown.type = static_cast<MediaPacket::Type>(99);
    test(std::move(unknown), NodeBase::InvalidDataFrame, true);
}

void mediaFormats() {
    const auto invalid_video = [] {
        auto value = stream(true); value.codec->width = 0; return value;
    };
    const auto invalid_audio = [] {
        auto value = stream(false); value.codec->sample_rate = 0; return value;
    };
    const auto video_config = [] { return MediaFrame::configFrame(4, stream(true)); };
    const auto audio_config = [] { return MediaFrame::configFrame(4, stream(false)); };
    reject<VideoScalerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, invalid_video()),
        NodeBase::InvalidConfigFrame, "scaler dimensions", false, video_config);
    auto unsupported_video = stream(true); unsupported_video.codec->format = -1;
    reject<VideoScalerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, std::move(unsupported_video)),
        NodeBase::InvalidConfigFrame, "scaler format", false, video_config);
    reject<VideoScalerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, stream(false)),
        NodeBase::InvalidConfigFrame, "scaler media type", false, video_config);
    reject<AudioResamplerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, invalid_audio()),
        NodeBase::InvalidConfigFrame, "resampler sample rate", false, audio_config);
    auto unsupported_audio = stream(false); unsupported_audio.codec->format = -1;
    reject<AudioResamplerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, std::move(unsupported_audio)),
        NodeBase::InvalidConfigFrame, "resampler sample format", false, audio_config);
    reject<AudioResamplerNode, MediaFrame, MediaFrame>(MediaFrame::configFrame(4, stream(true)),
        NodeBase::InvalidConfigFrame, "resampler media type", false, audio_config);
    reject<VideoEncoderNode, MediaFrame, MediaPacket>(MediaFrame::configFrame(4, invalid_video()),
        NodeBase::InvalidConfigFrame, "video encoder dimensions", false, video_config);
    reject<AudioEncoderNode, MediaFrame, MediaPacket>(MediaFrame::configFrame(4, invalid_audio()),
        NodeBase::InvalidConfigFrame, "audio encoder sample rate", false, audio_config);
    reject<VideoEncoderNode, MediaFrame, MediaPacket>(MediaFrame::configFrame(4, stream(false)),
        NodeBase::InvalidConfigFrame, "video encoder media type", false, video_config);
    reject<AudioEncoderNode, MediaFrame, MediaPacket>(MediaFrame::configFrame(4, stream(true)),
        NodeBase::InvalidConfigFrame, "audio encoder media type", false, audio_config);
    const auto video_packet_config = [] { return MediaPacket::configFrame(4, stream(true)); };
    const auto audio_packet_config = [] { return MediaPacket::configFrame(4, stream(false)); };
    reject<VideoDecoderNode, MediaPacket, MediaFrame>(MediaPacket::configFrame(4, stream(false)),
        NodeBase::InvalidConfigFrame, "video decoder media type", false, video_packet_config);
    reject<AudioDecoderNode, MediaPacket, MediaFrame>(MediaPacket::configFrame(4, stream(true)),
        NodeBase::InvalidConfigFrame, "audio decoder media type", false, audio_packet_config);

    auto bad_width = videoFrame(); bad_width->width = 16;
    reject<VideoScalerNode, MediaFrame, MediaFrame>(MediaFrame::dataFrame(4, bad_width),
        NodeBase::InvalidDataFrame, "scaler changed dimensions", true, video_config);
    reject<VideoEncoderNode, MediaFrame, MediaPacket>(MediaFrame::dataFrame(4, bad_width),
        NodeBase::InvalidDataFrame, "encoder changed dimensions", true, video_config);
    auto bad_rate = audioFrame(); bad_rate->sample_rate = 44100;
    reject<AudioResamplerNode, MediaFrame, MediaFrame>(MediaFrame::dataFrame(4, bad_rate),
        NodeBase::InvalidDataFrame, "resampler changed rate", true, audio_config);
    reject<AudioEncoderNode, MediaFrame, MediaPacket>(MediaFrame::dataFrame(4, bad_rate),
        NodeBase::InvalidDataFrame, "encoder changed rate", true, audio_config);
    auto zero_samples = audioFrame(); zero_samples->nb_samples = 0;
    reject<AudioResamplerNode, MediaFrame, MediaFrame>(MediaFrame::dataFrame(4, zero_samples),
        NodeBase::InvalidDataFrame, "resampler zero samples", true, audio_config);
    reject<AudioEncoderNode, MediaFrame, MediaPacket>(MediaFrame::dataFrame(4, zero_samples),
        NodeBase::InvalidDataFrame, "encoder zero samples", true, audio_config);
}

void closedQueues() {
    {
        DemuxNode node(2); Source<MediaPacket> source; Sink<MediaPacket> first, unused;
        source.connect(node); node.connect(first, 0); node.connect(unused, 1);
        source.send(MediaPacket::configFrame(4, stream(true)));
        check(node.process() == 0, "demux config failed"); (void)first.take();
        source.close(); check(node.process() == NodeBase::Finished, "demux did not finish on closure");
        first.close(); first.expectClosed(); unused.expectClosed();
    }
    {
        MuxerNode node(1); Source<MediaPacket> source; Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        source.send(MediaPacket::configFrame(4, stream(true)));
        check(node.process() == 0, "mux config failed"); (void)sink.take();
        source.close(); check(node.process() == NodeBase::Finished, "mux did not finish on closure");
        sink.close(); sink.expectClosed();
    }
    {
        VideoScalerNode node; Source<MediaFrame> source; Sink<MediaFrame> sink;
        source.connect(node); node.connect(sink);
        source.send(MediaFrame::configFrame(4, stream(true)));
        check(node.process() == 0, "scaler config failed"); (void)sink.take();
        source.close(); check(node.process() == NodeBase::Finished, "scaler did not finish on closure");
        sink.close(); sink.expectClosed();
    }
    {
        VideoEncoderNode node; Source<MediaFrame> source; Sink<MediaPacket> sink;
        source.connect(node); node.connect(sink);
        source.send(MediaFrame::configFrame(4, stream(true)));
        check(node.process() == 0, "encoder config failed"); (void)sink.take();
        source.close(); check(node.process() == NodeBase::Finished, "encoder did not finish on closure");
        sink.close(); sink.expectClosed();
    }
    {
        VideoDecoderNode node; Source<MediaPacket> source; Sink<MediaFrame> sink;
        source.connect(node); node.connect(sink);
        source.send(MediaPacket::configFrame(4, stream(true)));
        check(node.process() == 0, "decoder config failed");
        source.close(); check(node.process() == NodeBase::Finished, "decoder did not finish on closure");
        sink.close(); sink.expectClosed();
    }
}
} // namespace

int main() {
    helpers();
    options();
    singleFrameProtocol<VideoScalerNode>(true, "scaler");
    singleFrameProtocol<AudioResamplerNode>(false, "resampler");
    encoderProtocol<VideoEncoderNode>(true, "video encoder");
    encoderProtocol<AudioEncoderNode>(false, "audio encoder");
    decoderProtocol<VideoDecoderNode>(true, "video decoder");
    decoderProtocol<AudioDecoderNode>(false, "audio decoder");
    demuxProtocol();
    muxProtocol();
    mediaFormats();
    closedQueues();
}
