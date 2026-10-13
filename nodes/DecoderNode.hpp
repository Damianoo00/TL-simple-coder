#pragma once
#include "core/Node.hpp"
#include "MediaFrame.hpp"

// Shared implementation; public concrete nodes fix the media type.
class DecoderNode : public NodeBase {
public:
    ~DecoderNode() override;
    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;
protected:
    explicit DecoderNode(AVMediaType type);
private:
    int open(const MediaPacket& message);
    int receive(bool draining);
    int send(MediaFrame message);
    AVMediaType m_type;
    AVCodecContext* m_codec = nullptr;
    std::shared_ptr<const MediaStream> m_config;
    int m_stream_id = -1;
    int m_threads = 1;
    bool m_config_sent = false;
    bool m_finished = false;
};
