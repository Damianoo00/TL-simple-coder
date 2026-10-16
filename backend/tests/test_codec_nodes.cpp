#include "core/Graph.hpp"
#include "nodes/VideoDecoderNode.hpp"
#include "nodes/AudioDecoderNode.hpp"
#include "nodes/VideoEncoderNode.hpp"
#include "nodes/AudioEncoderNode.hpp"
#include <chrono>
#include <algorithm>
#include <stdexcept>

void require(bool value, const char* detail) { if (!value) throw std::logic_error(detail); }

class FrameSender : public NodeBase {
public:
    FrameSender() { addOutput<MediaFrame>(); }
    int process() override { return Finished; }
    void send(MediaFrame frame) { require(output<MediaFrame>()->push(std::move(frame)), "push failed"); }
    void close() { output<MediaFrame>()->close(); }
};
class PacketSender : public NodeBase {
public:
    PacketSender() { addOutput<MediaPacket>(); }
    int process() override { return Finished; }
    void send(MediaPacket frame) { require(output<MediaPacket>()->push(std::move(frame)), "push failed"); }
};
class PacketSink : public NodeBase {
public:
    PacketSink() { addInput<MediaPacket>(); }
    int process() override { return Finished; }
};
class FrameSink : public NodeBase {
public:
    FrameSink() { addInput<MediaFrame>(); }
    ~FrameSink() override { stop(); }
    int process() override {
        auto message = input<MediaFrame>()->pop();
        if (!message) { finished = true; return Finished; }
        messages.push_back(std::move(*message));
        return Success;
    }
    std::vector<MediaFrame> messages;
    std::atomic<bool> finished{false};
};
MediaStream parameters(bool video) {
    auto raw = avcodec_parameters_alloc();
    require(raw != nullptr, "allocation failed");
    std::shared_ptr<AVCodecParameters> owned(raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    raw->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    raw->codec_id = video ? AV_CODEC_ID_RAWVIDEO : AV_CODEC_ID_PCM_F32LE;
    raw->format = video ? static_cast<int>(AV_PIX_FMT_YUV420P) : static_cast<int>(AV_SAMPLE_FMT_FLTP);
    raw->width = 64; raw->height = 48;
    raw->sample_rate = 48000;
    av_channel_layout_default(&raw->ch_layout, 1);
    return {std::move(owned), video ? AVRational{1, 12} : AVRational{1, 48000}, video ? AVRational{12, 1} : AVRational{0, 1}};
}
AVFramePtr makeFrame(bool video, int64_t pts, int samples = 0) {
    auto raw = av_frame_alloc();
    require(raw != nullptr, "frame allocation failed");
    AVFramePtr frame(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->format = video ? static_cast<int>(AV_PIX_FMT_YUV420P) : static_cast<int>(AV_SAMPLE_FMT_FLTP);
    raw->pts = pts;
    raw->width = 64; raw->height = 48;
    raw->sample_rate = 48000;
    raw->nb_samples = samples;
    av_channel_layout_default(&raw->ch_layout, 1);
    require(av_frame_get_buffer(raw, 0) >= 0, "frame buffer failed");
    if (video) {
        for (int plane = 0; plane < 3; ++plane) {
            const int height = plane ? 24 : 48;
            const int width = plane ? 32 : 64;
            for (int y = 0; y < height; ++y)
                std::fill_n(raw->data[plane] + y * raw->linesize[plane], width,
                            static_cast<unsigned char>(plane ? 128 : 32 + pts));
        }
    } else av_samples_set_silence(raw->extended_data, 0, samples, 1, AV_SAMPLE_FMT_FLTP);
    return frame;
}

template<typename Encoder, typename Decoder>
void roundTrip(bool video, bool empty = false) {
    Graph graph;
    auto& encoder = graph.addNode<Encoder>();
    auto& decoder = graph.addNode<Decoder>();
    auto& sink = graph.addNode<FrameSink>();
    require(encoder.configure(video ? std::unordered_map<std::string, std::string>{{"max_b_frames", "2"}} :
                                     std::unordered_map<std::string, std::string>{}) == NodeBase::Success,
            "encoder configure failed");
    require(decoder.configure({}) == NodeBase::Success, "decoder configure failed");
    FrameSender source;
    source.connect(encoder); graph.connect(encoder, decoder); graph.connect(decoder, sink);
    source.send(MediaFrame::configFrame(101, parameters(video)));
    int samples = 0;
    if (!empty) {
        for (int i = 0; i < 12; ++i) {
            // Deliberately different from AAC's 1024 samples; final block is short.
            source.send(MediaFrame::dataFrame(101, makeFrame(video, video ? i : samples, video ? 0 : 333)));
            if (!video) samples += 333;
        }
    }
    source.send(MediaFrame::endFrame(101));
    graph.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink.finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(sink.finished, "codec pipeline stalled");
    graph.stop();
    require(encoder.status() == NodeBase::Success && decoder.status() == NodeBase::Success, "codec runtime error");
    const auto& result = sink.messages;
    require(result.size() >= 2, "missing config/EOF");
    require(result.front().type == MediaFrame::Type::CONFIGFRAME && result.back().type == MediaFrame::Type::ENDFRAME,
            "incorrect config/EOF order");
    require(result.front().config && result.front().config->codec &&
            result.front().config->codec->codec_id == (video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC),
            "default encoder must produce AVC video or AAC audio");
    int decoded_samples = 0;
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(result[i].stream_id == 101, "stream ID changed");
        if (i && i + 1 < result.size()) {
            require(result[i].type == MediaFrame::Type::DATAFRAME && result[i].frame, "invalid decoded frame");
            if (video) require(result[i].frame->pts == static_cast<int64_t>(i - 1), "video timestamps changed");
            else decoded_samples += result[i].frame->nb_samples;
        }
    }
    if (empty) require(result.size() == 2, "empty stream generated frames");
    else if (video) require(result.size() == 14, "delayed video frames lost");
    else require(decoded_samples >= samples && decoded_samples <= samples + 2048, "audio samples lost or excessive padding");
}

int main() {
    VideoEncoderNode video;
    AudioEncoderNode audio;
    VideoDecoderNode decoder;
    require(video.configure({{"codec", "not-a-codec"}}) == NodeBase::InvalidConfiguration, "unknown encoder accepted");
    require(video.configure({{"codec", "aac"}}) == NodeBase::InvalidConfiguration, "wrong encoder type accepted");
    require(audio.configure({{"threads", "0"}}) == NodeBase::InvalidConfiguration, "zero threads accepted");
    require(decoder.configure({{"threads", "-1"}}) == NodeBase::InvalidConfiguration, "invalid decoder option accepted");
    require(decoder.configure({}) == NodeBase::Success, "decoder recovery failed");
    require(decoder.process() == NodeBase::OutputNotConnected, "unconnected decoder accepted");
    FrameSink frames;
    PacketSender packets;
    VideoDecoderNode invalid_decoder;
    packets.connect(invalid_decoder); invalid_decoder.connect(frames);
    packets.send(MediaPacket::configFrame(1, parameters(false)));
    require(invalid_decoder.process() == NodeBase::InvalidConfigFrame, "audio config accepted by video decoder");
    require(invalid_decoder.process() == NodeBase::InvalidConfigFrame, "decoder error is not terminal");
    FrameSender raw;
    AudioEncoderNode invalid_encoder;
    PacketSink output;
    raw.connect(invalid_encoder); invalid_encoder.connect(output);
    raw.send(MediaFrame::endFrame(1));
    require(invalid_encoder.process() == NodeBase::FrameBeforeConfig, "EOF before encoder config accepted");
    // Reject malformed payloads and stream changes without consuming more input.
    const auto reject_decoder = [](MediaPacket packet, int expected) {
        VideoDecoderNode node;
        PacketSender source;
        FrameSink target;
        source.connect(node); node.connect(target);
        auto stream = parameters(true);
        stream.codec->codec_id = AV_CODEC_ID_MPEG4;
        source.send(MediaPacket::configFrame(11, std::move(stream)));
        require(node.process() == NodeBase::Success, "valid decoder config rejected");
        source.send(std::move(packet));
        require(node.process() == expected && node.process() == expected,
                "incorrect decoder terminal error");
    };
    reject_decoder(MediaPacket::dataFrame(11, nullptr), NodeBase::InvalidDataFrame);
    reject_decoder(MediaPacket::endFrame(12), NodeBase::StreamIdChanged);
    auto bad_end = MediaPacket::endFrame(11);
    bad_end.packet = makeAVPacket();
    reject_decoder(std::move(bad_end), NodeBase::InvalidEndFrame);

    const auto reject_encoder = [](MediaFrame frame, int expected) {
        VideoEncoderNode node;
        FrameSender source;
        PacketSink target;
        source.connect(node); node.connect(target);
        source.send(MediaFrame::configFrame(11, parameters(true)));
        require(node.process() == NodeBase::Success, "valid encoder config rejected");
        source.send(std::move(frame));
        require(node.process() == expected && node.process() == expected,
                "incorrect encoder terminal error");
    };
    reject_encoder(MediaFrame::dataFrame(11, nullptr), NodeBase::InvalidDataFrame);
    reject_encoder(MediaFrame::endFrame(12), NodeBase::StreamIdChanged);
    auto bad_video = makeFrame(true, 0);
    bad_video->width = 32;
    reject_encoder(MediaFrame::dataFrame(11, std::move(bad_video)), NodeBase::InvalidDataFrame);

    AudioEncoderNode discontinuous;
    FrameSender samples;
    PacketSink compressed;
    samples.connect(discontinuous); discontinuous.connect(compressed);
    samples.send(MediaFrame::configFrame(3, parameters(false)));
    require(discontinuous.process() == NodeBase::Success, "audio config rejected");
    samples.send(MediaFrame::dataFrame(3, makeFrame(false, 0, 333)));
    require(discontinuous.process() == NodeBase::Success, "valid audio rejected");
    samples.send(MediaFrame::dataFrame(3, makeFrame(false, 1000, 333)));
    require(discontinuous.process() == NodeBase::InvalidDataFrame, "audio timestamp gap accepted");

    roundTrip<VideoEncoderNode, VideoDecoderNode>(true);
    roundTrip<AudioEncoderNode, AudioDecoderNode>(false);
    roundTrip<VideoEncoderNode, VideoDecoderNode>(true, true);
    roundTrip<AudioEncoderNode, AudioDecoderNode>(false, true);

    // Queue closure is cancellation and must not invent ENDFRAME.
    Graph cancelled;
    auto& enc = cancelled.addNode<VideoEncoderNode>();
    auto& dec = cancelled.addNode<VideoDecoderNode>();
    auto& sink = cancelled.addNode<FrameSink>();
    FrameSender source;
    source.connect(enc); cancelled.connect(enc, dec); cancelled.connect(dec, sink);
    source.send(MediaFrame::configFrame(2, parameters(true)));
    source.close();
    cancelled.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink.finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(sink.finished, "cancelled codec pipeline stalled");
    cancelled.stop();
    require(sink.messages.empty(), "cancelled decoder invented EOF");
}
