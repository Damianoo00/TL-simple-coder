#include "nodes/InputNode.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct WaveFile {
    std::filesystem::path directory, path;
    WaveFile() {
        auto pattern = (std::filesystem::temp_directory_path() / "async-input-XXXXXX").string();
        auto result = mkdtemp(pattern.data());
        require(result != nullptr, "Cannot create temporary directory");
        directory = result;
        path = directory / "input.wav";
        // Four signed 16-bit mono PCM samples, 8000 Hz; no external encoder needed.
        const unsigned char bytes[] = {
            'R','I','F','F', 44,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
            0x40,0x1f,0,0, 0x80,0x3e,0,0, 2,0, 16,0,
            'd','a','t','a', 8,0,0,0, 0,0, 1,0, 0xff,0x7f, 0,0x80
        };
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        require(static_cast<bool>(output), "Cannot write WAV fixture");
    }
    ~WaveFile() { std::error_code error; std::filesystem::remove_all(directory, error); }
};
class Collector : public NodeBase {
public:
    Collector() { addInput<MediaPacket>(); }
    int process() override { return Finished; }
    MediaPacket take() {
        auto value = input<MediaPacket>()->pop();
        require(value.has_value(), "Missing input message");
        return std::move(*value);
    }
    void close() { input<MediaPacket>()->close(); }
};
}

int main() {
    WaveFile fixture;
    InputNode input;
    Collector sink;
    input.connect(sink);
    require(input.configure({{"filename", ""}}) == NodeBase::InvalidConfiguration &&
            input.process() == NodeBase::InvalidConfiguration, "Invalid filename error was lost");
    require(input.configure({{"filename", (fixture.directory / "missing.wav").string()}}) == NodeBase::MediaError &&
            input.process() == NodeBase::MediaError, "Missing input error was lost");
    // The successful runs below also verify recovery after configuration failures.
    for (int run = 0; run < 2; ++run) {
        require(input.configure({{"filename", fixture.path.string()}}) == NodeBase::Success,
                "Valid WAV configuration failed");
        require(input.process() == NodeBase::Success, "Input did not send config first");
        auto config = sink.take();
        require(config.type == MediaPacket::Type::CONFIGFRAME && config.stream_id == 0 &&
                config.config && !config.packet, "Malformed input config");
        const auto& codec = *config.config->codec;
        require(codec.codec_type == AVMEDIA_TYPE_AUDIO && codec.codec_id == AV_CODEC_ID_PCM_S16LE &&
                codec.sample_rate == 8000 && codec.ch_layout.nb_channels == 1,
                "Input changed audio parameters");
        require(config.config->time_base.num == 1 && config.config->time_base.den == 8000,
                "Input changed time base");
        std::vector<unsigned char> samples;
        bool ended = false;
        for (int step = 0; step < 20 && !ended; ++step) {
            require(input.process() == NodeBase::Success, "Input failed before explicit EOF");
            auto message = sink.take();
            require(message.stream_id == 0 && !message.config, "Input changed stream ID or repeated config");
            if (message.type == MediaPacket::Type::ENDFRAME) {
                require(!message.packet, "EOF contains packet");
                ended = true;
            } else {
                require(message.type == MediaPacket::Type::DATAFRAME && message.packet &&
                        message.packet->size > 0, "Invalid input packet");
                samples.insert(samples.end(), message.packet->data,
                               message.packet->data + message.packet->size);
            }
        }
        require(ended, "Input did not send EOF");
        require(samples == std::vector<unsigned char>({0,0,1,0,0xff,0x7f,0,0x80}),
                "Input changed or lost PCM payload");
        require(input.process() == NodeBase::Finished && input.process() == NodeBase::Finished &&
                input.status() == NodeBase::Success, "Input completion is not stable");
    }
    for (const auto* value : {"true", "1"}) {
        InputNode looping;
        Collector collector;
        looping.connect(collector);
        require(looping.configure({{"filename", fixture.path.string()}, {"loop", value}}) == NodeBase::Success,
                "Loop configuration failed");
        require(looping.process() == NodeBase::Success &&
                collector.take().type == MediaPacket::Type::CONFIGFRAME, "Missing loop config");
        for (int iteration = 0; iteration < 5; ++iteration) {
            require(looping.process() == NodeBase::Success, "Loop stopped at EOF");
            auto message = collector.take();
            require(message.type == MediaPacket::Type::DATAFRAME && message.stream_id == 0,
                    "Loop repeated config or sent EOF");
            require(message.packet->pts == iteration * 4 && message.packet->dts == iteration * 4 &&
                    message.packet->duration == 4, "Loop timestamps are not continuous");
            require(message.packet->size == 8 && message.packet->data[5] == 0x7f,
                    "Loop changed payload");
        }
        collector.close();
        require(looping.process() == NodeBase::Finished, "Loop ignored closed output");
    }
    for (const auto* value : {"false", "0"}) {
        require(input.configure({{"filename", fixture.path.string()}, {"loop", value}}) == NodeBase::Success,
                "Disabled loop configuration failed");
    }
    require(input.configure({{"filename", fixture.path.string()}, {"loop", "yes"}}) ==
            NodeBase::InvalidConfiguration, "Invalid loop option accepted");
    {
        InputNode disconnected;
        require(disconnected.configure({{"filename", fixture.path.string()}}) == NodeBase::Success,
                "Input configuration failed");
        require(disconnected.process() == NodeBase::OutputNotConnected &&
                disconnected.process() == NodeBase::OutputNotConnected, "Disconnected output error was lost");
    }
    {
        InputNode cancelled;
        Collector closed;
        cancelled.connect(closed);
        require(cancelled.configure({{"filename", fixture.path.string()}}) == NodeBase::Success,
                "Input configuration failed");
        closed.close();
        require(cancelled.process() == NodeBase::Finished && cancelled.status() == NodeBase::Success,
                "Closed output was reported as media failure");
    }
}
