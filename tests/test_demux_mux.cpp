#include "core/Graph.hpp"
#include "nodes/DemuxNode.hpp"
#include "nodes/MuxerNode.hpp"
#include "nodes/InputNode.hpp"
#include "nodes/OutputNode.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

void require(bool condition, const char* message) {
    if (!condition) throw std::logic_error(message);
}

class Sender : public NodeBase {
public:
    Sender() { addOutput<MediaPacket>(); }
    int process() override { return -1; }
    void send(MediaPacket frame) { require(output<MediaPacket>()->push(std::move(frame)), "Push failed"); }
};
class Collector : public NodeBase {
public:
    Collector() { addInput<MediaPacket>(); }
    ~Collector() override { stop(); }
    int process() override {
        auto message = input<MediaPacket>()->pop();
        if (!message) { finished = true; return -1; }
        messages.push_back(std::move(*message));
        ++message_count;
        return 0;
    }
    MediaPacket take() { return std::move(*input<MediaPacket>()->pop()); }
    std::atomic<bool> finished{false};
    std::atomic<std::size_t> message_count{0};
    std::vector<MediaPacket> messages;
};

MediaStream config(AVRational time_base) {
    auto codec = avcodec_parameters_alloc();
    if (!codec) throw std::bad_alloc();
    return {std::shared_ptr<AVCodecParameters>(codec,
             [](AVCodecParameters* p) { avcodec_parameters_free(&p); }), time_base};
}
MediaPacket data(int id, int64_t dts) {
    auto packet = makeAVPacket();
    packet->dts = packet->pts = dts;
    return MediaPacket::dataFrame(id, std::move(packet));
}

