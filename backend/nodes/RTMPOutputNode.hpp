#pragma once

#include "core/Node.hpp"
#include "FFmpeg.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>

class RTMPOutputNode : public NodeBase {
public:
    RTMPOutputNode() { addInput<MediaPacket>(); }
    ~RTMPOutputNode() override;

    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    const char* nodeType() const override { return "RTMPOutputNode"; }
    static int interrupt(void* opaque);
    void setDeadline();
    int writeHeader();
    int finishMedia(bool completed);
    bool pace(const AVPacket& packet, AVRational time_base);

    struct StreamState {
        AVRational time_base;
        int output_index;
        bool ended = false;
    };
    std::unordered_map<int, StreamState> m_streams;
    AVFormatContext* m_media = nullptr;
    std::string m_url;
    std::atomic<std::int64_t> m_deadline_ns{0};
    std::chrono::steady_clock::time_point m_clock_origin;
    std::int64_t m_media_origin_us = AV_NOPTS_VALUE;
    std::size_t m_ended_streams = 0;
    std::size_t m_packets_written = 0;
    bool m_header_written = false;
    bool m_realtime = true;
};
