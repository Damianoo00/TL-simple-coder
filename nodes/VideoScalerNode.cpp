#include "VideoScalerNode.hpp"

namespace {
int coefficients(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default: return SWS_CS_DEFAULT;
    }
}
bool rgb(AVPixelFormat format) {
    const auto descriptor = av_pix_fmt_desc_get(format);
    return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB);
}
}
VideoScalerNode::~VideoScalerNode() { stop(); sws_freeContext(m_scaler); }

int VideoScalerNode::configure(std::unordered_map<std::string, std::string> config) {
    int width = 0, height = 0, algorithm = SWS_BILINEAR;
    auto format = AV_PIX_FMT_YUV420P;
    for (const auto& [key, value] : config) {
        if (key == "width" || key == "height") {
            int number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number <= 0)
                return fail(InvalidConfiguration, "width and height must be positive integers");
            (key == "width" ? width : height) = number;
        } else if (key == "pixel_format") {
            format = av_get_pix_fmt(value.c_str());
            if (format == AV_PIX_FMT_NONE || !sws_isSupportedOutput(format))
                return fail(InvalidConfiguration, "unsupported output pixel_format");
        } else if (key == "algorithm") {
            if (value == "bilinear") algorithm = SWS_BILINEAR;
            else if (value == "bicubic") algorithm = SWS_BICUBIC;
            else if (value == "nearest") algorithm = SWS_POINT;
            else if (value == "lanczos") algorithm = SWS_LANCZOS;
            else return fail(InvalidConfiguration, "algorithm must be bilinear, bicubic, nearest or lanczos");
        } else return fail(InvalidConfiguration, "unknown scaler option");
    }
    sws_freeContext(m_scaler);
    m_scaler = nullptr;
    m_output_parameters.reset();
    m_width = width; m_height = height; m_format = format; m_algorithm = algorithm;
    resetStream();
    return Success;
}

int VideoScalerNode::open(const MediaStream& stream) {
    const auto& input = *stream.codec;
    const auto format = static_cast<AVPixelFormat>(input.format);
    if (input.width <= 0 || input.height <= 0 || !sws_isSupportedInput(format))
        return fail(InvalidConfigFrame, "unsupported source dimensions or pixel format", 0, m_stream_id);
    auto raw = avcodec_parameters_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate scaled parameters", 0, m_stream_id);
    m_output_parameters = {raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); }};
    if (logMediaResult(avcodec_parameters_copy(raw, &input), nodeType()) < 0)
        return fail(MediaError, "cannot copy scaled parameters", 0, m_stream_id);
    raw->width = m_width ? m_width : input.width;
    raw->height = m_height ? m_height : input.height;
    raw->format = m_format;
    if (input.sample_aspect_ratio.num > 0 && input.sample_aspect_ratio.den > 0)
        raw->sample_aspect_ratio = av_mul_q(input.sample_aspect_ratio,
            av_div_q(AVRational{input.width, input.height}, AVRational{raw->width, raw->height}));
    if (rgb(m_format)) {
        raw->color_space = AVCOL_SPC_RGB;
        raw->color_range = AVCOL_RANGE_JPEG;
    } else if (rgb(format)) {
        raw->color_space = AVCOL_SPC_SMPTE170M;
        raw->color_range = AVCOL_RANGE_MPEG;
    }
    m_scaler = sws_getContext(input.width, input.height, format, raw->width, raw->height, m_format,
                              m_algorithm, nullptr, nullptr, nullptr);
    if (!m_scaler) return fail(MediaError, "cannot create scaler context", 0, m_stream_id);
    const auto matrix = sws_getCoefficients(coefficients(rgb(format) ? raw->color_space : input.color_space));
    if (logMediaResult(sws_setColorspaceDetails(m_scaler, matrix, rgb(format) || input.color_range == AVCOL_RANGE_JPEG,
            matrix, rgb(m_format) || raw->color_range == AVCOL_RANGE_JPEG, 0, 1 << 16, 1 << 16), nodeType()) < 0)
        return fail(MediaError, "cannot configure scaler color range", 0, m_stream_id);
    const int sent = send(MediaFrame::configFrame(m_stream_id, {m_output_parameters, stream.time_base, stream.frame_rate}));
    if (sent == Success)
        std::fprintf(stderr, "[info] VideoScalerNode: scaler opened stream_id=%d input=%dx%d/%s output=%dx%d/%s\n",
                     m_stream_id, input.width, input.height, av_get_pix_fmt_name(format), raw->width, raw->height, av_get_pix_fmt_name(m_format));
    return sent;
}

int VideoScalerNode::transform(const AVFrame& frame) {
    const auto& input = *m_input_config->codec;
    if (frame.width != input.width || frame.height != input.height || frame.format != input.format)
        return fail(InvalidDataFrame, "video format differs from CONFIGFRAME", 0, m_stream_id);
    auto raw = av_frame_alloc();
    if (!raw) return fail(AllocationFailed, "cannot allocate scaled frame", 0, m_stream_id);
    AVFramePtr output(raw, [](AVFrame* p) { av_frame_free(&p); });
    raw->width = m_output_parameters->width;
    raw->height = m_output_parameters->height;
    raw->format = m_output_parameters->format;
    if (logMediaResult(av_frame_get_buffer(raw, 32), nodeType()) < 0 ||
        logMediaResult(av_frame_copy_props(raw, &frame), nodeType()) < 0)
        return fail(MediaError, "cannot allocate scaled frame data or copy properties", 0, m_stream_id);
    raw->sample_aspect_ratio = m_output_parameters->sample_aspect_ratio;
    raw->colorspace = m_output_parameters->color_space;
    raw->color_range = m_output_parameters->color_range;
    raw->time_base = m_input_config->time_base;
    const int rows = sws_scale(m_scaler, frame.data, frame.linesize, 0, frame.height, raw->data, raw->linesize);
    if (rows != raw->height) {
        if (rows < 0) logMediaResult(rows, nodeType());
        return fail(MediaError, "scaler did not produce a complete image", 0, m_stream_id);
    }
    return send(MediaFrame::dataFrame(m_stream_id, std::move(output)));
}
