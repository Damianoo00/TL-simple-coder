#include "MuxerNode.hpp"


MuxerNode::MuxerNode(std::size_t ports) : m_streams(ports) {
    if (!ports) fail(InvalidPorts, "requires at least one input port");
    for (std::size_t i = 0; i < ports; ++i) addInput<MediaPacket>();
    addOutput<MediaPacket>();
}

int MuxerNode::configure(std::unordered_map<std::string, std::string> config) {
    return configureChecked(config);
}

int MuxerNode::configureChecked(const std::unordered_map<std::string, std::string>& config) {
    auto ports = m_streams.size();
    if (!parseMediaPortCount(config, ports))
        return fail(InvalidPorts, "configuration 'ports' must be a positive integer");
    resetInputs();
    for (std::size_t i = 0; i < ports; ++i) addInput<MediaPacket>();
    m_streams.clear();
    m_streams.resize(ports);
    m_initialized = false;
    m_truncated = false;
    m_next_config = m_next_end = 0;
    m_status = Success;
    return Success;
}

int MuxerNode::send(MediaPacket message) {
    auto queue = output<MediaPacket>();
    if (!queue) return fail(OutputNotConnected, "output port 0 is not connected");
    return queue->push(std::move(message)) ? Success : Finished;
}

int MuxerNode::initialize() {
    for (std::size_t i = 0; i < m_streams.size(); ++i) {
        auto message = input<MediaPacket>(i)->pop();
        if (!message) { m_streams[i].finished = true; continue; }
        if (message->type != MediaPacket::Type::CONFIGFRAME || message->stream_id < 0 ||
            !message->config || !message->config->codec || message->packet ||
            message->config->time_base.num <= 0 || message->config->time_base.den <= 0)
            return fail(InvalidConfigFrame, "expected CONFIGFRAME with nonnegative ID, codec and positive time base, without packet", i, message->stream_id);
        for (std::size_t previous = 0; previous < i; ++previous) {
            const auto& config = m_streams[previous].config;
            if (config && config->stream_id == message->stream_id)
                return fail(DuplicateStreamId, "stream ID already configured on another input port", i, message->stream_id);
        }
        m_streams[i].config = std::move(message);
    }
    m_initialized = true;
    return Success;
}

int MuxerNode::process() {
    if (m_status != Success) return m_status;
    if (!m_initialized) {
        const int result = initialize();
        if (result != Success) return result;
    }
    while (m_next_config < m_streams.size()) {
        auto& stream = m_streams[m_next_config++];
        if (stream.config) return send(*stream.config);
    }

    std::optional<std::size_t> selected;
    int64_t best_time = AV_NOPTS_VALUE;
    for (std::size_t i = 0; i < m_streams.size(); ++i) {
        auto& stream = m_streams[i];
        if (stream.finished) continue;
        if (!stream.head) {
            auto message = input<MediaPacket>(i)->pop();
            if (!message) {
                stream.finished = true;
                m_truncated = true;
                continue;
            }
            if (message->stream_id != stream.config->stream_id)
                return fail(StreamIdChanged, "stream ID differs from initial CONFIGFRAME", i, message->stream_id);
            if (message->type == MediaPacket::Type::ENDFRAME) {
                if (message->config || message->packet) return fail(InvalidEndFrame, "ENDFRAME must not contain config or packet", i, message->stream_id);
                stream.finished = true;
                continue;
            }
            if (message->type != MediaPacket::Type::DATAFRAME || !message->packet || message->config)
                return fail(InvalidDataFrame, "expected DATAFRAME with packet and without config, or ENDFRAME", i, message->stream_id);
            stream.head = std::move(message);
        }
        const auto& packet = stream.head->packet;
        const int64_t time = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        // Untimed packets are emitted in port order; otherwise compare actual
        // timestamps in each stream's time base, never the raw integer values.
        if (!selected || (time == AV_NOPTS_VALUE && best_time != AV_NOPTS_VALUE) ||
            (time != AV_NOPTS_VALUE && best_time != AV_NOPTS_VALUE &&
             av_compare_ts(time, stream.config->config->time_base, best_time,
                           m_streams[*selected].config->config->time_base) < 0)) {
            selected = i;
            best_time = time;
        }
    }
    if (selected) {
        auto& head = m_streams[*selected].head;
        auto message = std::move(*head);
        head.reset();
        return send(std::move(message));
    }
    // A closed queue without ENDFRAME means cancellation, not a synthetic EOF.
    if (m_truncated) return Finished;
    while (m_next_end < m_streams.size()) {
        const auto& stream = m_streams[m_next_end++];
        if (stream.config) return send(MediaPacket::endFrame(stream.config->stream_id));
    }
    return Finished;
}
