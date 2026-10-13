#include "EncoderNode.hpp"
#include <algorithm>
#include <limits>

EncoderNode::EncoderNode(AVMediaType type) : m_type(type),
    m_codec_name(type == AVMEDIA_TYPE_VIDEO ? "libx264" : "aac"),
    m_bitrate(type == AVMEDIA_TYPE_VIDEO ? 2000000 : 128000) {
    addInput<MediaFrame>();
    addOutput<MediaPacket>();
}
EncoderNode::~EncoderNode() { stop(); reset(); }
void EncoderNode::reset() {
    avcodec_free_context(&m_codec);
    av_audio_fifo_free(m_fifo);
    m_fifo = nullptr;
    m_stream_id = -1;
    m_audio_pts = AV_NOPTS_VALUE;
    m_finished = false;
    m_packets = m_frames = 0;
}

int EncoderNode::configure(std::unordered_map<std::string, std::string> config) {
    auto name = m_type == AVMEDIA_TYPE_VIDEO ? std::string("libx264") : std::string("aac");
    int64_t bitrate = m_type == AVMEDIA_TYPE_VIDEO ? 2000000 : 128000;
    int threads = 1, gop = 12, b_frames = 0;
    for (const auto& [key, value] : config) {
        if (key == "codec") { name = value; continue; }
        int64_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            return fail(InvalidConfiguration, "numeric encoder option is not an integer");
        if (key == "bit_rate" && number > 0) bitrate = number;
        else if (key == "threads" && number > 0 && number <= std::numeric_limits<int>::max()) threads = static_cast<int>(number);
        else if (m_type == AVMEDIA_TYPE_VIDEO && key == "gop_size" && number > 0 && number <= std::numeric_limits<int>::max()) gop = static_cast<int>(number);
        else if (m_type == AVMEDIA_TYPE_VIDEO && key == "max_b_frames" && number >= 0 && number <= 16) b_frames = static_cast<int>(number);
        else return fail(InvalidConfiguration, "unknown encoder option or value outside supported range");
    }
    const auto codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec || codec->type != m_type) {
        std::fprintf(stderr, "%s: requested codec=%s\n", nodeType(), name.c_str());
        return fail(InvalidConfiguration, "encoder unavailable or wrong media type");
    }
    reset();
    m_codec_name = std::move(name);
    m_bitrate = bitrate;
    m_threads = threads;
    m_gop = gop;
    m_b_frames = b_frames;
    m_status = Success;
    return Success;
}

int EncoderNode::send(MediaPacket message) {
    auto queue = output<MediaPacket>();
    if (!queue) return fail(OutputNotConnected, "encoded packet output is not connected", 0, m_stream_id);
    return queue->push(std::move(message)) ? Success : Finished;
}

