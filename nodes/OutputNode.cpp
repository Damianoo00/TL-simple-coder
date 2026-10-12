#include "OutputNode.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

void OutputNode::configure(std::unordered_map<std::string, std::string> config) {
    const std::string& filename = config.at("filename");
    finishMedia();
    checkMedia(avformat_alloc_output_context2(&m_media, nullptr, "mp4", filename.c_str()),
               "Cannot create output MP4 " + filename);
    try {
        checkMedia(avio_open(&m_media->pb, filename.c_str(), AVIO_FLAG_WRITE),
                   "Cannot open output MP4 " + filename);
    } catch (...) {
        finishMedia();
        throw;
    }
    m_filename = filename;
}

OutputNode::~OutputNode() {
    // Join before destroying the file accessed by the worker thread.
    stop();
    finishMedia();
}

void OutputNode::writeHeader() {
    if (m_header_written) return;
    if (m_streams.empty()) throw std::runtime_error("MP4 output has no configured streams");
    checkMedia(avformat_write_header(m_media, nullptr), "Cannot write MP4 header");
    m_header_written = true;
}

int OutputNode::process() {
    auto message = input<MediaPacket>()->pop();
    if (!message) {
        // Queue closure also permits a partial file to be finalized on stop.
        if (!m_streams.empty()) writeHeader();
        finishMedia();
        return -1;
    }
    if (!m_media) throw std::runtime_error("OutputNode is not configured or is already finished");
    const int id = message->stream_id;
    if (id < 0) throw std::runtime_error("Invalid media stream ID");

    switch (message->type) {
    case MediaPacket::Type::CONFIGFRAME: {
        if (m_header_written) throw std::runtime_error("CONFIGFRAME received after MP4 header");
        if (m_streams.contains(id)) throw std::runtime_error("Duplicate CONFIGFRAME for stream");
        const auto& config = message->config;
        if (!config || message->packet || !config->codec || config->time_base.num <= 0 || config->time_base.den <= 0)
            throw std::runtime_error("Invalid CONFIGFRAME parameters");
        auto stream = avformat_new_stream(m_media, nullptr);
        if (!stream) throw std::bad_alloc();
        checkMedia(avcodec_parameters_copy(stream->codecpar, config->codec.get()),
                   "Cannot configure output stream");
        stream->codecpar->codec_tag = 0;
        stream->time_base = config->time_base;
        m_streams.emplace(id, StreamState{config->time_base, stream->index, false});
        return 0;
    }
    case MediaPacket::Type::DATAFRAME: {
        auto found = m_streams.find(id);
        if (found == m_streams.end()) throw std::runtime_error("DATAFRAME before CONFIGFRAME");
        if (found->second.ended) throw std::runtime_error("DATAFRAME after ENDFRAME");
        const auto& packet = message->packet;
        if (!packet || message->config) throw std::runtime_error("DATAFRAME requires an AVPacket");
        writeHeader();
        auto copy = makeAVPacket();
        checkMedia(av_packet_ref(copy.get(), packet.get()), "Cannot reference MP4 packet");
        copy->stream_index = found->second.output_index;
        av_packet_rescale_ts(copy.get(), found->second.time_base,
                            m_media->streams[copy->stream_index]->time_base);
        copy->pos = -1;
        checkMedia(av_interleaved_write_frame(m_media, copy.get()), "Cannot write MP4 packet");
        return 0;
    }
    case MediaPacket::Type::ENDFRAME: {
        auto found = m_streams.find(id);
        if (found == m_streams.end()) throw std::runtime_error("ENDFRAME before CONFIGFRAME");
        if (found->second.ended) throw std::runtime_error("Duplicate ENDFRAME for stream");
        if (message->config || message->packet)
            throw std::runtime_error("ENDFRAME must not contain data");
        writeHeader();
        found->second.ended = true;
        if (++m_ended_streams == m_streams.size()) {
            finishMedia();
            return -1;
        }
        return 0;
    }
    }
    throw std::runtime_error("Unknown media frame type");
}

void OutputNode::finishMedia() {
    if (!m_media) return;
    if (m_header_written) {
        const int result = av_write_trailer(m_media);
        if (result < 0) std::fprintf(stderr, "OutputNode: cannot finalize MP4 (%d)\n", result);
    }
    const int result = avio_closep(&m_media->pb);
    if (result < 0) std::fprintf(stderr, "OutputNode: cannot close MP4 (%d)\n", result);
    avformat_free_context(m_media);
    m_media = nullptr;
    m_header_written = false;
    m_streams.clear();
    m_ended_streams = 0;
}
