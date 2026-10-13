#include "FrameTransformNode.hpp"

FrameTransformNode::FrameTransformNode(AVMediaType type) : m_type(type) {
    addInput<MediaFrame>();
    addOutput<MediaFrame>();
}
void FrameTransformNode::resetStream() {
    m_stream_id = -1;
    m_input_config.reset();
    m_finished = false;
    m_input_frames = m_output_frames = 0;
    m_status = Success;
}
int FrameTransformNode::send(MediaFrame message) {
    const bool data = message.type == MediaFrame::Type::DATAFRAME;
    auto queue = output<MediaFrame>();
    if (!queue) return fail(OutputNotConnected, "raw frame output is not connected", 0, m_stream_id);
    if (!queue->push(std::move(message))) return Finished;
    if (data) ++m_output_frames;
    return Success;
}
int FrameTransformNode::process() {
    if (m_status != Success) return m_status;
    if (m_finished || m_stop_requested.load()) return Finished;
    if (!output<MediaFrame>()) return fail(OutputNotConnected, "raw frame output is not connected");
    auto message = input<MediaFrame>()->pop();
    if (!message) { m_finished = true; return Finished; }
    if (message->type == MediaFrame::Type::CONFIGFRAME) {
        if (m_input_config) return fail(DuplicateStreamId, "single-stream converter already configured", 0, message->stream_id);
        const auto& config = message->config;
        if (message->stream_id < 0 || !config || !config->codec || message->frame ||
            config->codec->codec_type != m_type || config->time_base.num <= 0 || config->time_base.den <= 0)
            return fail(InvalidConfigFrame, "invalid raw CONFIGFRAME or wrong media type", 0, message->stream_id);
        m_stream_id = message->stream_id;
        m_input_config = config;
        return open(*config);
    }
    if (!m_input_config) return fail(FrameBeforeConfig, "frame before CONFIGFRAME", 0, message->stream_id);
    if (message->stream_id != m_stream_id)
        return fail(StreamIdChanged, "single-stream converter received another stream ID", 0, message->stream_id);
    if (message->type == MediaFrame::Type::ENDFRAME) {
        if (message->config || message->frame) return fail(InvalidEndFrame, "ENDFRAME contains data", 0, m_stream_id);
        const int result = flush();
        if (result != Success) return result;
        const int sent = send(MediaFrame::endFrame(m_stream_id));
        m_finished = true;
        if (sent == Success)
            std::fprintf(stderr, "[info] %s: conversion finished stream_id=%d input_frames=%zu output_frames=%zu\n",
                         nodeType(), m_stream_id, m_input_frames, m_output_frames);
        return sent == Success ? Finished : sent;
    }
    if (message->type != MediaFrame::Type::DATAFRAME)
        return fail(UnknownFrameType, "unknown frame type", 0, m_stream_id);
    if (!message->frame || message->config || !message->frame->data[0])
        return fail(InvalidDataFrame, "DATAFRAME requires allocated AVFrame without config", 0, m_stream_id);
    const int result = transform(*message->frame);
    if (result == Success) ++m_input_frames;
    return result;
}
