#include "nodes/VideoOverlayNode.hpp"
#include "core/Graph.hpp"
#include "api/JsonGraphParser.hpp"
#include <future>
#include <stdexcept>
#include <cstdio>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class Source : public NodeBase {
public:
    Source() { addOutput<MediaFrame>(); }
    int process() override { return Finished; }
    void send(MediaFrame message) { require(output<MediaFrame>()->push(std::move(message)), "source push failed"); }
};
class Sink : public NodeBase {
public:
    Sink() { addInput<MediaFrame>(); }
    ~Sink() override { stop(); }
    int process() override {
        auto message = input<MediaFrame>()->pop();
        if (!message) { done = true; return Finished; }
        messages.push_back(std::move(*message));
        return Success;
    }
    std::vector<MediaFrame> messages;
    std::atomic<bool> done{false};
};
void queueCancellation() {
    ThreadSafeQueue<int> queue;
    queue.setCapacity(1);
    require(queue.push(1), "initial push failed");
    auto blocked = std::async(std::launch::async, [&] { return queue.push(2); });
    require(blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout, "capacity did not block producer");
    queue.close();
    require(blocked.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !blocked.get(), "close did not unblock producer");
    require(queue.pop() == 1 && !queue.pop(), "closed queue did not drain");
}
void validationAndStartupFailure() {
    const std::string document = R"({"queue_capacity":8,"graph":[
        {"type":"VideoDecoderNode","name":"decoder","configuration":[],"connections":["overlay"]},
        {"type":"VideoOverlayNode","name":"overlay","configuration":[],"connections":["scaler"]},
        {"type":"VideoScalerNode","name":"scaler","configuration":[],"connections":[]}]})";
    JsonGraphParser::validateJson(document);
    auto audio = document;
    audio.replace(audio.find("VideoDecoderNode"), std::string("VideoDecoderNode").size(), "AudioDecoderNode");
    bool rejected = false;
    try { JsonGraphParser::validateJson(audio); } catch (const std::exception&) { rejected = true; }
    require(rejected, "parser allowed audio into overlay");
    BrowserRenderer browser;
    const auto started = std::chrono::steady_clock::now();
    require(!browser.start("/usr/bin/false", "unused", "http://test.invalid", 64, 48, 20, "cpu"), "dead renderer was accepted");
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "dead renderer did not fail promptly");
}
void composite() {
    Graph graph;
    auto& overlay = graph.addNode<VideoOverlayNode>();
    auto& sink = graph.addNode<Sink>();
    require(overlay.configure({{"url", "http://test.invalid/overlay"}, {"width", "64"}, {"height", "48"}, {"fps", "20"},
        {"renderer_executable", TEST_PYTHON}, {"renderer_script", FAKE_OVERLAY_SCRIPT}}) == NodeBase::Success, "fake renderer startup failed");
    Source source;
    source.connect(overlay); graph.connect(overlay, sink);
    auto codec = avcodec_parameters_alloc();
    require(codec, "codec allocation failed");
    std::shared_ptr<AVCodecParameters> parameters(codec, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    codec->codec_type = AVMEDIA_TYPE_VIDEO; codec->format = AV_PIX_FMT_BGRA;
    codec->width = 64; codec->height = 48; codec->color_range = AVCOL_RANGE_JPEG; codec->color_space = AVCOL_SPC_RGB;
    source.send(MediaFrame::configFrame(7, {parameters, {1, 1000}, {10, 1}}));
    for (int i = 0; i < 3; ++i) {
        auto raw = av_frame_alloc();
        require(raw, "frame allocation failed");
        AVFramePtr frame(raw, [](AVFrame* p) { av_frame_free(&p); });
        raw->width = 64; raw->height = 48; raw->format = AV_PIX_FMT_BGRA;
        raw->pts = i * 100; raw->duration = 100;
        require(av_frame_get_buffer(raw, 32) >= 0, "frame buffer allocation failed");
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
            auto pixel = raw->data[0] + y * raw->linesize[0] + x * 4;
            pixel[0] = 255; pixel[1] = pixel[2] = 0; pixel[3] = 255;
        }
        source.send(MediaFrame::dataFrame(7, frame));
    }
    source.send(MediaFrame::endFrame(7));
    const auto start = std::chrono::steady_clock::now();
    graph.start();
    while (!sink.done && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(sink.done, "overlay pipeline stalled"); graph.stop();
    require(overlay.status() == NodeBase::Success, "overlay failed");
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(240), "video was not paced in real time");
    require(sink.messages.size() == 8, "CFR output must contain config, six frames and EOF");
    require(sink.messages.front().config->time_base.den == 20 && sink.messages.front().config->frame_rate.num == 20,
            "incorrect output clock");
    for (int i = 0; i < 6; ++i) {
        const auto& message = sink.messages[i + 1];
        require(message.stream_id == 7 && message.frame->pts == i && message.frame->duration == 1, "timestamps or stream ID changed");
        const auto pixel = message.frame->data[0];
        require(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0, "transparent background changed source video");
        require(pixel[4] == 127 && pixel[5] == 0 && pixel[6] == 128, "premultiplied alpha blending is incorrect");
        require(pixel[8] == 0 && pixel[9] == 255 && pixel[10] == 0, "opaque overlay blending is incorrect");
    }
    require(sink.messages.back().type == MediaFrame::Type::ENDFRAME, "EOF missing");
}
int main() try { queueCancellation(); validationAndStartupFailure(); composite(); return 0; }
catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
