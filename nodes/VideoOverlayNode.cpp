#include "VideoOverlayNode.hpp"
#include <charconv>
#include <cstring>
extern "C" {
#include <libavutil/pixdesc.h>
}

VideoOverlayNode::~VideoOverlayNode() {
    stop(); m_browser.stop(); sws_freeContext(m_video_scaler); sws_freeContext(m_overlay_scaler);
}
int VideoOverlayNode::configure(std::unordered_map<std::string, std::string> config) {
    std::string url, executable = OVERLAY_ELECTRON_PATH, script = OVERLAY_SCRIPT_PATH, mode = "cpu";
    int width = 1920, height = 1080, fps = 60;
    for (const auto& [key, value] : config) {
        if (key == "url") url = value;
        else if (key == "renderer_executable") executable = value;
        else if (key == "renderer_script") script = value;
        else if (key == "render_mode") mode = value;
        else if (key == "width" || key == "height" || key == "fps") {
            int number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number < 1 ||
                number > (key == "fps" ? 120 : 4096))
                return fail(InvalidConfiguration, "invalid overlay dimensions or fps");
            (key == "width" ? width : key == "height" ? height : fps) = number;
        } else return fail(InvalidConfiguration, "unknown overlay option");
    }
    if ((!url.starts_with("http://") && !url.starts_with("https://")) || executable.empty() || script.empty() ||
        (mode != "cpu" && mode != "gpu"))
        return fail(InvalidConfiguration, "overlay requires http(s) url, renderer paths and cpu/gpu mode");
    m_browser.stop();
    sws_freeContext(m_video_scaler); m_video_scaler = nullptr;
    sws_freeContext(m_overlay_scaler); m_overlay_scaler = nullptr;
    m_parameters.reset(); m_source_overlay.reset(); m_scaled_overlay = {};
    m_width = width; m_height = height; m_fps = fps;
    m_first_pts = m_last_pts = AV_NOPTS_VALUE; m_frames = m_late = 0;
    m_previous.reset(); m_output_time_base = {1, fps};
    resetStream();
    if (!m_browser.start(executable, script, url, width, height, fps, mode))
        return fail(MediaError, "cannot initialize browser overlay; check renderer/server diagnostics");
    return Success;
}
int VideoOverlayNode::open(const MediaStream& stream) {
    const auto& input = *stream.codec;
    if (!m_browser.latest()) return fail(NotConfigured, "browser overlay not configured");
    if (input.width <= 0 || input.height <= 0 || !sws_isSupportedInput(static_cast<AVPixelFormat>(input.format)))
        return fail(InvalidConfigFrame, "unsupported overlay input format");
    auto raw = avcodec_parameters_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate overlay parameters");
    m_parameters = {raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); }};
    if (avcodec_parameters_copy(raw, &input) < 0) return fail(MediaError, "cannot copy overlay parameters");
    raw->format = AV_PIX_FMT_BGRA; raw->color_space = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    m_video_scaler = sws_getContext(input.width, input.height, static_cast<AVPixelFormat>(input.format),
        input.width, input.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_video_scaler) return fail(MediaError, "cannot create overlay video converter");
    const int colorspace = input.color_space == AVCOL_SPC_BT709 ? SWS_CS_ITU709 :
        input.color_space == AVCOL_SPC_BT2020_NCL || input.color_space == AVCOL_SPC_BT2020_CL ? SWS_CS_BT2020 : SWS_CS_DEFAULT;
    const auto matrix = sws_getCoefficients(colorspace);
    const auto descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input.format));
    const bool rgb = descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB);
    if (sws_setColorspaceDetails(m_video_scaler, matrix, rgb || input.color_range == AVCOL_RANGE_JPEG,
        matrix, 1, 0, 1 << 16, 1 << 16) < 0) return fail(MediaError, "cannot set overlay color conversion");
    if (input.width != m_width || input.height != m_height) {
        m_overlay_scaler = sws_getContext(m_width, m_height, AV_PIX_FMT_BGRA, input.width, input.height,
            AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_overlay_scaler) return fail(MediaError, "cannot create overlay canvas scaler");
        m_scaled_overlay.width = input.width; m_scaled_overlay.height = input.height;
        m_scaled_overlay.pixels.resize(static_cast<std::size_t>(input.width) * input.height * 4);
    }
    return send(MediaFrame::configFrame(m_stream_id, {m_parameters, m_output_time_base, {m_fps, 1}}));
}
int VideoOverlayNode::transform(const AVFrame& frame) {
    const auto& input = *m_input_config->codec;
    if (frame.width != input.width || frame.height != input.height || frame.format != input.format ||
        frame.pts == AV_NOPTS_VALUE || (m_last_pts != AV_NOPTS_VALUE && frame.pts < m_last_pts))
        return fail(InvalidDataFrame, "overlay needs matching video format and monotonic PTS");
    if (m_browser.failed()) return fail(MediaError, "browser renderer disconnected");
    if (!m_previous) m_next_pts = av_rescale_q(frame.pts, m_input_config->time_base, m_output_time_base);
    while (m_previous && av_compare_ts(m_next_pts, m_output_time_base, frame.pts, m_input_config->time_base) < 0) {
        const int result = emit(m_next_pts++);
        if (result != Success) return result;
    }
    m_last_pts = frame.pts;
    return convert(frame);
}
int VideoOverlayNode::emit(int64_t pts) {
    if (m_browser.failed()) return fail(MediaError, "browser renderer disconnected");
    const auto now = std::chrono::steady_clock::now();
    if (m_first_pts == AV_NOPTS_VALUE) { m_first_pts = pts; m_started = m_last_log = now; }
    const auto elapsed = av_rescale_q(pts - m_first_pts, m_output_time_base, AVRational{1, 1000000});
    const auto deadline = m_started + std::chrono::microseconds(elapsed);
    // A running page uses wall time. Pace media by PTS so animations have the same clock.
    while (!m_stop_requested && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::min(std::chrono::microseconds(2000),
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now())));
    if (m_stop_requested) return Finished;
    if (std::chrono::steady_clock::now() - deadline > std::chrono::milliseconds(1000 / m_fps)) ++m_late;
    auto raw = av_frame_clone(m_previous.get());
    if (!raw) return fail(AllocationFailed, "cannot allocate overlaid frame");
    AVFramePtr owned(raw, [](AVFrame* p) { av_frame_free(&p); });
    if (av_frame_make_writable(raw) < 0) return fail(MediaError, "cannot copy overlay frame data");
    raw->pts = pts; raw->duration = 1; raw->time_base = m_output_time_base;
    const auto overlay = m_browser.latest();
    if (!overlay) return fail(MediaError, "missing overlay bitmap");
    if (m_overlay_scaler) {
        if (overlay != m_source_overlay) {
            const uint8_t* source[] = {overlay->pixels.data()}; const int source_stride[] = {m_width * 4};
            uint8_t* target[] = {m_scaled_overlay.pixels.data()}; const int target_stride[] = {raw->width * 4};
            if (sws_scale(m_overlay_scaler, source, source_stride, 0, m_height, target, target_stride) != raw->height)
                return fail(MediaError, "incomplete overlay scaling");
            m_scaled_overlay.findSpans(); m_source_overlay = overlay;
        }
        blendOverlay(raw->data[0], raw->linesize[0], m_scaled_overlay);
    } else blendOverlay(raw->data[0], raw->linesize[0], *overlay);
    ++m_frames;
    const auto completed = std::chrono::steady_clock::now();
    if (completed - m_last_log >= std::chrono::seconds(5)) {
        std::fprintf(stderr, "[overlay] video_fps=%.1f late_frames=%zu pts_lag_ms=%.1f\n",
            m_frames / std::chrono::duration<double>(completed - m_last_log).count(), m_late,
            std::chrono::duration<double, std::milli>(completed - deadline).count());
        m_frames = m_late = 0; m_last_log = completed;
    }
    return send(MediaFrame::dataFrame(m_stream_id, std::move(owned)));
}
int VideoOverlayNode::convert(const AVFrame& frame) {
    auto raw = av_frame_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate overlay source frame");
    AVFramePtr owned(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->width = frame.width; raw->height = frame.height; raw->format = AV_PIX_FMT_BGRA;
    if (av_frame_get_buffer(raw, 32) < 0 || av_frame_copy_props(raw, &frame) < 0)
        return fail(MediaError, "cannot allocate overlay source data");
    raw->colorspace = AVCOL_SPC_RGB; raw->color_range = AVCOL_RANGE_JPEG;
    raw->time_base = m_input_config->time_base;
    if (sws_scale(m_video_scaler, frame.data, frame.linesize, 0, frame.height, raw->data, raw->linesize) != frame.height)
        return fail(MediaError, "incomplete overlay video conversion");
    m_previous = std::move(owned);
    return Success;
}
int VideoOverlayNode::flush() {
    if (m_previous) {
        const auto duration = m_previous->duration > 0 ? m_previous->duration :
            m_input_config->frame_rate.num > 0 ? av_rescale_q(1, av_inv_q(m_input_config->frame_rate), m_input_config->time_base) :
            av_rescale_q(1, m_output_time_base, m_input_config->time_base);
        const auto end = m_last_pts + std::max<int64_t>(1, duration);
        while (av_compare_ts(m_next_pts, m_output_time_base, end, m_input_config->time_base) < 0) {
            const int result = emit(m_next_pts++);
            if (result != Success) return result;
        }
    }
    m_browser.stop(); return Success;
}
