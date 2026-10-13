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
    AVFormatContext* m_media = nullptr;
    std::vector<MediaStream> m_streams;
    std::size_t m_next_config = 0;
    std::size_t m_next_end = 0;
    bool m_eof = false;
    std::string m_filename;
};
