#pragma once

#include "core/Node.hpp"
#include "FFmpeg.hpp"

#include <string>
#include <vector>

class InputNode : public NodeBase {
public:
    InputNode() { addOutput<MediaPacket>(); }
    ~InputNode() override;

    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    const char* nodeType() const override { return "InputNode"; }
    int progressPercent(const AVPacket& packet) const;
    void logProgress(int percent);
    std::size_t m_packets_read = 0;
    int m_last_progress = -1;
    bool m_progress_started = false;
    bool m_finished = false;
    AVFormatContext* m_media = nullptr;
    std::vector<MediaStream> m_streams;
    std::size_t m_next_config = 0;
    std::size_t m_next_end = 0;
    bool m_eof = false;
    bool m_loop = false;
    bool m_loop_has_packets = false;
    int64_t m_loop_offset = 0; // AV_TIME_BASE units, shared by all streams.
    int64_t m_loop_duration = 0;
    std::vector<int64_t> m_first_timestamp;

    std::string m_filename;
};
