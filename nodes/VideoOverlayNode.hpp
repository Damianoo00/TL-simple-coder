#pragma once
#include "FrameTransformNode.hpp"
#include "BrowserRenderer.hpp"
extern "C" {
#include <libswscale/swscale.h>
}

class VideoOverlayNode final : public FrameTransformNode {
public:
    VideoOverlayNode() : FrameTransformNode(AVMEDIA_TYPE_VIDEO) {}
    ~VideoOverlayNode() override;
    int configure(std::unordered_map<std::string, std::string> config) override;
private:
    const char* nodeType() const override { return "VideoOverlayNode"; }
    int open(const MediaStream& stream) override;
    int transform(const AVFrame& frame) override;
    int convert(const AVFrame& frame);
    int emit(int64_t pts);
    int flush() override;
    BrowserRenderer m_browser;
    SwsContext* m_video_scaler = nullptr;
    SwsContext* m_overlay_scaler = nullptr;
    std::shared_ptr<AVCodecParameters> m_parameters;
    std::shared_ptr<const OverlayBitmap> m_source_overlay;
    OverlayBitmap m_scaled_overlay;
    int m_width = 1920, m_height = 1080, m_fps = 60;
    int64_t m_first_pts = AV_NOPTS_VALUE, m_last_pts = AV_NOPTS_VALUE;
    int64_t m_next_pts = 0;
    AVFramePtr m_previous;
    AVRational m_output_time_base{1, 60};
    std::chrono::steady_clock::time_point m_started, m_last_log;
    std::size_t m_frames = 0, m_late = 0;
};
