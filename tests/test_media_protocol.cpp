#include "nodes/InputNode.hpp"
#include "nodes/OutputNode.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

class Collector : public NodeBase {
public:
    Collector() { addInput<MediaPacket>(); }
    int process() override { return -1; }
    MediaPacket take() { return std::move(*input<MediaPacket>()->pop()); }
};

class Sender : public NodeBase {
public:
    Sender() { addOutput<MediaPacket>(); }
    int process() override { return -1; }
    void send(MediaPacket message) {
        if (!output<MediaPacket>()->push(std::move(message)))
            throw std::logic_error("Output queue unexpectedly closed");
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::logic_error(message);
}

int main(int argc, char** argv) {
    require(argc == 3, "Expected input and output MP4 paths");
    InputNode input;
    Collector collector;
    require(input.configure({{"filename", argv[1]}}) == NodeBase::Success, "Input configuration failed");
    input.connect(collector);

    std::vector<MediaPacket> messages;
    while (input.process() >= 0) {
        messages.push_back(collector.take());
    }
    require(messages.size() > 4, "Expected two streams with data");
    require(messages[0].type == MediaPacket::Type::CONFIGFRAME && messages[0].stream_id == 0,
            "First message must configure stream 0");
    require(messages[1].type == MediaPacket::Type::CONFIGFRAME && messages[1].stream_id == 1,
            "Second message must configure stream 1");
    const auto count = messages.size();
    require(messages[count - 2].type == MediaPacket::Type::ENDFRAME && messages[count - 2].stream_id == 0,
            "Expected EOF for stream 0");
    require(messages[count - 1].type == MediaPacket::Type::ENDFRAME && messages[count - 1].stream_id == 1,
            "Expected EOF for stream 1");
    bool seen[2] = {false, false};
    std::size_t transitions = 0;
    int previous = -1;
    for (std::size_t i = 2; i < count - 2; ++i) {
        const auto& message = messages[i];
        require(message.type == MediaPacket::Type::DATAFRAME, "Configurations and EOFs must surround data");
        require(message.stream_id == 0 || message.stream_id == 1, "Invalid stream ID");
        const auto& packet = message.packet;
        require(packet && packet->stream_index == message.stream_id, "Packet is assigned to the wrong stream");
        seen[message.stream_id] = true;
        if (previous != -1 && previous != message.stream_id) ++transitions;
        previous = message.stream_id;
    }
    require(seen[0] && seen[1] && transitions > 1, "Expected interleaved audio and video");

    // IDs need not be contiguous or equal to the AVPacket stream_index.
    Sender sender;
    OutputNode output;
    require(output.configure({{"filename", argv[2]}}) == NodeBase::Success, "Output configuration failed");
    sender.connect(output);
    for (std::size_t i = 0; i < count; ++i) {
        auto message = messages[i];
        message.stream_id = message.stream_id == 0 ? 101 : 7;
        sender.send(std::move(message));
        require(output.process() == (i + 1 == count ? -1 : 0),
                "Output must finish only after EOF for both streams");
    }

    unsigned rejected = 0;
    auto expectRejected = [&](std::vector<MediaPacket> sequence, int error) {
        Sender source;
        OutputNode sink;
        require(sink.configure({{"filename", std::string(argv[2]) + ".invalid-" + std::to_string(rejected++)}})
                == NodeBase::Success, "Invalid sequence sink configuration failed");
        source.connect(sink);
        for (auto& message : sequence) {
            source.send(std::move(message));
            const int result = sink.process();
            if (result < NodeBase::Finished) {
                require(result == error, "Unexpected protocol error code");
                require(sink.status() == error && sink.process() == error, "Error must remain terminal");
                return;
            }
        }
        throw std::logic_error("Invalid message sequence was accepted");
    };
    expectRejected({messages[2]}, NodeBase::FrameBeforeConfig);
    expectRejected({messages[0], messages[0]}, NodeBase::DuplicateStreamId);
    expectRejected({messages[count - 1]}, NodeBase::FrameBeforeConfig);
    expectRejected({messages[0], messages[1], messages[count - 2], messages[count - 2]},
                   NodeBase::FrameAfterEnd);
    expectRejected({messages[0], messages[1], messages[2], messages[0]},
                   NodeBase::LateConfigFrame);
    auto data = messages[2];
    const auto eof = MediaPacket::endFrame(data.stream_id);
    expectRejected({messages[0], messages[1], eof, data}, NodeBase::FrameAfterEnd);

    // Configured streams with no packets still have explicit EOF messages.
    Sender empty_source;
    OutputNode empty_output;
    require(empty_output.configure({{"filename", std::string(argv[2]) + ".empty"}})
            == NodeBase::Success, "Empty output configuration failed");
    empty_source.connect(empty_output);
    for (auto index : {std::size_t(0), std::size_t(1), count - 2, count - 1}) {
        empty_source.send(messages[index]);
        require(empty_output.process() == (index == count - 1 ? -1 : 0), "Empty stream EOF failed");
    }
}
