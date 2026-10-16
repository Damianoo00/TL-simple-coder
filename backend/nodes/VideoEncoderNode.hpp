#pragma once
#include "EncoderNode.hpp"

class VideoEncoderNode final : public EncoderNode {
public:
    VideoEncoderNode() : EncoderNode(AVMEDIA_TYPE_VIDEO) {}
    ~VideoEncoderNode() override { stop(); }
private:
    const char* nodeType() const override { return "VideoEncoderNode"; }
};
