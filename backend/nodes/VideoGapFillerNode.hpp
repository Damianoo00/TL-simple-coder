#pragma once
#include "FrameTransformNode.hpp"

class VideoGapFillerNode final : public FrameTransformNode {
public:
    VideoGapFillerNode() : FrameTransformNode(AVMEDIA_TYPE_VIDEO) {}
    ~VideoGapFillerNode() override { stop(); }
    int configure(std::unordered_map<std::string, std::string> config) override;
private:
    const char* nodeType() const override { return "VideoGapFillerNode"; }
    int open(const MediaStream& stream) override;
    int transform(const AVFrame& frame) override;
    AVFramePtr m_last;
    int64_t m_next_pts = AV_NOPTS_VALUE;
    int64_t m_step = 0;
    int m_max_gap_ms = 10000;
};
