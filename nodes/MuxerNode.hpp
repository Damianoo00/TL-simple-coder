#pragma once

#include "core/Node.hpp"
#include "FFmpeg.hpp"

#include <optional>
#include <vector>

class MuxerNode : public NodeBase {
public:
    int configureChecked(const std::unordered_map<std::string, std::string>& config);
    explicit MuxerNode(std::size_t ports = 2);
    ~MuxerNode() override { stop(); }
    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    const char* nodeType() const override { return "MuxerNode"; }
    struct Stream {
        std::optional<MediaPacket> config;
        std::optional<MediaPacket> head;
        bool finished = false;
    };
    std::vector<Stream> m_streams;
    bool m_initialized = false;
    bool m_truncated = false;
    std::size_t m_next_config = 0;
    std::size_t m_next_end = 0;
    int send(MediaPacket message);
    int initialize();
};