int main() {
    InputNode unconfigured_input;
    OutputNode unconfigured_output;
    require(unconfigured_input.process() == NodeBase::NotConfigured, "Unconfigured input accepted");
    require(unconfigured_output.process() == NodeBase::NotConfigured, "Unconfigured output accepted");
    require(unconfigured_input.configure({}) == NodeBase::InvalidConfiguration, "Missing input filename accepted");
    require(unconfigured_output.configure({{"filename", ""}}) == NodeBase::InvalidConfiguration, "Empty output filename accepted");
    require(unconfigured_input.configure({{"filename", "/nonexistent/async-nodes-test.mp4"}})
            == NodeBase::MediaError, "Missing media file accepted");
    require(unconfigured_output.configure({{"filename", "/nonexistent/async-nodes-test/output.mp4"}})
            == NodeBase::MediaError, "Unwritable output path accepted");
    require(unconfigured_output.process() == NodeBase::MediaError, "Output configuration error was lost");
    DemuxNode invalid_demux(0);
    require(invalid_demux.status() == NodeBase::InvalidPorts, "Zero demux ports accepted");
    require(invalid_demux.configure({{"ports", "bad"}}) == NodeBase::InvalidPorts, "Invalid demux ports accepted");
    require(invalid_demux.configure({{"ports", "1"}}) == NodeBase::Success, "Demux reconfiguration failed");
    Sender bad_source;
    bad_source.connect(invalid_demux);
    bad_source.send(MediaPacket::endFrame(7));
    require(invalid_demux.process() == NodeBase::FrameBeforeConfig, "Unconfigured demux stream accepted");
    require(invalid_demux.process() == NodeBase::FrameBeforeConfig, "Demux error was not terminal");

    // The first configured ID goes to port 0, regardless of its numeric value.
    Sender sender;
    DemuxNode demux(2);
    Collector first, second;
    sender.connect(demux);
    demux.connect(first, 0);
    demux.connect(second, 1);
    sender.send(MediaPacket::configFrame(42, config({1, 1000})));
    require(demux.process() == 0 && first.take().stream_id == 42, "Incorrect first stream routing");
    sender.send(MediaPacket::configFrame(7, config({1, 100})));
    require(demux.process() == 0 && second.take().stream_id == 7, "Incorrect second stream routing");
    auto original = data(7, 3);
    auto buffer = original.packet;
    sender.send(std::move(original));
    require(demux.process() == 0 && second.take().packet == buffer, "Demux changed packet ownership");
    sender.send(MediaPacket::endFrame(42));
    require(demux.process() == 0 && first.take().type == MediaPacket::Type::ENDFRAME, "First EOF failed");
    sender.send(MediaPacket::endFrame(7));
    require(demux.process() == -1 && second.take().stream_id == 7, "Last EOF failed");

    // A complete split/merge round trip, including an unused third port.
    Graph graph;
    auto& split = graph.addNode<DemuxNode>(3);
    auto& merge = graph.addNode<MuxerNode>(3);
    auto& sink = graph.addNode<Collector>();
    Sender source;
    source.connect(split);
    for (std::size_t i = 0; i < 3; ++i) graph.connect(split, merge, i, i);
    graph.connect(merge, sink);
    source.send(MediaPacket::configFrame(42, config({1, 1000})));
    source.send(MediaPacket::configFrame(7, config({1, 100})));
    // Raw DTS 10 < 20, but actual time 100ms > 20ms.
    source.send(data(42, 20));
    source.send(data(7, 10));
    source.send(data(42, 120));
    source.send(MediaPacket::endFrame(7));
    source.send(MediaPacket::endFrame(42));
    graph.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sink.finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(sink.finished, "Split/merge stalled on EOF or unused port");
    graph.stop();
    const auto& result = sink.messages;
    require(result.size() == 7, "Split/merge lost messages");
    require(result[0].type == MediaPacket::Type::CONFIGFRAME && result[1].type == MediaPacket::Type::CONFIGFRAME,
            "Configurations must precede all data");
    require(result[2].stream_id == 42 && result[2].packet->dts == 20 &&
            result[3].stream_id == 7 && result[3].packet->dts == 10 &&
            result[4].stream_id == 42 && result[4].packet->dts == 120, "Incorrect DTS interleaving");
    require(result[5].type == MediaPacket::Type::ENDFRAME && result[6].type == MediaPacket::Type::ENDFRAME,
            "EOF must follow all data");

    // Distinct ports must not claim the same stream ID.
    MuxerNode duplicate(2);
    Sender left, right;
    Collector target;
    left.connect(duplicate, 0, 0);
    right.connect(duplicate, 0, 1);
    duplicate.connect(target);
    left.send(MediaPacket::configFrame(42, config({1, 1000})));
    right.send(MediaPacket::configFrame(42, config({1, 1000})));
    require(duplicate.process() == MuxerNode::DuplicateStreamId, "Mux accepted duplicate stream IDs");
    require(duplicate.process() == MuxerNode::DuplicateStreamId, "Mux lost terminal error");

    MuxerNode invalid(0);
    require(invalid.status() == MuxerNode::InvalidPorts, "Zero ports accepted");
    require(invalid.process() == MuxerNode::InvalidPorts, "Invalid construction was ignored");
    for (const auto* value : {"0", "-1", "abc", "2x", "", "999999999999999999999999"})
        require(invalid.configureChecked({{"ports", value}}) == MuxerNode::InvalidPorts,
                "Invalid port configuration accepted");
    require(invalid.configureChecked({{"ports", "1"}}) == MuxerNode::Success,
            "Valid reconfiguration failed");
    Sender isolated_source;
    isolated_source.connect(invalid);
    isolated_source.send(MediaPacket::configFrame(1, config({1, 1000})));
    require(invalid.process() == MuxerNode::OutputNotConnected, "Missing output accepted");


    // Protocol errors return distinct codes and remain terminal.
    const auto check_frame_error = [](MediaPacket frame, int expected, bool initial) {
        MuxerNode mux(1);
        Sender source;
        Collector sink;
        source.connect(mux);
        mux.connect(sink);
        if (!initial) {
            source.send(MediaPacket::configFrame(42, config({1, 1000})));
            require(mux.process() == MuxerNode::Success, "Valid config rejected");
            require(sink.take().stream_id == 42, "Config not forwarded");
        }
        source.send(std::move(frame));
        require(mux.process() == expected, "Incorrect frame error code");
        require(mux.status() == expected && mux.process() == expected,
                "Protocol error did not remain terminal");
    };
    check_frame_error(data(42, 0), MuxerNode::InvalidConfigFrame, true);
    check_frame_error(MediaPacket::configFrame(42, config({0, 1000})),
                      MuxerNode::InvalidConfigFrame, true);
    check_frame_error(MediaPacket::configFrame(-1, config({1, 1000})),
                      MuxerNode::InvalidConfigFrame, true);
    check_frame_error(data(7, 0), MuxerNode::StreamIdChanged, false);
    check_frame_error(MediaPacket::dataFrame(42, nullptr), MuxerNode::InvalidDataFrame, false);
    check_frame_error(MediaPacket::configFrame(42, config({1, 1000})),
                      MuxerNode::InvalidDataFrame, false);
    auto invalid_end = MediaPacket::endFrame(42);
    invalid_end.packet = makeAVPacket();
    check_frame_error(std::move(invalid_end), MuxerNode::InvalidEndFrame, false);

    // Stop wakes a mux waiting for a stream's next packet without inventing EOF.
    Graph partial;
    auto& partial_mux = partial.addNode<MuxerNode>(1);
    auto& partial_sink = partial.addNode<Collector>();
    Sender partial_source;
    partial_source.connect(partial_mux);
    partial.connect(partial_mux, partial_sink);
    partial_source.send(MediaPacket::configFrame(99, config({1, 1000})));
    partial_source.send(data(99, 0));
    partial.start();
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (partial_sink.message_count < 2 && std::chrono::steady_clock::now() < stop_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(partial_sink.message_count == 2, "Partial mux did not forward config and data");
    partial.stop();
    require(partial_sink.finished && partial_sink.messages.size() == 2,
            "Stop must terminate the mux without synthesizing ENDFRAME");

}
