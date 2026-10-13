#include "InputNode.hpp"

int InputNode::configure(std::unordered_map<std::string, std::string> config) {
    const auto entry = config.find("filename");
    if (entry == config.end() || entry->second.empty())
        return fail(InvalidConfiguration, "missing non-empty filename");
    const auto& filename = entry->second;
    AVFormatContext* raw = nullptr;
    if (logMediaResult(avformat_open_input(&raw, filename.c_str(), nullptr, nullptr),
                       "InputNode: cannot open " + filename) < 0)
        return fail(MediaError, "cannot open input file");
    auto close = [](AVFormatContext* context) { avformat_close_input(&context); };
    std::unique_ptr<AVFormatContext, decltype(close)> media(raw, close);
    if (logMediaResult(avformat_find_stream_info(raw, nullptr), "InputNode: cannot read streams " + filename) < 0)
        return fail(MediaError, "cannot read stream information");
    std::vector<MediaStream> streams;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        auto codec = avcodec_parameters_alloc();
        if (!codec) return fail(AllocationFailed, "cannot allocate codec parameters", 0, static_cast<int>(i));
        std::shared_ptr<AVCodecParameters> owned(codec,
            [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
        if (logMediaResult(avcodec_parameters_copy(codec, raw->streams[i]->codecpar),
                           "InputNode: cannot copy stream parameters") < 0)
            return fail(MediaError, "cannot copy stream parameters", 0, static_cast<int>(i));
        streams.push_back({std::move(owned), raw->streams[i]->time_base, av_guess_frame_rate(raw, raw->streams[i], nullptr)});
    }
    m_filename = filename;
    avformat_close_input(&m_media);
    m_media = media.release();
    m_streams = std::move(streams);
    m_next_config = m_next_end = 0;
    m_eof = false;
    m_status = Success;
    return Success;
}

InputNode::~InputNode() {
    stop();
    avformat_close_input(&m_media);
}

int InputNode::process() {
    if (m_status != Success) return m_status;
    if (m_stop_requested.load()) return Finished;
    if (!m_media) return fail(NotConfigured, "input is not configured");
    auto queue = output<MediaPacket>();
    if (!queue) return fail(OutputNotConnected, "output is not connected");
    auto send = [&queue](MediaPacket message) {
        return queue->push(std::move(message)) ? Success : Finished;
    };
    if (m_next_config < m_streams.size()) {
        const auto id = m_next_config++;
        return send(MediaPacket::configFrame(static_cast<int>(id), m_streams[id]));
    }
    if (!m_eof) {
        auto raw = av_packet_alloc();
        if (!raw) return fail(AllocationFailed, "cannot allocate input packet");
        AVPacketPtr packet(raw, [](AVPacket* p) { av_packet_free(&p); });
        const int result = av_read_frame(m_media, packet.get());
        if (result != AVERROR_EOF) {
            if (logMediaResult(result, "InputNode: cannot read " + m_filename) < 0)
                return fail(MediaError, "cannot read input packet");
            const int id = packet->stream_index;
            return send(MediaPacket::dataFrame(id, std::move(packet)));
        }
        m_eof = true;
    }
    if (m_next_end < m_streams.size())
        return send(MediaPacket::endFrame(static_cast<int>(m_next_end++)));
    return Finished;
}