int EncoderNode::open(const MediaFrame& message) {
    if (m_codec) return fail(DuplicateStreamId, "single-stream encoder already configured", 0, message.stream_id);
    const auto& config = message.config;
    if (message.stream_id < 0 || !config || !config->codec || message.frame ||
        config->time_base.num <= 0 || config->time_base.den <= 0 || config->codec->codec_type != m_type)
        return fail(InvalidConfigFrame, "invalid decoded CONFIGFRAME or wrong media type", 0, message.stream_id);
    const auto codec = avcodec_find_encoder_by_name(m_codec_name.c_str());
    if (!codec || codec->type != m_type) return fail(MediaError, "requested encoder unavailable", 0, message.stream_id);
    m_codec = avcodec_alloc_context3(codec);
    if (!m_codec) return fail(AllocationFailed, "cannot allocate encoder context", 0, message.stream_id);
    m_stream_id = message.stream_id;
    m_input_time_base = config->time_base;
    const auto& parameters = *config->codec;
    m_codec->bit_rate = m_bitrate;
    m_codec->thread_count = m_threads;
    m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    m_codec->time_base = config->time_base;
    if (m_type == AVMEDIA_TYPE_VIDEO) {
        if (parameters.width <= 0 || parameters.height <= 0 || parameters.format < 0)
            return fail(InvalidConfigFrame, "decoded video requires dimensions and pixel format", 0, m_stream_id);
        m_codec->width = parameters.width;
        m_codec->height = parameters.height;
        m_codec->pix_fmt = static_cast<AVPixelFormat>(parameters.format);
        m_codec->sample_aspect_ratio = parameters.sample_aspect_ratio;
        m_codec->framerate = config->frame_rate;
        // Keep timestamp precision, including variable frame rate. MPEG-4
        // restricts the time-base denominator to 16 bits.
        if (codec->id == AV_CODEC_ID_MPEG4 && m_codec->time_base.den > 65535)
            av_reduce(&m_codec->time_base.num, &m_codec->time_base.den,
                      config->time_base.num, config->time_base.den, 65535);
        m_codec->gop_size = m_gop;
        m_codec->max_b_frames = m_b_frames;
    } else {
        if (parameters.sample_rate <= 0 || parameters.ch_layout.nb_channels <= 0 || parameters.format < 0)
            return fail(InvalidConfigFrame, "decoded audio requires sample rate, layout and sample format", 0, m_stream_id);
        m_codec->sample_rate = parameters.sample_rate;
        m_codec->sample_fmt = static_cast<AVSampleFormat>(parameters.format);
        m_codec->time_base = {1, parameters.sample_rate};
        if (logMediaResult(av_channel_layout_copy(&m_codec->ch_layout, &parameters.ch_layout), nodeType()) < 0)
            return fail(MediaError, "cannot copy encoder channel layout", 0, m_stream_id);
    }
    // Validate the unchanged decoded format before opening the encoder.
    const void* formats = nullptr;
    int count = 0;
    if (logMediaResult(avcodec_get_supported_config(m_codec, codec,
            m_type == AVMEDIA_TYPE_VIDEO ? AV_CODEC_CONFIG_PIX_FORMAT : AV_CODEC_CONFIG_SAMPLE_FORMAT,
            0, &formats, &count), nodeType()) < 0)
        return fail(MediaError, "cannot query encoder formats", 0, m_stream_id);
    bool supported = !formats;
    for (int i = 0; formats && i < count; ++i) {
        const int format = m_type == AVMEDIA_TYPE_VIDEO ? static_cast<int>(static_cast<const AVPixelFormat*>(formats)[i])
                                                     : static_cast<int>(static_cast<const AVSampleFormat*>(formats)[i]);
        if (format == parameters.format) supported = true;
    }
    if (!supported) return fail(InvalidConfigFrame, "encoder does not support input format; conversion required", 0, m_stream_id);
    if (logMediaResult(avcodec_open2(m_codec, codec, nullptr), nodeType()) < 0)
        return fail(MediaError, "cannot open encoder", 0, m_stream_id);
    if (m_type == AVMEDIA_TYPE_AUDIO) {
        m_fifo = av_audio_fifo_alloc(m_codec->sample_fmt, m_codec->ch_layout.nb_channels,
                                     std::max(1, m_codec->frame_size));
        if (!m_fifo) return fail(AllocationFailed, "cannot allocate audio FIFO", 0, m_stream_id);
    }
    auto raw = avcodec_parameters_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate encoded parameters", 0, m_stream_id);
    std::shared_ptr<AVCodecParameters> owned(raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    if (logMediaResult(avcodec_parameters_from_context(raw, m_codec), nodeType()) < 0)
        return fail(MediaError, "cannot export encoder parameters", 0, m_stream_id);
    const int sent = send(MediaPacket::configFrame(m_stream_id, {std::move(owned), m_codec->time_base, m_codec->framerate}));
    if (sent == Success)
        std::fprintf(stderr, "[info] %s: encoder opened codec=%s stream_id=%d bit_rate=%lld threads=%d\n",
                     nodeType(), codec->name, m_stream_id, static_cast<long long>(m_bitrate), m_threads);
    return sent;
}

int EncoderNode::receive(bool draining) {
    for (;;) {
        auto raw = av_packet_alloc();
        if (!raw) return fail(AllocationFailed, "cannot allocate encoded packet", 0, m_stream_id);
        AVPacketPtr packet(raw, [](AVPacket* p) { av_packet_free(&p); });
        const int result = avcodec_receive_packet(m_codec, raw);
        if (result == AVERROR_EOF) return Success;
        if (result == AVERROR(EAGAIN))
            return draining ? fail(MediaError, "encoder requested input while draining", 0, m_stream_id) : Success;
        if (logMediaResult(result, nodeType()) < 0)
            return fail(MediaError, "cannot receive encoded packet", 0, m_stream_id);
        raw->stream_index = m_stream_id;
        if (m_type == AVMEDIA_TYPE_VIDEO && raw->duration <= 0 &&
            m_codec->framerate.num > 0 && m_codec->framerate.den > 0)
            raw->duration = av_rescale_q(1, av_inv_q(m_codec->framerate), m_codec->time_base);
        const int sent = send(MediaPacket::dataFrame(m_stream_id, std::move(packet)));
        if (sent != Success) return sent;
        ++m_packets;
    }
}

int EncoderNode::encode(AVFrame* frame) {
    if (logMediaResult(avcodec_send_frame(m_codec, frame), nodeType()) < 0)
        return fail(MediaError, "cannot send frame to encoder", 0, m_stream_id);
    return receive(frame == nullptr);
}

int EncoderNode::audio(AVFrame* frame, bool draining) {
    if (frame) {
        const int buffered = av_audio_fifo_size(m_fifo);
        const int64_t pts = frame->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
            av_rescale_q(frame->pts, m_input_time_base, m_codec->time_base);
        if (m_audio_pts == AV_NOPTS_VALUE) m_audio_pts = pts == AV_NOPTS_VALUE ? 0 : pts;
        else if (pts != AV_NOPTS_VALUE && pts != m_audio_pts + buffered)
            return fail(InvalidDataFrame, "audio timestamps are discontinuous; resampling or gap handling required", 0, m_stream_id);
        if (av_audio_fifo_write(m_fifo, reinterpret_cast<void**>(frame->extended_data), frame->nb_samples) != frame->nb_samples)
            return fail(MediaError, "cannot buffer audio samples", 0, m_stream_id);
    }
    for (;;) {
        const int available = av_audio_fifo_size(m_fifo);
        const int chunk = m_codec->frame_size > 0 ? m_codec->frame_size : available;
        if (!available || (!draining && available < chunk)) break;
        const int samples = std::min(available, chunk);
        auto raw = av_frame_alloc();
        if (!raw) return fail(AllocationFailed, "cannot allocate audio encoder frame", 0, m_stream_id);
        AVFramePtr block(raw, [](AVFrame* p) { av_frame_free(&p); });
        raw->format = m_codec->sample_fmt;
        raw->sample_rate = m_codec->sample_rate;
        raw->nb_samples = samples;
        if (samples < chunk && !(m_codec->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) raw->nb_samples = chunk;
        if (logMediaResult(av_channel_layout_copy(&raw->ch_layout, &m_codec->ch_layout), nodeType()) < 0 ||
            logMediaResult(av_frame_get_buffer(raw, 0), nodeType()) < 0)
            return fail(MediaError, "cannot allocate audio frame data", 0, m_stream_id);
        if (av_audio_fifo_read(m_fifo, reinterpret_cast<void**>(raw->extended_data), samples) != samples)
            return fail(MediaError, "cannot read audio FIFO", 0, m_stream_id);
        if (raw->nb_samples > samples &&
            logMediaResult(av_samples_set_silence(raw->extended_data, samples, raw->nb_samples - samples,
                                                 raw->ch_layout.nb_channels, m_codec->sample_fmt), nodeType()) < 0)
            return fail(MediaError, "cannot pad final audio frame", 0, m_stream_id);
        raw->pts = m_audio_pts;
        m_audio_pts += raw->nb_samples;
        const int result = encode(raw);
        if (result != Success) return result;
    }
    return Success;
}

int EncoderNode::process() {
    if (m_status != Success) return m_status;
    if (m_finished || m_stop_requested.load()) return Finished;
    if (!output<MediaPacket>()) return fail(OutputNotConnected, "encoded packet output is not connected");
    auto message = input<MediaFrame>()->pop();
    if (!message) { m_finished = true; return Finished; }
    if (message->type == MediaFrame::Type::CONFIGFRAME) return open(*message);
    if (!m_codec) return fail(FrameBeforeConfig, "frame before CONFIGFRAME", 0, message->stream_id);
    if (message->stream_id != m_stream_id)
        return fail(StreamIdChanged, "single-stream encoder received another stream ID", 0, message->stream_id);
    if (message->type == MediaFrame::Type::ENDFRAME) {
        if (message->frame || message->config) return fail(InvalidEndFrame, "ENDFRAME contains data", 0, m_stream_id);
        if (m_type == AVMEDIA_TYPE_AUDIO) {
            const int result = audio(nullptr, true);
            if (result != Success) return result;
        }
        const int result = encode(nullptr);
        if (result != Success) return result;
        const int sent = send(MediaPacket::endFrame(m_stream_id));
        m_finished = true;
        if (sent == Success)
            std::fprintf(stderr, "[info] %s: encoding finished stream_id=%d frames=%zu packets=%zu\n",
                         nodeType(), m_stream_id, m_frames, m_packets);
        return sent == Success ? Finished : sent;
    }
    if (message->type != MediaFrame::Type::DATAFRAME)
        return fail(UnknownFrameType, "unknown frame type", 0, m_stream_id);
    const auto& frame = message->frame;
    if (!frame || message->config || !frame->data[0])
        return fail(InvalidDataFrame, "DATAFRAME requires allocated AVFrame without config", 0, m_stream_id);
    if (m_type == AVMEDIA_TYPE_AUDIO) {
        if (frame->format != m_codec->sample_fmt || frame->sample_rate != m_codec->sample_rate || frame->nb_samples <= 0 ||
            av_channel_layout_compare(&frame->ch_layout, &m_codec->ch_layout))
            return fail(InvalidDataFrame, "audio format differs from CONFIGFRAME", 0, m_stream_id);
        const int result = audio(frame.get(), false);
        if (result == Success) ++m_frames;
        return result;
    }
    if (frame->format != m_codec->pix_fmt || frame->width != m_codec->width || frame->height != m_codec->height)
        return fail(InvalidDataFrame, "video format differs from CONFIGFRAME", 0, m_stream_id);
    auto raw = av_frame_clone(frame.get());
    if (!raw) return fail(AllocationFailed, "cannot reference video frame", 0, m_stream_id);
    AVFramePtr copy(raw, [](AVFrame* p) { av_frame_free(&p); });
    if (raw->pts != AV_NOPTS_VALUE) raw->pts = av_rescale_q(raw->pts, m_input_time_base, m_codec->time_base);
    if (raw->duration > 0) raw->duration = av_rescale_q(raw->duration, m_input_time_base, m_codec->time_base);
    raw->time_base = m_codec->time_base;
    raw->pict_type = AV_PICTURE_TYPE_NONE;
    const int result = encode(raw);
    if (result == Success) ++m_frames;
    return result;
}
