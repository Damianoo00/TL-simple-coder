#include "core/Graph.hpp"
#include "nodes/VideoGapFillerNode.hpp"
#include "nodes/AudioGapFillerNode.hpp"
#include "nodes/AudioResamplerNode.hpp"
#include "api/JsonGraphParser.hpp"
#include <nlohmann/json.hpp>
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

void audio(bool resample) {
    Graph graph;
    auto& filler = graph.addNode<AudioGapFillerNode>();
    auto& sink = graph.addNode<Sink>();
    Source source;
    source.connect(filler);
    require(filler.configure({}) == NodeBase::Success, "audio configure failed");
    if (resample) {
        auto& converter = graph.addNode<AudioResamplerNode>();
        require(converter.configure({{"sample_rate", "48000"}}) == NodeBase::Success, "resampler config failed");
        graph.connect(filler, converter); graph.connect(converter, sink);
    } else graph.connect(filler, sink);
    auto first = frame(false, 100, 32);
    auto second = frame(false, 232, 16);
    source.send(MediaFrame::configFrame(7, parameters(false)));
    source.send(MediaFrame::dataFrame(7, first));
    source.send(MediaFrame::dataFrame(7, second));
    source.send(MediaFrame::endFrame(7));
    run(graph, sink);
    require(filler.status() == NodeBase::Success, "audio filling failed");
    require(first->pts == 100 && second->pts == 232, "source mutated");
    require(sink.messages.front().type == MediaFrame::Type::CONFIGFRAME &&
            sink.messages.back().type == MediaFrame::Type::ENDFRAME, "audio protocol broken");
    if (!resample) {
        require(sink.messages.size() == 5, "wrong audio message count");
        const auto& silence = *sink.messages[2].frame;
        require(silence.pts == 132 && silence.nb_samples == 100 && silence.duration == 100,
                "wrong silence timing");
        auto samples = reinterpret_cast<const int16_t*>(silence.data[0]);
        require(std::all_of(samples, samples + 100, [](auto v) { return v == 0; }), "silence is audible");
        require(reinterpret_cast<int16_t*>(sink.messages[3].frame->data[0])[0] == 8192,
                "audio payload changed");
    }
}
void video() {
    Graph graph;
    auto& filler = graph.addNode<VideoGapFillerNode>();
    auto& sink = graph.addNode<Sink>();
    Source source; source.connect(filler); graph.connect(filler, sink);
    require(filler.configure({}) == NodeBase::Success, "video configure failed");
    auto first = frame(true, 100); auto second = frame(true, 205);
    second->data[0][0] = 99;
    source.send(MediaFrame::configFrame(9, parameters(true)));
    source.send(MediaFrame::dataFrame(9, first));
    source.send(MediaFrame::dataFrame(9, second));
    source.send(MediaFrame::endFrame(9));
    run(graph, sink);
    require(filler.status() == NodeBase::Success && sink.messages.size() == 6, "video filling failed");
    require(sink.messages[2].frame->pts == 142 && sink.messages[2].frame->duration == 42 &&
            sink.messages[3].frame->pts == 184 && sink.messages[3].frame->duration == 21,
            "wrong repeat timestamps");
    require(sink.messages[2].frame->data[0][0] == 255 && sink.messages[3].frame->data[0][0] == 255 &&
            sink.messages[4].frame->data[0][0] == 99, "wrong repeated image");
    require(first->pts == 100 && first->duration == 42 && second->pts == 205, "video input mutated");
}
template<class T> void protocol(bool is_video) {
    for (bool eof : {false, true}) {
        Graph graph; auto& filler = graph.addNode<T>(); auto& sink = graph.addNode<Sink>();
        Source source; source.connect(filler); graph.connect(filler, sink);
        require(filler.configure({}) == NodeBase::Success, "configure failed");
        source.send(MediaFrame::configFrame(3, parameters(is_video)));
        if (eof) source.send(MediaFrame::endFrame(3)); else source.close();
        run(graph, sink);
        require(sink.messages.size() == (eof ? 2u : 1u), "empty/cancelled stream generated data or EOF");
    }
    for (int mode = 0; mode < 4; ++mode) {
        T filler; Source source; Sink sink; source.connect(filler); filler.connect(sink);
        require(filler.configure({{"max_gap_ms", "10"}}) == NodeBase::Success, "limit configuration failed");
        source.send(MediaFrame::configFrame(0, parameters(is_video)));
        source.send(MediaFrame::dataFrame(0, frame(is_video, 100, 32)));
        auto bad = frame(is_video, mode == 0 ? 100000 : 50, 32);
        if (mode == 2) bad->pts = AV_NOPTS_VALUE;
        if (mode == 3) bad->format = -1;
        source.send(MediaFrame::dataFrame(0, bad));
        require(filler.process() == NodeBase::Success && filler.process() == NodeBase::Success,
                "valid frame rejected");
        require(filler.process() == NodeBase::InvalidDataFrame && filler.process() == NodeBase::InvalidDataFrame,
                "invalid frame accepted or error lost");
    }
    T options;
    require(options.configure({{"max_gap_ms", "0"}}) == NodeBase::InvalidConfiguration &&
            options.configure({{"other", "1"}}) == NodeBase::InvalidConfiguration, "invalid option accepted");
}

