#pragma once
#include "DecoderNode.hpp"

class AudioDecoderNode final : public DecoderNode {
public:
    AudioDecoderNode() : DecoderNode(AVMEDIA_TYPE_AUDIO) {}
    ~AudioDecoderNode() override { stop(); }
private:
    const char* nodeType() const override { return "AudioDecoderNode"; }
};
