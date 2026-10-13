#include "DecoderNode.hpp"

DecoderNode::DecoderNode(AVMediaType type) : m_type(type) {
    addInput<MediaPacket>();
    addOutput<MediaFrame>();
}
DecoderNode::~DecoderNode() { stop(); avcodec_free_context(&m_codec); }

int DecoderNode::configure(std::unordered_map<std::string, std::string> config) {
    int threads = 1;
    for (const auto& [key, value] : config) {
        if (key != "threads") return fail(InvalidConfiguration, "supported option: threads");
        const auto result = std::from_chars(value.data(), value.data() + value.size(), threads);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || threads <= 0)
            return fail(InvalidConfiguration, "threads must be a positive integer");
    }
    avcodec_free_context(&m_codec);
    m_config.reset();
    m_stream_id = -1;
    m_threads = threads;
    m_config_sent = m_finished = false;
    m_status = Success;
    return Success;
}

int DecoderNode::send(MediaFrame message) {
    auto queue = output<MediaFrame>();
    if (!queue) return fail(OutputNotConnected, "decoded frame output is not connected", 0, m_stream_id);
    return queue->push(std::move(message)) ? Success : Finished;
}

int DecoderNode::open(const MediaPacket& message) {
    if (m_codec) return fail(DuplicateStreamId, "single-stream decoder already configured", 0, message.stream_id);
    const auto& config = message.config;
    if (message.stream_id < 0 || !config || !config->codec || message.packet ||
        config->time_base.num <= 0 || config->time_base.den <= 0 || config->codec->codec_type != m_type)
        return fail(InvalidConfigFrame, "invalid CONFIGFRAME or wrong media type", 0, message.stream_id);
    const auto codec = avcodec_find_decoder(config->codec->codec_id);
    if (!codec) return fail(MediaError, "decoder for input codec is unavailable", 0, message.stream_id);
    m_codec = avcodec_alloc_context3(codec);
    if (!m_codec) return fail(AllocationFailed, "cannot allocate decoder context", 0, message.stream_id);
    m_stream_id = message.stream_id;
    m_config = config;
    if (logMediaResult(avcodec_parameters_to_context(m_codec, config->codec.get()), nodeType()) < 0)
        return fail(MediaError, "cannot copy decoder parameters", 0, m_stream_id);
    m_codec->pkt_timebase = config->time_base;
    m_codec->framerate = config->frame_rate;
    m_codec->thread_count = m_threads;
    if (logMediaResult(avcodec_open2(m_codec, codec, nullptr), nodeType()) < 0)
        return fail(MediaError, "cannot open decoder", 0, m_stream_id);
    return Success;
}

int DecoderNode::receive(bool draining) {
    for (;;) {
        auto raw = av_frame_alloc();
        if (!raw) return fail(AllocationFailed, "cannot allocate decoded frame", 0, m_stream_id);
        AVFramePtr frame(raw, [](AVFrame* p) { av_frame_free(&p); });
        const int result = avcodec_receive_frame(m_codec, raw);
        if (result == AVERROR_EOF) return Success;
        if (result == AVERROR(EAGAIN))
            return draining ? fail(MediaError, "decoder requested input while draining", 0, m_stream_id) : Success;
        if (logMediaResult(result, nodeType()) < 0)
            return fail(MediaError, "cannot receive decoded frame", 0, m_stream_id);
        // Formats can be unknown until the first decoded frame arrives.
        if (!m_config_sent) {
            auto parameters = avcodec_parameters_alloc();
            if (!parameters) return fail(AllocationFailed, "cannot allocate decoded parameters", 0, m_stream_id);
            std::shared_ptr<AVCodecParameters> owned(parameters,
                [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
            if (logMediaResult(avcodec_parameters_from_context(parameters, m_codec), nodeType()) < 0)
                return fail(MediaError, "cannot export decoded parameters", 0, m_stream_id);
            parameters->format = raw->format;
            if (m_type == AVMEDIA_TYPE_VIDEO) {
                parameters->width = raw->width;
                parameters->height = raw->height;
            } else {
                parameters->sample_rate = raw->sample_rate;
                if (logMediaResult(av_channel_layout_copy(&parameters->ch_layout, &raw->ch_layout), nodeType()) < 0)
                    return fail(MediaError, "cannot copy decoded channel layout", 0, m_stream_id);
            }
            const int sent = send(MediaFrame::configFrame(m_stream_id, {std::move(owned), m_config->time_base, m_config->frame_rate}));
            if (sent != Success) return sent;
            m_config_sent = true;
        }
        raw->pts = raw->best_effort_timestamp;
        raw->time_base = m_config->time_base;
        const int sent = send(MediaFrame::dataFrame(m_stream_id, std::move(frame)));
        if (sent != Success) return sent;
    }
}

int DecoderNode::process() {
    if (m_status != Success) return m_status;
    if (m_finished || m_stop_requested.load()) return Finished;
    if (!output<MediaFrame>()) return fail(OutputNotConnected, "decoded frame output is not connected");
    auto message = input<MediaPacket>()->pop();
    if (!message) { m_finished = true; return Finished; }
    if (message->type == MediaPacket::Type::CONFIGFRAME) return open(*message);
    if (!m_codec) return fail(FrameBeforeConfig, "packet before CONFIGFRAME", 0, message->stream_id);
    if (message->stream_id != m_stream_id)
        return fail(StreamIdChanged, "single-stream decoder received another stream ID", 0, message->stream_id);
    bool draining = false;
    if (message->type == MediaPacket::Type::DATAFRAME) {
        if (!message->packet || message->config)
            return fail(InvalidDataFrame, "DATAFRAME requires packet without config", 0, m_stream_id);
    } else if (message->type == MediaPacket::Type::ENDFRAME) {
        if (message->packet || message->config) return fail(InvalidEndFrame, "ENDFRAME contains data", 0, m_stream_id);
        draining = true;
    } else return fail(UnknownFrameType, "unknown packet type", 0, m_stream_id);
    const int result = avcodec_send_packet(m_codec, draining ? nullptr : message->packet.get());
    if (logMediaResult(result, nodeType()) < 0)
        return fail(MediaError, "cannot send packet to decoder", 0, m_stream_id);
    const int received = receive(draining);
    if (received != Success) return received;
    if (!draining) return Success;
    // Empty streams still carry configuration and EOF.
    if (!m_config_sent) {
        const int sent = send(MediaFrame::configFrame(m_stream_id, *m_config));
        if (sent != Success) return sent;
    }
    const int sent = send(MediaFrame::endFrame(m_stream_id));
    m_finished = true;
    return sent == Success ? Finished : sent;
}
