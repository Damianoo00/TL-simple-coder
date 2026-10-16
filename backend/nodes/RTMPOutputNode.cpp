#include "RTMPOutputNode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr auto ioTimeout = std::chrono::seconds(5);
}

RTMPOutputNode::~RTMPOutputNode() {
    stop();
    finishMedia(false);
}

int RTMPOutputNode::configure(std::unordered_map<std::string, std::string> config) {
    const auto url = config.find("url");
    if (url == config.end() || !url->second.starts_with("rtmp://") || url->second.size() <= 7)
        return fail(InvalidConfiguration, "url must be a non-empty rtmp:// address");
    bool realtime = true;
    if (const auto option = config.find("realtime"); option != config.end()) {
        if (option->second == "true") realtime = true;
        else if (option->second == "false") realtime = false;
        else return fail(InvalidConfiguration, "realtime must be true or false");
    }
    for (const auto& [key, value] : config) {
        (void)value;
        if (key != "url" && key != "realtime")
            return fail(InvalidConfiguration, "unknown RTMP output option");
    }
    finishMedia(false);
    m_url = url->second;
    m_realtime = realtime;
    m_packets_written = 0;
    m_status = Success;
    return Success;
}

int RTMPOutputNode::interrupt(void* opaque) {
    auto* self = static_cast<RTMPOutputNode*>(opaque);
    if (self->m_stop_requested.load(std::memory_order_relaxed)) return 1;
    const auto deadline = self->m_deadline_ns.load(std::memory_order_relaxed);
    if (!deadline) return 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() >= deadline;
}

void RTMPOutputNode::setDeadline() {
    const auto until = (std::chrono::steady_clock::now() + ioTimeout).time_since_epoch();
    m_deadline_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(until).count(),
                        std::memory_order_relaxed);
}

int RTMPOutputNode::writeHeader() {
    if (m_header_written) return Success;
    if (m_streams.empty()) return fail(InvalidConfigFrame, "RTMP output has no configured streams");
    // FLV muxing is selected explicitly; avio_open2 is the first network operation.
    setDeadline();
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    const int opened = avio_open2(&m_media->pb, m_url.c_str(), AVIO_FLAG_WRITE,
                                  &m_media->interrupt_callback, &options);
    av_dict_free(&options);
    if (opened < 0) {
        if (m_stop_requested.load(std::memory_order_relaxed)) return Finished;
        logMediaResult(opened, "RTMPOutputNode: cannot connect");
        return fail(MediaError, "cannot connect to RTMP server");
    }
    setDeadline();
    const int header = avformat_write_header(m_media, nullptr);
    if (header < 0) {
        if (m_stop_requested.load(std::memory_order_relaxed)) return Finished;
        logMediaResult(header, "RTMPOutputNode: cannot write FLV header");
        return fail(MediaError, "cannot write FLV header");
    }
    m_header_written = true;
    return Success;
}

bool RTMPOutputNode::pace(const AVPacket& packet, AVRational time_base) {
    if (!m_realtime) return true;
    const auto stamp = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
    if (stamp == AV_NOPTS_VALUE) return true;
    const auto media_us = av_rescale_q(stamp, time_base, AVRational{1, 1000000});
    if (m_media_origin_us == AV_NOPTS_VALUE) {
        m_media_origin_us = media_us;
        m_clock_origin = std::chrono::steady_clock::now();
    }
    const auto elapsed = std::max<std::int64_t>(0, media_us - m_media_origin_us);
    const auto target = m_clock_origin + std::chrono::microseconds(elapsed);
    while (!m_stop_requested.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) return true;
        std::this_thread::sleep_for(std::min(target - now, std::chrono::steady_clock::duration(std::chrono::milliseconds(10))));
    }
    return false;
}

