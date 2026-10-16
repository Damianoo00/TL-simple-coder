#include "OutputNode.hpp"

#include <utility>

int OutputNode::configure(std::unordered_map<std::string, std::string> config) {
    const auto entry = config.find("filename");
    if (entry == config.end() || entry->second.empty())
        return fail(InvalidConfiguration, "missing non-empty filename");
    if (finishMedia() < Finished) return m_status;
    const auto& filename = entry->second;
    if (logMediaResult(avformat_alloc_output_context2(&m_media, nullptr, "mp4", filename.c_str()),
                       "OutputNode: cannot create " + filename) < 0 || !m_media)
        return fail(MediaError, "cannot create output context");
    if (logMediaResult(avio_open(&m_media->pb, filename.c_str(), AVIO_FLAG_WRITE),
                       "OutputNode: cannot open " + filename) < 0) {
        finishMedia();
        return fail(MediaError, "cannot open output file");
    }
    m_filename = filename;
    m_packets_written = 0;
    m_status = Success;
    std::fprintf(stderr, "[info] OutputNode: opened file=\"%s\"\n", m_filename.c_str());
    return Success;
}

OutputNode::~OutputNode() {
    // Join before destroying the file accessed by the worker thread.
    stop();
    finishMedia();
}

int OutputNode::writeHeader() {
    if (m_header_written) return Success;
    if (m_streams.empty()) return fail(InvalidConfigFrame, "MP4 output has no configured streams");
    if (logMediaResult(avformat_write_header(m_media, nullptr), "OutputNode: Cannot write MP4 header " + m_filename) < 0)
        return fail(MediaError, "Cannot write MP4 header");
    m_header_written = true;
    return Success;
}

int OutputNode::process() {
    if (m_status != Success) return m_status;
    if (!m_media) return fail(NotConfigured, "output is not configured or already finished");
    auto message = input<MediaPacket>()->pop();
    if (!message) {
        // Queue closure also permits a partial file to be finalized on stop.
        if (!m_streams.empty() && writeHeader() != Success) {
            finishMedia();
            return m_status;
        }
        const int result = finishMedia("queue_closed");
        return result == Success ? Finished : result;
    }
    const int id = message->stream_id;
    if (id < 0) return fail(InvalidStreamId, "Invalid media stream ID", 0, id);

    switch (message->type) {
    case MediaPacket::Type::CONFIGFRAME: {
        if (m_header_written) return fail(LateConfigFrame, "CONFIGFRAME received after MP4 header", 0, id);
        if (m_streams.contains(id)) return fail(DuplicateStreamId, "Duplicate CONFIGFRAME for stream", 0, id);
        const auto& config = message->config;
        if (!config || message->packet || !config->codec || config->time_base.num <= 0 || config->time_base.den <= 0)
            return fail(InvalidConfigFrame, "Invalid CONFIGFRAME parameters", 0, id);
        auto stream = avformat_new_stream(m_media, nullptr);
        if (!stream) return fail(AllocationFailed, "cannot allocate output stream", 0, id);
        if (logMediaResult(avcodec_parameters_copy(stream->codecpar, config->codec.get()), "OutputNode: Cannot configure output stream " + m_filename) < 0)
            return fail(MediaError, "Cannot configure output stream", 0, id);
        stream->codecpar->codec_tag = 0;
        stream->time_base = config->time_base;
        m_streams.emplace(id, StreamState{config->time_base, stream->index, false});
        return 0;
    }
    case MediaPacket::Type::DATAFRAME: {
        auto found = m_streams.find(id);
        if (found == m_streams.end()) return fail(FrameBeforeConfig, "DATAFRAME before CONFIGFRAME", 0, id);
        if (found->second.ended) return fail(FrameAfterEnd, "DATAFRAME after ENDFRAME", 0, id);
        const auto& packet = message->packet;
        if (!packet || message->config) return fail(InvalidDataFrame, "DATAFRAME requires an AVPacket", 0, id);
        if (writeHeader() != Success) return m_status;
        auto raw = av_packet_alloc();
        if (!raw) return fail(AllocationFailed, "cannot allocate output packet", 0, id);
        AVPacketPtr copy(raw, [](AVPacket* p) { av_packet_free(&p); });
        if (logMediaResult(av_packet_ref(copy.get(), packet.get()), "OutputNode: Cannot reference MP4 packet " + m_filename) < 0)
            return fail(MediaError, "Cannot reference MP4 packet", 0, id);
        copy->stream_index = found->second.output_index;
        av_packet_rescale_ts(copy.get(), found->second.time_base,
                            m_media->streams[copy->stream_index]->time_base);
        copy->pos = -1;
        if (logMediaResult(av_interleaved_write_frame(m_media, copy.get()), "OutputNode: Cannot write MP4 packet " + m_filename) < 0)
            return fail(MediaError, "Cannot write MP4 packet", 0, id);
        ++m_packets_written;
        return 0;
    }
    case MediaPacket::Type::ENDFRAME: {
        auto found = m_streams.find(id);
        if (found == m_streams.end()) return fail(FrameBeforeConfig, "ENDFRAME before CONFIGFRAME", 0, id);
        if (found->second.ended) return fail(FrameAfterEnd, "Duplicate ENDFRAME for stream", 0, id);
        if (message->config || message->packet)
            return fail(InvalidEndFrame, "ENDFRAME must not contain data", 0, id);
        if (writeHeader() != Success) return m_status;
        found->second.ended = true;
        if (++m_ended_streams == m_streams.size()) {
            const int result = finishMedia("end_of_stream");
            return result == Success ? Finished : result;
        }
        return 0;
    }
    }
    return fail(UnknownFrameType, "Unknown media frame type", 0, id);
}

int OutputNode::finishMedia(const char* reason) {
    if (!m_media) return Success;
    int status = Success;
    const bool had_output = m_media->pb != nullptr;
    if (m_header_written && logMediaResult(av_write_trailer(m_media),
                                          "OutputNode: cannot finalize " + m_filename) < 0)
        status = fail(MediaError, "cannot finalize output file");
    if (m_media->pb && logMediaResult(avio_closep(&m_media->pb),
                                    "OutputNode: cannot close " + m_filename) < 0)
        status = fail(MediaError, "cannot close output file");
    if (had_output && !m_filename.empty())
        std::fprintf(stderr, "[info] OutputNode: output closed file=\"%s\" packets=%zu streams=%zu ended_streams=%zu finalized=%s reason=%s\n",
                     m_filename.c_str(), m_packets_written, m_streams.size(), m_ended_streams,
                     m_header_written && status == Success ? "yes" : "no", reason);
    avformat_free_context(m_media);
    m_media = nullptr;
    m_header_written = false;
    m_streams.clear();
    m_ended_streams = 0;
    return status;
}
