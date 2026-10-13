#pragma once
#include "EncoderNode.hpp"

class AudioEncoderNode final : public EncoderNode {
public:
    AudioEncoderNode() : EncoderNode(AVMEDIA_TYPE_AUDIO) {}
    ~AudioEncoderNode() override { stop(); }
private:
    const char* nodeType() const override { return "AudioEncoderNode"; }
};
