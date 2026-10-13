#include "DemuxNode.hpp"

DemuxNode::DemuxNode(std::size_t ports) : m_ports(ports) {
    if (!ports) fail(InvalidPorts, "requires output ports");
    addInput<MediaPacket>();
    for (std::size_t i = 0; i < ports; ++i) addOutput<MediaPacket>();
}

int DemuxNode::configure(std::unordered_map<std::string, std::string> config) {
    auto ports = m_ports;
    if (!parseMediaPortCount(config, ports)) return fail(InvalidPorts, "ports must be a positive integer");
    m_ports = ports;
    resetOutputs();
    for (std::size_t i = 0; i < m_ports; ++i) addOutput<MediaPacket>();
    m_routes.clear();
    m_ended = 0;
    m_configs_complete = false;
    m_status = Success;
    return Success;
}

void DemuxNode::finishConfigs() {
    if (m_configs_complete) return;
    m_configs_complete = true;
    for (std::size_t i = m_routes.size(); i < m_ports; ++i) {
        if (auto queue = output<MediaPacket>(i)) queue->close();
    }
}

int DemuxNode::process() {
    if (m_status != Success) return m_status;
    auto message = input<MediaPacket>()->pop();
    if (!message) { finishConfigs(); return -1; }
    if (message->stream_id < 0) return fail(InvalidStreamId, "invalid stream ID", 0, message->stream_id);
    auto found = m_routes.find(message->stream_id);
    if (message->type == MediaPacket::Type::CONFIGFRAME) {
        if (m_configs_complete) return fail(LateConfigFrame, "late CONFIGFRAME", 0, message->stream_id);
        if (found != m_routes.end()) return fail(DuplicateStreamId, "duplicate stream ID", 0, message->stream_id);
        if (!message->config || !message->config->codec || message->packet ||
            message->config->time_base.num <= 0 || message->config->time_base.den <= 0)
            return fail(InvalidConfigFrame, "invalid CONFIGFRAME", 0, message->stream_id);
        const auto port = m_routes.size();
        if (port >= m_ports) return fail(InsufficientPorts, "insufficient output ports", 0, message->stream_id);
        found = m_routes.emplace(message->stream_id, Route{port, false}).first;
    } else {
        finishConfigs();
        if (found == m_routes.end()) return fail(FrameBeforeConfig, "frame before CONFIGFRAME", 0, message->stream_id);
        if (found->second.ended) return fail(FrameAfterEnd, "frame after ENDFRAME", 0, message->stream_id);
        if (message->type == MediaPacket::Type::DATAFRAME) {
            if (!message->packet || message->config) return fail(InvalidDataFrame, "invalid DATAFRAME", 0, message->stream_id);
        } else if (message->type == MediaPacket::Type::ENDFRAME) {
            if (message->packet || message->config) return fail(InvalidEndFrame, "invalid ENDFRAME", 0, message->stream_id);
            found->second.ended = true;
            ++m_ended;
        } else return fail(UnknownFrameType, "unknown frame type", 0, message->stream_id);
    }
    auto queue = output<MediaPacket>(found->second.port);
    if (!queue) return fail(OutputNotConnected, "stream output is not connected", found->second.port, message->stream_id);
    if (!queue->push(std::move(*message))) return -1;
    if (found->second.ended) queue->close();
    return m_configs_complete && m_ended == m_routes.size() ? -1 : 0;
}