int RTMPOutputNode::process() {
    if (m_status != Success) return m_status;
    if (m_url.empty())
        return fail(NotConfigured, "RTMP output is not configured");
    auto message = input<MediaPacket>()->pop();
    if (m_stop_requested.load(std::memory_order_relaxed)) {
        finishMedia(false);
        return Finished;
    }
    if (!message) {
        // A closed queue without EOF represents cancellation; no synthetic EOF is sent.
        const int result = finishMedia(false);
        return result == Success ? Finished : result;
    }
    const int id = message->stream_id;
    if (id < 0) return fail(InvalidStreamId, "invalid stream ID", 0, id);
    switch (message->type) {
    case MediaPacket::Type::CONFIGFRAME: {
        if (m_header_written) return fail(LateConfigFrame, "CONFIGFRAME after FLV header", 0, id);
        if (m_streams.contains(id)) return fail(DuplicateStreamId, "duplicate CONFIGFRAME", 0, id);
        const auto& config = message->config;
        if (!config || message->packet || !config->codec || config->time_base.num <= 0 || config->time_base.den <= 0)
            return fail(InvalidConfigFrame, "invalid CONFIGFRAME", 0, id);
        const auto* codec = config->codec.get();
        if (!((codec->codec_type == AVMEDIA_TYPE_VIDEO && codec->codec_id == AV_CODEC_ID_H264) ||
              (codec->codec_type == AVMEDIA_TYPE_AUDIO && codec->codec_id == AV_CODEC_ID_AAC)))
            return fail(InvalidConfigFrame, "RTMP requires H.264 video or AAC audio", 0, id);
        if (!m_media) {
            const int result = avformat_alloc_output_context2(&m_media, nullptr, "flv", nullptr);
            if (result < 0 || !m_media) return fail(MediaError, "cannot allocate FLV context");
            m_media->interrupt_callback = {interrupt, this};
            m_media->max_interleave_delta = 100000; // Do not hold live packets for a sparse second stream.
        }
        for (const auto& [other_id, state] : m_streams) {
            (void)other_id;
            if (m_media->streams[state.output_index]->codecpar->codec_type == codec->codec_type)
                return fail(InvalidConfigFrame, "FLV supports one stream per media type", 0, id);
        }
        AVStream* stream = avformat_new_stream(m_media, nullptr);
        if (!stream) return fail(AllocationFailed, "cannot allocate output stream", 0, id);
        if (avcodec_parameters_copy(stream->codecpar, codec) < 0)
            return fail(MediaError, "cannot copy codec parameters", 0, id);
        stream->codecpar->codec_tag = 0;
        stream->time_base = config->time_base;
        m_streams.emplace(id, StreamState{config->time_base, stream->index});
        return Success;
    }
    case MediaPacket::Type::DATAFRAME: {
        const auto found = m_streams.find(id);
        if (found == m_streams.end()) return fail(FrameBeforeConfig, "DATAFRAME before CONFIGFRAME", 0, id);
        if (found->second.ended) return fail(FrameAfterEnd, "DATAFRAME after ENDFRAME", 0, id);
        if (!message->packet || message->config) return fail(InvalidDataFrame, "DATAFRAME requires an AVPacket", 0, id);
        if (const int header = writeHeader(); header != Success) {
            if (header == Finished) { finishMedia(false); return Finished; }
            return m_status;
        }
        if (!pace(*message->packet, found->second.time_base)) {
            finishMedia(false);
            return Finished;
        }
        AVPacketPtr copy = makeAVPacket();
        if (av_packet_ref(copy.get(), message->packet.get()) < 0)
            return fail(MediaError, "cannot reference packet", 0, id);
        copy->stream_index = found->second.output_index;
        av_packet_rescale_ts(copy.get(), found->second.time_base, m_media->streams[copy->stream_index]->time_base);
        copy->pos = -1;
        setDeadline();
        const int result = av_interleaved_write_frame(m_media, copy.get());
        if (result < 0) {
            if (m_stop_requested.load()) { finishMedia(false); return Finished; }
            logMediaResult(result, "RTMPOutputNode: cannot write FLV packet");
            return fail(MediaError, "cannot write FLV packet", 0, id);
        }
        ++m_packets_written;
        return Success;
    }
    case MediaPacket::Type::ENDFRAME: {
        const auto found = m_streams.find(id);
        if (found == m_streams.end()) return fail(FrameBeforeConfig, "ENDFRAME before CONFIGFRAME", 0, id);
        if (found->second.ended) return fail(FrameAfterEnd, "duplicate ENDFRAME", 0, id);
        if (message->config || message->packet) return fail(InvalidEndFrame, "ENDFRAME must be empty", 0, id);
        if (const int header = writeHeader(); header != Success) {
            if (header == Finished) { finishMedia(false); return Finished; }
            return m_status;
        }
        found->second.ended = true;
        if (++m_ended_streams == m_streams.size()) {
            const int result = finishMedia(true);
            return result == Success ? Finished : result;
        }
        return Success;
    }
    }
    return fail(UnknownFrameType, "unknown frame type", 0, id);
}

int RTMPOutputNode::finishMedia(bool completed) {
    if (!m_media) return Success;
    int result = Success;
    if (completed && m_header_written && !m_stop_requested.load()) {
        setDeadline();
        const int trailer = av_write_trailer(m_media);
        if (trailer < 0) {
            logMediaResult(trailer, "RTMPOutputNode: cannot write FLV trailer");
            result = fail(MediaError, "cannot write FLV trailer");
        }
    }
    if (m_media->pb) {
        setDeadline();
        const int closed = avio_closep(&m_media->pb);
        if (closed < 0 && !m_stop_requested.load() && result == Success) {
            logMediaResult(closed, "RTMPOutputNode: cannot close connection");
            result = fail(MediaError, "cannot close RTMP connection");
        }
    }
    std::fprintf(stderr, "[info] RTMPOutputNode: connection closed packets=%zu streams=%zu completed=%s\n",
                 m_packets_written, m_streams.size(), completed && result == Success ? "yes" : "no");
    avformat_free_context(m_media);
    m_media = nullptr;
    m_streams.clear();
    m_ended_streams = 0;
    m_header_written = false;
    m_media_origin_us = AV_NOPTS_VALUE;
    return result;
}
