#pragma once
#include "core/Node.hpp"
#include "MediaFrame.hpp"
extern "C" {
#include <libavutil/audio_fifo.h>
}

class EncoderNode : public NodeBase {
public:
    ~EncoderNode() override;
    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;
protected:
    explicit EncoderNode(AVMediaType type);
private:
    int open(const MediaFrame& message);
    int receive(bool draining);
    int encode(AVFrame* frame);
    int audio(AVFrame* frame, bool draining);
    int send(MediaPacket message);
    void reset();
    AVMediaType m_type;
    AVCodecContext* m_codec = nullptr;
    AVAudioFifo* m_fifo = nullptr;
    AVRational m_input_time_base{0, 1};
    std::string m_codec_name;
    int64_t m_bitrate = 0;
    int m_threads = 1;
    int m_gop = 12;
    int m_b_frames = 0;
    int m_stream_id = -1;
    int64_t m_audio_pts = AV_NOPTS_VALUE;
    std::size_t m_packets = 0;
    std::size_t m_frames = 0;
    bool m_finished = false;
};
