#include "InputNode.hpp"

void InputNode::configure(std::unordered_map<std::string, std::string> config) {
    const std::string& filename = config.at("filename");

    AVFormatContext* media = nullptr;
    checkMedia(avformat_open_input(&media, filename.c_str(), nullptr, nullptr),
               "Cannot open input MP4 " + filename);
    try {
        checkMedia(avformat_find_stream_info(media, nullptr), "Cannot read MP4 streams");
        std::vector<MediaStream> streams;
        for (unsigned i = 0; i < media->nb_streams; ++i) {
            auto codec = avcodec_parameters_alloc();
            if (!codec) throw std::bad_alloc();
            std::shared_ptr<AVCodecParameters> owned(codec,
                [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
            checkMedia(avcodec_parameters_copy(codec, media->streams[i]->codecpar),
                       "Cannot copy stream parameters");
            streams.push_back({std::move(owned), media->streams[i]->time_base});
        }
        m_filename = filename;
        avformat_close_input(&m_media);
        m_media = media;
        m_streams = std::move(streams);
        m_next_config = 0;
        m_next_end = 0;
        m_eof = false;
        return;
    } catch (...) {
        avformat_close_input(&media);
        throw;
    }
}

InputNode::~InputNode() {
    // Join before destroying the file accessed by the worker thread.
    stop();
    avformat_close_input(&m_media);
}

int InputNode::process() {
    if (m_stop_requested.load()) {
        return -1;
    }

    if (!m_media) throw std::runtime_error("InputNode is not configured");
    auto send = [this](MediaPacket message) {
        auto queue = output<MediaPacket>();
        return (!queue || queue->push(std::move(message))) ? 0 : -1;
    };
    if (m_next_config < m_streams.size()) {
        const auto id = m_next_config++;
        return send(MediaPacket::configFrame(static_cast<int>(id), m_streams.at(id)));
    }
    if (!m_eof) {
        auto packet = makeAVPacket();
        const int result = av_read_frame(m_media, packet.get());
        if (result != AVERROR_EOF) {
            checkMedia(result, "Cannot read media " + m_filename);
            const int id = packet->stream_index;
            return send(MediaPacket::dataFrame(id, std::move(packet)));
        }
        m_eof = true;
    }
    if (m_next_end < m_streams.size()) {
        return send(MediaPacket::endFrame(static_cast<int>(m_next_end++)));
    }
    return -1;
}
