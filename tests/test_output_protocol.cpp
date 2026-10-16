#include "nodes/OutputNode.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "async-output-XXXXXX").string();
        auto result = mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Cannot create temporary directory");
        path = result;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
class Sender : public NodeBase {
public:
    Sender() { addOutput<MediaPacket>(); }
    int process() override { return Finished; }
    void send(MediaPacket packet) { require(output<MediaPacket>()->push(std::move(packet)), "Cannot send packet"); }
    void close() { output<MediaPacket>()->close(); }
};
MediaStream stream() {
    auto raw = avcodec_parameters_alloc();
    if (!raw) throw std::bad_alloc();
    std::shared_ptr<AVCodecParameters> codec(raw, [](auto* p) { avcodec_parameters_free(&p); });
    raw->codec_type = AVMEDIA_TYPE_VIDEO;
    raw->codec_id = AV_CODEC_ID_MPEG4;
    raw->format = AV_PIX_FMT_YUV420P;
    raw->width = raw->height = 16;
    return {std::move(codec), {1, 25}, {25, 1}};
}
struct Fixture {
    TemporaryDirectory directory;
    Sender source;
    OutputNode node;
    Fixture() {
        require(node.configure({{"filename", (directory.path / "output.mp4").string()}}) == NodeBase::Success,
                "Cannot configure output");
        source.connect(node);
    }
    int send(MediaPacket packet) { source.send(std::move(packet)); return node.process(); }
    void configureStream(int id = 5) {
        require(send(MediaPacket::configFrame(id, stream())) == NodeBase::Success, "Valid stream rejected");
    }
    void rejected(MediaPacket packet, int error) {
        require(send(std::move(packet)) == error, "Unexpected protocol error");
        require(node.status() == error && node.process() == error, "Protocol error is not terminal");
    }
};
}

int main() {
    int passed = 0;
    auto test = [&](const char* name, auto action) {
        try { action(); ++passed; }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
    };
    test("negative stream id", [] {
        Fixture f; f.rejected(MediaPacket::endFrame(-1), NodeBase::InvalidStreamId);
    });
    test("data before config", [] {
        Fixture f; f.rejected(MediaPacket::dataFrame(5, makeAVPacket()), NodeBase::FrameBeforeConfig);
    });
    test("end before config", [] {
        Fixture f; f.rejected(MediaPacket::endFrame(5), NodeBase::FrameBeforeConfig);
    });
    test("unknown frame type", [] {
        Fixture f;
        f.rejected({static_cast<MediaPacket::Type>(99), 5, nullptr, nullptr}, NodeBase::UnknownFrameType);
    });
    for (int mutation = 0; mutation < 5; ++mutation) {
        test("invalid config payload", [=] {
            Fixture f;
            auto parameters = stream();
            if (mutation == 0) parameters.codec.reset();
            if (mutation == 1) parameters.time_base.num = 0;
            if (mutation == 2) parameters.time_base.den = -1;
            auto packet = MediaPacket::configFrame(5, std::move(parameters));
            if (mutation == 3) packet.config.reset();
            if (mutation == 4) packet.packet = makeAVPacket();
            f.rejected(std::move(packet), NodeBase::InvalidConfigFrame);
        });
    }
    test("duplicate config", [] {
        Fixture f; f.configureStream();
        f.rejected(MediaPacket::configFrame(5, stream()), NodeBase::DuplicateStreamId);
    });
    for (bool withConfig : {false, true}) {
        test("invalid data payload", [=] {
            Fixture f; f.configureStream();
            auto packet = MediaPacket::dataFrame(5, withConfig ? makeAVPacket() : nullptr);
            if (withConfig) packet.config = std::make_shared<const MediaStream>(stream());
            f.rejected(std::move(packet), NodeBase::InvalidDataFrame);
        });
        test("invalid end payload", [=] {
            Fixture f; f.configureStream();
            auto packet = MediaPacket::endFrame(5);
            if (withConfig) packet.config = std::make_shared<const MediaStream>(stream());
            else packet.packet = makeAVPacket();
            f.rejected(std::move(packet), NodeBase::InvalidEndFrame);
        });
    }
    for (int mutation = 0; mutation < 3; ++mutation) {
        test("messages after first stream ended", [=] {
            Fixture f; f.configureStream(5); f.configureStream(8);
            require(f.send(MediaPacket::endFrame(5)) == NodeBase::Success, "First end stopped other stream");
            if (mutation == 0)
                f.rejected(MediaPacket::endFrame(5), NodeBase::FrameAfterEnd);
            if (mutation == 1)
                f.rejected(MediaPacket::dataFrame(5, makeAVPacket()), NodeBase::FrameAfterEnd);
            if (mutation == 2)
                f.rejected(MediaPacket::configFrame(9, stream()), NodeBase::LateConfigFrame);
        });
    }
    test("all streams must end before completion", [] {
        Fixture f; f.configureStream(5); f.configureStream(8);
        require(f.send(MediaPacket::endFrame(8)) == NodeBase::Success, "Ended prematurely");
        require(f.send(MediaPacket::endFrame(5)) == NodeBase::Finished, "Did not finalize after last end");
        require(f.node.status() == NodeBase::Success, "Finalization failed");
        require(std::filesystem::file_size(f.directory.path / "output.mp4") > 0, "Empty output file");
    });
    for (bool configuredStream : {false, true}) {
        test("queue closure finalizes without end message", [=] {
            Fixture f;
            if (configuredStream) f.configureStream();
            f.source.close();
            require(f.node.process() == NodeBase::Finished && f.node.status() == NodeBase::Success,
                    "Queue closure failed");
        });
    }
    test("successful reconfiguration resets previous streams", [] {
        Fixture f; f.configureStream();
        require(f.node.configure({{"filename", (f.directory.path / "second.mp4").string()}}) == NodeBase::Success,
                "Reconfiguration failed");
        f.configureStream(); // Same ID must be permitted in the new output.
        require(f.send(MediaPacket::endFrame(5)) == NodeBase::Finished, "New output did not complete");
    });
    std::cout << passed << " output protocol cases passed\n";
}
