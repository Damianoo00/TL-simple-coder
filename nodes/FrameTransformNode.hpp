#pragma once
#include "core/Node.hpp"
#include "MediaFrame.hpp"

// Shared single-stream raw-frame protocol for scaler and resampler.
class FrameTransformNode : public NodeBase {
public:
    ~FrameTransformNode() override { stop(); }
    int process() override;
protected:
    explicit FrameTransformNode(AVMediaType type);
    virtual int open(const MediaStream& stream) = 0;
    virtual int transform(const AVFrame& frame) = 0;
    virtual int flush() { return Success; }
    int send(MediaFrame message);
    void resetStream();
    int m_stream_id = -1;
    std::shared_ptr<const MediaStream> m_input_config;
private:
    AVMediaType m_type;
    bool m_finished = false;
    std::size_t m_input_frames = 0;
    std::size_t m_output_frames = 0;
};
