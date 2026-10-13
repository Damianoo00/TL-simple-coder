#pragma once

#include "core/Node.hpp"
#include "FFmpeg.hpp"

#include <string>
#include <unordered_map>

class OutputNode : public NodeBase {
public:
    OutputNode() { addInput<MediaPacket>(); }
    ~OutputNode() override;

    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    const char* nodeType() const override { return "OutputNode"; }
    int finishMedia(const char* reason = "cleanup");
    std::size_t m_packets_written = 0;
    AVFormatContext* m_media = nullptr;
    struct StreamState {
        AVRational time_base;
        int output_index;
        bool ended = false;
    };
    std::unordered_map<int, StreamState> m_streams;
    std::size_t m_ended_streams = 0;
    int writeHeader();
    bool m_header_written = false;
    std::string m_filename;
};
