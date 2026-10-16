#pragma once
#include "FrameTransformNode.hpp"
extern "C" {
#include <libswresample/swresample.h>
}

class AudioResamplerNode final : public FrameTransformNode {
public:
    AudioResamplerNode() : FrameTransformNode(AVMEDIA_TYPE_AUDIO) {}
    ~AudioResamplerNode() override;
    int configure(std::unordered_map<std::string, std::string> config) override;
private:
    const char* nodeType() const override { return "AudioResamplerNode"; }
    int open(const MediaStream& stream) override;
    int transform(const AVFrame& frame) override;
    int flush() override;
    int convert(const AVFrame* frame, bool& produced);
    SwrContext* m_resampler = nullptr;
    std::shared_ptr<AVCodecParameters> m_output_parameters;
    int m_sample_rate = 0;
    AVSampleFormat m_format = AV_SAMPLE_FMT_FLTP;
    std::string m_layout;
    int64_t m_next_pts = AV_NOPTS_VALUE;
    int64_t m_next_input_pts = AV_NOPTS_VALUE;
    int64_t m_input_samples = 0;
    int64_t m_output_samples = 0;
};
