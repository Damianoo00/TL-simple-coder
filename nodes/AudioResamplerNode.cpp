#include "AudioResamplerNode.hpp"
#include <algorithm>

AudioResamplerNode::~AudioResamplerNode() { stop(); swr_free(&m_resampler); }

int AudioResamplerNode::configure(std::unordered_map<std::string, std::string> config) {
    int sample_rate = 0;
    auto format = AV_SAMPLE_FMT_FLTP;
    std::string layout;
    for (const auto& [key, value] : config) {
        if (key == "sample_rate") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), sample_rate);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || sample_rate <= 0)
                return fail(InvalidConfiguration, "sample_rate must be a positive integer");
        } else if (key == "sample_format") {
            format = av_get_sample_fmt(value.c_str());
            if (format == AV_SAMPLE_FMT_NONE) return fail(InvalidConfiguration, "unknown output sample_format");
        } else if (key == "channel_layout") {
            AVChannelLayout parsed{};
            const int result = av_channel_layout_from_string(&parsed, value.c_str());
            const bool valid = result >= 0 && av_channel_layout_check(&parsed) && parsed.order != AV_CHANNEL_ORDER_UNSPEC;
            av_channel_layout_uninit(&parsed);
            if (!valid) return fail(InvalidConfiguration, "channel_layout must describe named channels, e.g. mono or stereo");
            layout = value;
        } else return fail(InvalidConfiguration, "unknown resampler option");
    }
    swr_free(&m_resampler);
    m_output_parameters.reset();
    m_sample_rate = sample_rate; m_format = format; m_layout = std::move(layout);
    m_next_pts = m_next_input_pts = AV_NOPTS_VALUE;
    m_input_samples = m_output_samples = 0;
    resetStream();
    return Success;
}

int AudioResamplerNode::open(const MediaStream& stream) {
    const auto& input = *stream.codec;
    if (input.sample_rate <= 0 || input.format < 0 || input.format >= AV_SAMPLE_FMT_NB || !av_channel_layout_check(&input.ch_layout))
        return fail(InvalidConfigFrame, "invalid source sample rate, sample format or channel layout", 0, m_stream_id);
    auto raw = avcodec_parameters_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate resampled parameters", 0, m_stream_id);
    m_output_parameters = {raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); }};
    if (logMediaResult(avcodec_parameters_copy(raw, &input), nodeType()) < 0)
        return fail(MediaError, "cannot copy resampled parameters", 0, m_stream_id);
    raw->sample_rate = m_sample_rate ? m_sample_rate : input.sample_rate;
    raw->format = m_format;
    if (!m_layout.empty()) {
        av_channel_layout_uninit(&raw->ch_layout);
        if (logMediaResult(av_channel_layout_from_string(&raw->ch_layout, m_layout.c_str()), nodeType()) < 0)
            return fail(MediaError, "cannot create output channel layout", 0, m_stream_id);
    }
    if (logMediaResult(swr_alloc_set_opts2(&m_resampler, &raw->ch_layout, m_format, raw->sample_rate,
            &input.ch_layout, static_cast<AVSampleFormat>(input.format), input.sample_rate, 0, nullptr), nodeType()) < 0 ||
        logMediaResult(swr_init(m_resampler), nodeType()) < 0)
        return fail(MediaError, "cannot initialize resampler", 0, m_stream_id);
    const int sent = send(MediaFrame::configFrame(m_stream_id, {m_output_parameters, {1, raw->sample_rate}}));
    if (sent == Success)
        std::fprintf(stderr, "[info] AudioResamplerNode: resampler opened stream_id=%d input=%dHz/%s/%dch output=%dHz/%s/%dch\n",
                     m_stream_id, input.sample_rate, av_get_sample_fmt_name(static_cast<AVSampleFormat>(input.format)), input.ch_layout.nb_channels,
                     raw->sample_rate, av_get_sample_fmt_name(m_format), raw->ch_layout.nb_channels);
    return sent;
}