void parser() {
    using J = nlohmann::json;
    for (const auto* kind : {"AudioGapFillerNode", "VideoGapFillerNode"}) {
        const bool audio = std::string(kind) == "AudioGapFillerNode";
        J document = {{"graph", J::array({
            {{"name", "fill"}, {"type", kind}, {"configuration", J::array()}, {"connections", J::array({"encode"})}},
            {{"name", "encode"}, {"type", audio ? "AudioEncoderNode" : "VideoEncoderNode"},
             {"configuration", J::array()}, {"connections", J::array()}}
        })}};
        JsonGraphParser::validateJson(document.dump());
        document["graph"][1]["type"] = audio ? "VideoEncoderNode" : "AudioEncoderNode";
        bool rejected = false;
        try { JsonGraphParser::validateJson(document.dump()); } catch (const std::exception&) { rejected = true; }
        require(rejected, "parser accepted incompatible filler output");
    }
}
void unsignedSilence() {
    AudioGapFillerNode filler; Source source; Sink sink;
    source.connect(filler); filler.connect(sink); filler.configure({});
    auto config = parameters(false);
    config.codec->format = AV_SAMPLE_FMT_U8;
    auto make = [](int64_t pts) {
        AVFramePtr f(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
        f->format = AV_SAMPLE_FMT_U8; f->sample_rate = 44100; f->nb_samples = 2; f->pts = pts;
        av_channel_layout_default(&f->ch_layout, 1);
        require(av_frame_get_buffer(f.get(), 0) >= 0, "U8 allocation failed");
        std::fill_n(f->data[0], 2, 200); return f;
    };
    source.send(MediaFrame::configFrame(0, config));
    source.send(MediaFrame::dataFrame(0, make(0)));
    source.send(MediaFrame::dataFrame(0, make(5)));
    require(filler.process() == NodeBase::Success && filler.process() == NodeBase::Success &&
            filler.process() == NodeBase::Success, "U8 filling failed");
    sink.process(); sink.process(); sink.process();
    auto f = sink.messages[2].frame;
    require(f->pts == 2 && f->nb_samples == 3 && f->data[0][0] == 128 && f->data[0][2] == 128,
            "unsigned silence must use midpoint");
}
template<class T> void blockedCancellation(bool video) {
    Graph graph; auto& filler = graph.addNode<T>();
    Source source; Sink sink; source.connect(filler); filler.connect(sink);
    sink.setQueueCapacity(1);
    filler.configure({});
    source.send(MediaFrame::configFrame(0, parameters(video)));
    source.send(MediaFrame::dataFrame(0, frame(video, 0, 32)));
    source.send(MediaFrame::dataFrame(0, frame(video, 4000, 32)));
    graph.start();
    // The sink deliberately has no thread: filler blocks on its bounded queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    graph.stop();
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "filler stop blocked");
}
int main() {
    audio(false); audio(true); video(); unsignedSilence(); parser();
    blockedCancellation<AudioGapFillerNode>(false); blockedCancellation<VideoGapFillerNode>(true);
    protocol<AudioGapFillerNode>(false); protocol<VideoGapFillerNode>(true);
}
