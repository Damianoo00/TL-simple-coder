#pragma once
#include "FrameTransformNode.hpp"

class AudioGapFillerNode final : public FrameTransformNode {
public:
    AudioGapFillerNode() : FrameTransformNode(AVMEDIA_TYPE_AUDIO) {}
    ~AudioGapFillerNode() override { stop(); }
    int configure(std::unordered_map<std::string, std::string> config) override;
private:
    const char* nodeType() const override { return "AudioGapFillerNode"; }
    int open(const MediaStream& stream) override;
    int transform(const AVFrame& frame) override;
    int64_t m_next_pts = AV_NOPTS_VALUE;
    int m_max_gap_ms = 10000;
};
