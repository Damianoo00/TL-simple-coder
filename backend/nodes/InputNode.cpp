#include "InputNode.hpp"

#include <algorithm>

int InputNode::configure(std::unordered_map<std::string, std::string> config) {
    const auto entry = config.find("filename");
    if (entry == config.end() || entry->second.empty())
        return fail(InvalidConfiguration, "missing non-empty filename");
    bool loop = false;
    if (const auto option = config.find("loop"); option != config.end()) {
        if (option->second == "true" || option->second == "1") loop = true;
        else if (option->second != "false" && option->second != "0")
            return fail(InvalidConfiguration, "loop must be true, false, 1 or 0");
    }
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
    m_eof = m_finished = m_progress_started = false;
    m_loop = loop;
    m_loop_has_packets = false;
    m_loop_offset = 0;
    m_loop_duration = m_media->duration > 0 ? m_media->duration : 0;
    m_first_timestamp.assign(m_streams.size(), AV_NOPTS_VALUE);
    m_packets_read = 0;
    m_last_progress = -1;
    m_status = Success;
    std::fprintf(stderr, "[info] InputNode: opened file=\"%s\" streams=%zu\n", m_filename.c_str(), m_streams.size());
    return Success;
}

InputNode::~InputNode() {
    stop();
    avformat_close_input(&m_media);
}

int InputNode::progressPercent(const AVPacket& packet) const {
    if (m_media->duration <= 0 || m_media->duration == AV_NOPTS_VALUE) return 0;
    const auto timestamp = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
    if (timestamp == AV_NOPTS_VALUE) return 0;
    const auto time_base = m_media->streams[packet.stream_index]->time_base;
    const auto position = av_rescale_q(timestamp, time_base, AV_TIME_BASE_Q);
    const auto duration = packet.duration > 0 ? av_rescale_q(packet.duration, time_base, AV_TIME_BASE_Q) : 0;
    const auto start = m_media->start_time == AV_NOPTS_VALUE ? 0 : m_media->start_time;
    const long double elapsed = static_cast<long double>(position) - start + duration;
    // 100% is reserved for EOF after all end messages have been accepted.
    return static_cast<int>(std::clamp(elapsed * 100 / m_media->duration, 0.0L, 99.0L));
}

void InputNode::logProgress(int percent) {
    if (percent == m_last_progress) return;
    m_last_progress = percent;
    std::fprintf(stderr, "[info] InputNode: read progress=%d%% file=\"%s\" packets=%zu\n",
                 percent, m_filename.c_str(), m_packets_read);
}

int InputNode::process() {
    if (m_status != Success) return m_status;
    if (m_finished || m_stop_requested.load()) return Finished;
    if (!m_media) return fail(NotConfigured, "input is not configured");
    auto queue = output<MediaPacket>();
    if (!queue) return fail(OutputNotConnected, "output is not connected");
    if (!m_progress_started) {
        m_progress_started = true;
        if (m_media->duration > 0 && m_media->duration != AV_NOPTS_VALUE) logProgress(0);
        else std::fprintf(stderr, "[info] InputNode: read progress=unavailable file=\"%s\" reason=unknown_duration\n",
                          m_filename.c_str());
    }
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
        int result = av_read_frame(m_media, packet.get());
        if (result == AVERROR_EOF && m_loop) {
            if (!m_loop_has_packets || m_loop_duration <= 0)
                return fail(MediaError, "cannot loop empty input or input without a known timeline");
            const auto start = m_media->start_time == AV_NOPTS_VALUE ? 0 : m_media->start_time;
            if (logMediaResult(av_seek_frame(m_media, -1, start, AVSEEK_FLAG_BACKWARD),
                               "InputNode: cannot rewind " + m_filename) < 0)
                return fail(MediaError, "cannot rewind looped input");
            m_loop_offset += m_loop_duration;
            m_loop_has_packets = false;
            m_last_progress = -1;
            logProgress(0);
            result = av_read_frame(m_media, packet.get());
            if (result == AVERROR_EOF)
                return fail(MediaError, "looped input has no packets after rewind");
        }
        if (result != AVERROR_EOF) {
            if (logMediaResult(result, "InputNode: cannot read " + m_filename) < 0)
                return fail(MediaError, "cannot read input packet");
            const int id = packet->stream_index;
            if (id < 0 || static_cast<std::size_t>(id) >= m_streams.size())
                return fail(InvalidStreamId, "input packet references an unknown stream", 0, id);
            const int percent = progressPercent(*packet);
            if (m_loop) {
                m_loop_has_packets = true;
                const auto time_base = m_streams[id].time_base;
                const auto timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                if (timestamp != AV_NOPTS_VALUE) {
                    if (m_first_timestamp[id] == AV_NOPTS_VALUE) m_first_timestamp[id] = timestamp;
                    const auto span = timestamp - m_first_timestamp[id] + std::max<int64_t>(packet->duration, 1);
                    m_loop_duration = std::max(m_loop_duration,
                        av_rescale_q_rnd(span, time_base, AV_TIME_BASE_Q, AV_ROUND_UP));
                }
                const auto offset = av_rescale_q(m_loop_offset, AV_TIME_BASE_Q, time_base);
                if (packet->pts != AV_NOPTS_VALUE) packet->pts += offset;
                if (packet->dts != AV_NOPTS_VALUE) packet->dts += offset;
            }
            const int sent = send(MediaPacket::dataFrame(id, std::move(packet)));
            if (sent == Success) {
                ++m_packets_read;
                if (percent >= m_last_progress + 5) logProgress(percent);
            }
            return sent;
        }
        m_eof = true;
    }
    if (m_next_end < m_streams.size()) {
        const int sent = send(MediaPacket::endFrame(static_cast<int>(m_next_end++)));
        if (sent == Success && m_next_end == m_streams.size()) {
            logProgress(100);
            m_finished = true;
        }
        return sent;
    }
    logProgress(100);
    m_finished = true;
    return Finished;
}
