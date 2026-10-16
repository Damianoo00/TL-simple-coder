#pragma once
#include "FrameTransformNode.hpp"
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
}

class VideoScalerNode final : public FrameTransformNode {
public:
    VideoScalerNode() : FrameTransformNode(AVMEDIA_TYPE_VIDEO) {}
    ~VideoScalerNode() override;
    int configure(std::unordered_map<std::string, std::string> config) override;
private:
    const char* nodeType() const override { return "VideoScalerNode"; }
    int open(const MediaStream& stream) override;
    int transform(const AVFrame& frame) override;
    SwsContext* m_scaler = nullptr;
    std::shared_ptr<AVCodecParameters> m_output_parameters;
    int m_width = 0;
    int m_height = 0;
    AVPixelFormat m_format = AV_PIX_FMT_YUV420P;
    int m_algorithm = SWS_BILINEAR;
};
