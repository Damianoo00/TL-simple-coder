#pragma once
#include "DecoderNode.hpp"

class VideoDecoderNode final : public DecoderNode {
public:
    VideoDecoderNode() : DecoderNode(AVMEDIA_TYPE_VIDEO) {}
    ~VideoDecoderNode() override { stop(); }
private:
    const char* nodeType() const override { return "VideoDecoderNode"; }
};
