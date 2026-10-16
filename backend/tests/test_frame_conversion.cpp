#include "core/Graph.hpp"
#include "nodes/VideoScalerNode.hpp"
#include "nodes/AudioResamplerNode.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::logic_error(message); }
class Source : public NodeBase {
public:
    Source() { addOutput<MediaFrame>(); }
    int process() override { return Finished; }
    void send(MediaFrame message) { require(output<MediaFrame>()->push(std::move(message)), "push failed"); }
    void close() { output<MediaFrame>()->close(); }
};
class Sink : public NodeBase {
public:
    Sink() { addInput<MediaFrame>(); }
    ~Sink() override { stop(); }
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
    require(raw != nullptr, "parameters allocation failed");
    std::shared_ptr<AVCodecParameters> owned(raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    raw->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    raw->codec_id = video ? AV_CODEC_ID_RAWVIDEO : AV_CODEC_ID_PCM_S16LE;
    raw->format = video ? static_cast<int>(AV_PIX_FMT_RGB24) : static_cast<int>(AV_SAMPLE_FMT_S16);
    raw->width = 64; raw->height = 48;
    raw->sample_aspect_ratio = {1, 1};
    raw->color_space = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    raw->sample_rate = 44100;
    av_channel_layout_default(&raw->ch_layout, 1);
    return {std::move(owned), video ? AVRational{1, 1000} : AVRational{1, 44100}, video ? AVRational{24, 1} : AVRational{0, 1}};
}
AVFramePtr frame(bool video, int64_t pts, int samples = 0) {
    auto raw = av_frame_alloc();
    require(raw != nullptr, "frame allocation failed");
    AVFramePtr owned(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->format = video ? static_cast<int>(AV_PIX_FMT_RGB24) : static_cast<int>(AV_SAMPLE_FMT_S16);
    raw->width = 64; raw->height = 48;
    raw->sample_rate = 44100;
    raw->nb_samples = samples;
    raw->pts = pts; raw->duration = video ? 42 : samples;
    raw->sample_aspect_ratio = {1, 1};
    raw->colorspace = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    av_channel_layout_default(&raw->ch_layout, 1);
    require(av_frame_get_buffer(raw, 32) >= 0, "frame buffer failed");
    if (video) {
        for (int y = 0; y < raw->height; ++y)
            for (int x = 0; x < raw->width; ++x) {
                auto pixel = raw->data[0] + y * raw->linesize[0] + x * 3;
                pixel[0] = 255; pixel[1] = pixel[2] = 0;
            }
    } else std::fill_n(reinterpret_cast<int16_t*>(raw->data[0]), samples, 8192);
    return owned;
}
void run(Graph& graph, Sink& sink) {
    graph.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink.finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(sink.finished, "frame conversion stalled");
    graph.stop();
}
void scale(int height = 24) {
    Graph graph;
    auto& scaler = graph.addNode<VideoScalerNode>();
    auto& sink = graph.addNode<Sink>();
    require(scaler.configure({{"width", "32"}, {"height", std::to_string(height)}}) == NodeBase::Success, "scaler configure failed");
    Source source;
    source.connect(scaler); graph.connect(scaler, sink);
    auto original = frame(true, 77);
    source.send(MediaFrame::configFrame(9, parameters(true)));
    source.send(MediaFrame::dataFrame(9, original));
    source.send(MediaFrame::endFrame(9));
    run(graph, sink);
    require(scaler.status() == NodeBase::Success, "scaling failed");
    require(sink.messages.size() == 3 && sink.messages.front().type == MediaFrame::Type::CONFIGFRAME &&
            sink.messages.back().type == MediaFrame::Type::ENDFRAME, "scaler lost config/data/EOF");
    const auto& config = *sink.messages.front().config;
    require(config.codec->width == 32 && config.codec->height == height && config.codec->format == AV_PIX_FMT_YUV420P,
            "incorrect scaled configuration");
    require(av_cmp_q(config.codec->sample_aspect_ratio, AVRational{height, 24}) == 0,
            "scaler changed display aspect ratio");
    require(config.time_base.num == 1 && config.time_base.den == 1000 && config.frame_rate.num == 24,
            "scaler changed time base or frame rate");
    const auto& output = *sink.messages[1].frame;
    require(output.width == 32 && output.height == height && output.format == AV_PIX_FMT_YUV420P,
            "incorrect scaled frame format");
    require(output.pts == 77 && output.duration == 42 && sink.messages[1].stream_id == 9,
            "scaler changed timing or ID");
    require(std::abs(static_cast<int>(output.data[0][0]) - 81) <= 3 &&
            std::abs(static_cast<int>(output.data[1][0]) - 90) <= 3 &&
            std::abs(static_cast<int>(output.data[2][0]) - 240) <= 3, "RGB to YUV color conversion failed");
    require(original->format == AV_PIX_FMT_RGB24 && original->width == 64 && original->data[0][0] == 255,
            "scaler mutated input frame");
}
void resample(bool empty = false, bool cancelled = false) {
    Graph graph;
    auto& resampler = graph.addNode<AudioResamplerNode>();
    auto& sink = graph.addNode<Sink>();
    require(resampler.configure({{"sample_rate", "48000"}, {"channel_layout", "stereo"}}) == NodeBase::Success,
            "resampler configure failed");
    Source source;
    source.connect(resampler); graph.connect(resampler, sink);
    source.send(MediaFrame::configFrame(7, parameters(false)));
    int total = 0;
    if (!empty)
        for (const int samples : {137, 333, 1024, 17}) {
            source.send(MediaFrame::dataFrame(7, frame(false, 44100 + total, samples)));
            total += samples;
        }
    if (cancelled) source.close();
    else source.send(MediaFrame::endFrame(7));
    run(graph, sink);
    require(resampler.status() == NodeBase::Success, "resampling failed");
    const auto& config = *sink.messages.front().config;
    require(config.codec->sample_rate == 48000 && config.codec->format == AV_SAMPLE_FMT_FLTP &&
            config.codec->ch_layout.nb_channels == 2 && config.time_base.num == 1 && config.time_base.den == 48000,
            "incorrect resampled configuration");
    int64_t output_samples = 0;
    for (const auto& message : sink.messages) {
        require(message.stream_id == 7, "resampler changed ID");
        if (message.type != MediaFrame::Type::DATAFRAME) continue;
        const auto& converted = *message.frame;
        require(converted.sample_rate == 48000 && converted.format == AV_SAMPLE_FMT_FLTP &&
                converted.ch_layout.nb_channels == 2 && converted.nb_samples > 0, "incorrect resampled frame");
        require(converted.pts == 48000 + output_samples && converted.duration == converted.nb_samples,
                "resampled timestamps are discontinuous or wrong");
        for (int i = 0; i < converted.nb_samples; ++i) {
            const auto left = reinterpret_cast<float*>(converted.extended_data[0])[i];
            const auto right = reinterpret_cast<float*>(converted.extended_data[1])[i];
            require(std::isfinite(left) && left > 0.05f && std::abs(left - right) < 0.00001f,
                    "sample format/channel conversion failed");
        }
        output_samples += converted.nb_samples;
    }
    if (cancelled) require(sink.messages.back().type != MediaFrame::Type::ENDFRAME, "cancellation invented EOF");
    else {
        require(sink.messages.back().type == MediaFrame::Type::ENDFRAME, "resampler lost EOF");
        const auto expected = av_rescale_rnd(total, 48000, 44100, AV_ROUND_NEAR_INF);
        require(std::abs(output_samples - expected) <= 1, "resampler lost delayed samples at EOF");
    }
    if (empty) require(sink.messages.size() == 2, "empty resampler generated audio");
}
int main() {
    VideoScalerNode scaler;
    AudioResamplerNode resampler;
    require(scaler.configure({{"width", "0"}}) == NodeBase::InvalidConfiguration, "zero width accepted");
    require(scaler.configure({{"pixel_format", "bad"}}) == NodeBase::InvalidConfiguration, "bad pixel format accepted");
    require(scaler.configure({{"algorithm", "bad"}}) == NodeBase::InvalidConfiguration, "bad interpolation accepted");
    require(resampler.configure({{"sample_rate", "-1"}}) == NodeBase::InvalidConfiguration, "negative sample rate accepted");
    require(resampler.configure({{"sample_format", "bad"}}) == NodeBase::InvalidConfiguration, "bad sample format accepted");
    require(resampler.configure({{"channel_layout", "bad"}}) == NodeBase::InvalidConfiguration, "bad layout accepted");
    require(scaler.configure({}) == NodeBase::Success && resampler.configure({}) == NodeBase::Success, "converter recovery failed");
    require(scaler.process() == NodeBase::OutputNotConnected, "disconnected scaler accepted");
    Source source;
    Sink target;
    source.connect(resampler); resampler.connect(target);
    source.send(MediaFrame::configFrame(1, parameters(true)));
    require(resampler.process() == NodeBase::InvalidConfigFrame && resampler.process() == NodeBase::InvalidConfigFrame,
            "wrong media type accepted or terminal error lost");
    const auto reject = [](MediaFrame message, int expected) {
        VideoScalerNode converter;
        Source input;
        Sink output;
        input.connect(converter); converter.connect(output);
        input.send(MediaFrame::configFrame(9, parameters(true)));
        require(converter.process() == NodeBase::Success, "valid scaler configuration rejected");
        input.send(std::move(message));
        require(converter.process() == expected && converter.process() == expected,
                "converter accepted invalid protocol or lost terminal error");
    };
    reject(MediaFrame::dataFrame(9, nullptr), NodeBase::InvalidDataFrame);
    reject(MediaFrame::endFrame(10), NodeBase::StreamIdChanged);
    auto invalid_end = MediaFrame::endFrame(9);
    invalid_end.frame = frame(true, 0);
    reject(std::move(invalid_end), NodeBase::InvalidEndFrame);
    scale();
    scale(48);
    resample();
    resample(true);
    resample(false, true);
}