int AudioResamplerNode::convert(const AVFrame* frame, bool& produced) {
    produced = false;
    const int capacity = swr_get_out_samples(m_resampler, frame ? frame->nb_samples : 0);
    if (logMediaResult(capacity, nodeType()) < 0)
        return fail(MediaError, "cannot determine resampler output size", 0, m_stream_id);
    auto raw = av_frame_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate resampled frame", 0, m_stream_id);
    AVFramePtr output(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->format = m_format;
    raw->sample_rate = m_output_parameters->sample_rate;
    raw->nb_samples = std::max(1, capacity);
    if (logMediaResult(av_channel_layout_copy(&raw->ch_layout, &m_output_parameters->ch_layout), nodeType()) < 0 ||
        logMediaResult(av_frame_get_buffer(raw, 0), nodeType()) < 0 ||
        (frame && logMediaResult(av_frame_copy_props(raw, frame), nodeType()) < 0))
        return fail(MediaError, "cannot allocate resampled frame data or copy properties", 0, m_stream_id);
    // copy_props also copies sample_rate from the source; restore target rate.
    raw->sample_rate = m_output_parameters->sample_rate;
    const auto input = frame ? const_cast<const uint8_t**>(frame->extended_data) : nullptr;
    const int samples = swr_convert(m_resampler, raw->extended_data, raw->nb_samples, input, frame ? frame->nb_samples : 0);
    if (logMediaResult(samples, nodeType()) < 0)
        return fail(MediaError, "cannot resample audio", 0, m_stream_id);
    if (!samples) return Success;
    raw->nb_samples = samples;
    raw->pts = m_next_pts;
    raw->best_effort_timestamp = raw->pts;
    raw->pkt_dts = AV_NOPTS_VALUE;
    raw->duration = samples;
    raw->time_base = {1, raw->sample_rate};
    const int sent = send(MediaFrame::dataFrame(m_stream_id, std::move(output)));
    if (sent == Success) {
        produced = true;
        m_next_pts += samples;
        m_output_samples += samples;
    }
    return sent;
}

int AudioResamplerNode::transform(const AVFrame& frame) {
    const auto& input = *m_input_config->codec;
    if (frame.format != input.format || frame.sample_rate != input.sample_rate || frame.nb_samples <= 0 ||
        !frame.extended_data || av_channel_layout_compare(&frame.ch_layout, &input.ch_layout))
        return fail(InvalidDataFrame, "audio format differs from CONFIGFRAME", 0, m_stream_id);
    const auto pts = frame.pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
        av_rescale_q(frame.pts, m_input_config->time_base, AVRational{1, input.sample_rate});
    if (m_next_pts == AV_NOPTS_VALUE) {
        m_next_pts = frame.pts == AV_NOPTS_VALUE ? 0 :
            av_rescale_q(frame.pts, m_input_config->time_base, AVRational{1, m_output_parameters->sample_rate});
        m_next_input_pts = pts == AV_NOPTS_VALUE ? 0 : pts;
    } else if (pts != AV_NOPTS_VALUE && (static_cast<long double>(pts) - m_next_input_pts > 1 ||
                                        static_cast<long double>(pts) - m_next_input_pts < -1))
        return fail(InvalidDataFrame, "audio timestamps are discontinuous; gap handling required", 0, m_stream_id);
    bool produced = false;
    const int result = convert(&frame, produced);
    if (result == Success) {
        m_next_input_pts += frame.nb_samples;
        m_input_samples += frame.nb_samples;
    }
    return result;
}

int AudioResamplerNode::flush() {
    if (m_next_pts != AV_NOPTS_VALUE) {
        bool produced;
        do {
            const int result = convert(nullptr, produced);
            if (result != Success) return result;
        } while (produced);
    }
    std::fprintf(stderr, "[info] AudioResamplerNode: resampler drained stream_id=%d input_samples=%lld output_samples=%lld\n",
                 m_stream_id, static_cast<long long>(m_input_samples), static_cast<long long>(m_output_samples));
    return Success;
}
