#include "AudioGapFillerNode.hpp"
#include "VideoGapFillerNode.hpp"
#include <algorithm>
#include <limits>

namespace {
bool parseLimit(const std::unordered_map<std::string, std::string>& config, int& limit) {
    limit = 10000;
    for (const auto& [key, value] : config) {
        if (key != "max_gap_ms") return false;
        auto result = std::from_chars(value.data(), value.data() + value.size(), limit);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || limit <= 0)
            return false;
    }
    return true;
}
AVFramePtr clone(const AVFrame& frame) {
    return AVFramePtr(av_frame_clone(&frame), [](AVFrame* p) { av_frame_free(&p); });
}
bool validEnd(int64_t pts, int64_t duration) {
    return duration > 0 && pts <= std::numeric_limits<int64_t>::max() - duration;
}
}

int AudioGapFillerNode::configure(std::unordered_map<std::string, std::string> config) {
    if (!parseLimit(config, m_max_gap_ms))
        return fail(InvalidConfiguration, "expected positive max_gap_ms (default 10000)");
    m_next_pts = AV_NOPTS_VALUE;
    resetStream();
    return Success;
}
int AudioGapFillerNode::open(const MediaStream& stream) {
    const auto& c = *stream.codec;
    if (c.sample_rate <= 0 || c.format < 0 || c.format >= AV_SAMPLE_FMT_NB || !av_channel_layout_check(&c.ch_layout))
        return fail(InvalidConfigFrame, "invalid audio format", 0, m_stream_id);
    return send(MediaFrame::configFrame(m_stream_id, {stream.codec, {1, c.sample_rate}, stream.frame_rate}));
}
int AudioGapFillerNode::transform(const AVFrame& frame) {
    const auto& c = *m_input_config->codec;
    if (frame.pts == AV_NOPTS_VALUE || frame.nb_samples <= 0 || frame.format != c.format ||
        frame.sample_rate != c.sample_rate || !frame.extended_data ||
        av_channel_layout_compare(&frame.ch_layout, &c.ch_layout))
        return fail(InvalidDataFrame, "invalid audio frame or missing PTS", 0, m_stream_id);
    int64_t pts = av_rescale_q(frame.pts, m_input_config->time_base, AVRational{1, c.sample_rate});
    if (m_next_pts == AV_NOPTS_VALUE) m_next_pts = pts;
    const long double gap = static_cast<long double>(pts) - m_next_pts;
    if (gap < -1 || gap * 1000 > static_cast<long double>(m_max_gap_ms) * c.sample_rate)
        return fail(InvalidDataFrame, "audio overlap or gap exceeds max_gap_ms", 0, m_stream_id);
    // Match the resampler's one-sample rounding tolerance.
    if (gap <= 1) pts = m_next_pts;
    while (m_next_pts < pts) {
        if (m_stop_requested.load()) return Finished;
        AVFramePtr silence(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
        if (!silence) return fail(AllocationFailed, "cannot allocate silence", 0, m_stream_id);
        silence->format = c.format;
        silence->sample_rate = c.sample_rate;
        silence->nb_samples = static_cast<int>(std::min<int64_t>(4096, pts - m_next_pts));
        if (av_channel_layout_copy(&silence->ch_layout, &c.ch_layout) < 0 ||
            av_frame_get_buffer(silence.get(), 0) < 0 ||
            av_samples_set_silence(silence->extended_data, 0, silence->nb_samples,
                                  c.ch_layout.nb_channels, static_cast<AVSampleFormat>(c.format)) < 0)
            return fail(AllocationFailed, "cannot create silence samples", 0, m_stream_id);
        silence->pts = silence->best_effort_timestamp = m_next_pts;
        silence->duration = silence->nb_samples;
        silence->time_base = {1, c.sample_rate};
        const int samples = silence->nb_samples;
        int result = send(MediaFrame::dataFrame(m_stream_id, std::move(silence)));
        if (result != Success) return result;
        m_next_pts += samples;
    }
    if (!validEnd(pts, frame.nb_samples))
        return fail(InvalidDataFrame, "audio timestamp overflow", 0, m_stream_id);
    auto output = clone(frame);
    if (!output) return fail(AllocationFailed, "cannot clone audio frame", 0, m_stream_id);
    output->pts = output->best_effort_timestamp = pts;
    output->pkt_dts = AV_NOPTS_VALUE;
    output->duration = frame.nb_samples;
    output->time_base = {1, c.sample_rate};
    int result = send(MediaFrame::dataFrame(m_stream_id, std::move(output)));
    if (result == Success) m_next_pts = pts + frame.nb_samples;
    return result;
}

int VideoGapFillerNode::configure(std::unordered_map<std::string, std::string> config) {
    if (!parseLimit(config, m_max_gap_ms))
        return fail(InvalidConfiguration, "expected positive max_gap_ms (default 10000)");
    m_last.reset(); m_next_pts = AV_NOPTS_VALUE; m_step = 0;
    resetStream();
    return Success;
}
int VideoGapFillerNode::open(const MediaStream& stream) {
    const auto& c = *stream.codec;
    if (c.width <= 0 || c.height <= 0 || c.format < 0 || c.format >= AV_PIX_FMT_NB)
        return fail(InvalidConfigFrame, "invalid video format", 0, m_stream_id);
    if (stream.frame_rate.num > 0 && stream.frame_rate.den > 0)
        m_step = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(stream.frame_rate), stream.time_base));
    return send(MediaFrame::configFrame(m_stream_id, stream));
}
int VideoGapFillerNode::transform(const AVFrame& frame) {
    const auto& c = *m_input_config->codec;
    const auto tb = m_input_config->time_base;
    const int64_t duration = frame.duration > 0 ? frame.duration : m_step;
    if (frame.pts == AV_NOPTS_VALUE || frame.format != c.format || frame.width != c.width ||
        frame.height != c.height || !validEnd(frame.pts, duration))
        return fail(InvalidDataFrame, "invalid video frame, PTS or duration (frame rate required as fallback)", 0, m_stream_id);
    if (m_last) {
        const long double gap = static_cast<long double>(frame.pts) - m_next_pts;
        // One tick may result from rounding frame durations to the stream time base.
        if (frame.pts <= m_last->pts || gap < -1 ||
            gap * tb.num * 1000 > static_cast<long double>(m_max_gap_ms) * tb.den)
            return fail(InvalidDataFrame, "video overlap or gap exceeds max_gap_ms", 0, m_stream_id);
        while (frame.pts > m_next_pts) {
            if (m_stop_requested.load()) return Finished;
            auto repeat = clone(*m_last);
            if (!repeat) return fail(AllocationFailed, "cannot repeat video frame", 0, m_stream_id);
            repeat->pts = repeat->best_effort_timestamp = m_next_pts;
            repeat->pkt_dts = AV_NOPTS_VALUE;
            repeat->duration = std::min(m_last->duration, frame.pts - m_next_pts);
            const int64_t step = repeat->duration;
            int result = send(MediaFrame::dataFrame(m_stream_id, std::move(repeat)));
            if (result != Success) return result;
            m_next_pts += step;
        }
    }
    auto output = clone(frame);
    if (!output) return fail(AllocationFailed, "cannot clone video frame", 0, m_stream_id);
    output->duration = duration;
    m_last = clone(*output);
    if (!m_last) return fail(AllocationFailed, "cannot retain video frame", 0, m_stream_id);
    int result = send(MediaFrame::dataFrame(m_stream_id, std::move(output)));
    if (result == Success) m_next_pts = frame.pts + duration;
    return result;
}
