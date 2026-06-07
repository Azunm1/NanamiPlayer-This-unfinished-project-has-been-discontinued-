#include "stream_decoder.h"

#include "ffmpeg_utils.h"

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <iostream>
#include <utility>

namespace
{
constexpr bool kVerboseDecodeLogging = false;
constexpr AVPixelFormat kVideoOutputFormat = AV_PIX_FMT_YUV420P16LE;

int64_t FrameTimestamp(const AVFrame *frame)
{
    if (!frame)
    {
        return AV_NOPTS_VALUE;
    }

    return frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
}
}

bool StreamDecoder::open(AVFormatContext *format_context, int stream_index)
{
    close();

    if (!format_context || stream_index < 0 || stream_index >= static_cast<int>(format_context->nb_streams))
    {
        return false;
    }

    AVStream *stream = format_context->streams[stream_index];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder)
    {
        std::cout << "Could not find decoder, stream_index: " << stream_index << std::endl;
        return false;
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(decoder);
    if (!codec_context)
    {
        std::cout << "Could not allocate AVCodecContext." << std::endl;
        return false;
    }

    int result = avcodec_parameters_to_context(codec_context, stream->codecpar);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not copy codec parameters");
        avcodec_free_context(&codec_context);
        return false;
    }

    result = avcodec_open2(codec_context, decoder, nullptr);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not open decoder");
        avcodec_free_context(&codec_context);
        return false;
    }

    AVFrame *new_frame = av_frame_alloc();
    if (!new_frame)
    {
        std::cout << "Could not allocate AVFrame." << std::endl;
        avcodec_free_context(&codec_context);
        return false;
    }

    stream_index_ = stream_index;
    stream_ = stream;
    codec_context_ = codec_context;
    media_type_ = stream->codecpar->codec_type;
    frame_ = new_frame;
    return true;
}

void StreamDecoder::close()
{
    av_frame_free(&frame_);
    avcodec_free_context(&codec_context_);

    if (sws_context_)
    {
        sws_freeContext(sws_context_);
        sws_context_ = nullptr;
    }

    reset_audio_converter();

    stream_index_ = -1;
    stream_ = nullptr;
    media_type_ = AVMEDIA_TYPE_UNKNOWN;

    video_buffer_.clear();
    video_output_format_ = AV_PIX_FMT_YUV420P;
    video_width_ = 0;
    video_height_ = 0;
    video_line_size_ = 0;
    video_line_size_u_ = 0;
    video_line_size_v_ = 0;
    video_plane_offset_u_ = 0;
    video_plane_offset_v_ = 0;
    video_frame_count_ = 0;
    video_frame_callback_ = nullptr;

    audio_buffer_.clear();
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_sample_format_ = AV_SAMPLE_FMT_NONE;
    audio_frame_count_ = 0;
}

bool StreamDecoder::is_open() const
{
    return codec_context_ != nullptr;
}

bool StreamDecoder::supports_frame_decoding() const
{
    return media_type_ == AVMEDIA_TYPE_VIDEO || media_type_ == AVMEDIA_TYPE_AUDIO;
}

int StreamDecoder::stream_index() const
{
    return stream_index_;
}

AVMediaType StreamDecoder::media_type() const
{
    return codec_context_ ? codec_context_->codec_type : media_type_;
}

AVStream *StreamDecoder::stream() const
{
    return stream_;
}

const std::vector<uint8_t> &StreamDecoder::video_buffer() const
{
    return video_buffer_;
}

VideoFrameView StreamDecoder::video_frame_view() const
{
    VideoFrameView view;
    if (video_buffer_.empty())
    {
        return view;
    }

    view.y_data = video_buffer_.data();
    view.u_data = video_buffer_.data() + video_plane_offset_u_;
    view.v_data = video_buffer_.data() + video_plane_offset_v_;
    view.pts = FrameTimestamp(frame_);
    view.pts_seconds = stream_ ? FFmpegUtils::timestamp_to_seconds(view.pts, stream_->time_base) : -1.0;

    view.width = video_width_;
    view.height = video_height_;

    view.y_line_size = video_line_size_;
    view.u_line_size = video_line_size_u_;
    view.v_line_size = video_line_size_v_;
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(video_output_format_);
    view.component_bits = descriptor ? descriptor->comp[0].depth : 8;
    view.plane_sample_bytes = view.component_bits > 8 ? 2 : 1;

    if (stream_)
    {
        AVRational sample_aspect_ratio = stream_->sample_aspect_ratio.num > 0 && stream_->sample_aspect_ratio.den > 0
                                              ? stream_->sample_aspect_ratio
                                              : AVRational{1, 1};
        view.display_aspect_ratio = video_height_ > 0
                                        ? static_cast<double>(video_width_) * sample_aspect_ratio.num / sample_aspect_ratio.den / video_height_
                                        : 0.0;
    }

    view.color_range = frame_ ? frame_->color_range : AVCOL_RANGE_UNSPECIFIED;
    if (view.color_range == AVCOL_RANGE_UNSPECIFIED && codec_context_)
    {
        view.color_range = codec_context_->color_range;
    }
    if (view.color_range == AVCOL_RANGE_UNSPECIFIED)
    {
        view.color_range = AVCOL_RANGE_MPEG;
    }

    view.color_space = frame_ ? frame_->colorspace : AVCOL_SPC_UNSPECIFIED;
    if (view.color_space == AVCOL_SPC_UNSPECIFIED && codec_context_)
    {
        view.color_space = codec_context_->colorspace;
    }
    if (view.color_space == AVCOL_SPC_UNSPECIFIED)
    {
        view.color_space = video_width_ >= 1280 || video_height_ >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    }

    view.color_primaries = frame_ ? frame_->color_primaries : AVCOL_PRI_UNSPECIFIED;
    if (view.color_primaries == AVCOL_PRI_UNSPECIFIED && codec_context_)
    {
        view.color_primaries = codec_context_->color_primaries;
    }

    return view;
}

int StreamDecoder::video_width() const
{
    return video_width_;
}

int StreamDecoder::video_height() const
{
    return video_height_;
}

int StreamDecoder::video_line_size() const
{
    return video_line_size_;
}

int StreamDecoder::video_line_size_y() const
{
    return video_line_size_;
}

int StreamDecoder::video_line_size_u() const
{
    return video_line_size_u_;
}

int StreamDecoder::video_line_size_v() const
{
    return video_line_size_v_;
}

size_t StreamDecoder::video_plane_offset_u() const
{
    return video_plane_offset_u_;
}

size_t StreamDecoder::video_plane_offset_v() const
{
    return video_plane_offset_v_;
}

size_t StreamDecoder::video_frame_count() const
{
    return video_frame_count_;
}

const std::vector<uint8_t> &StreamDecoder::audio_buffer() const
{
    return audio_buffer_;
}

int StreamDecoder::audio_sample_rate() const
{
    return audio_sample_rate_;
}

int StreamDecoder::audio_channels() const
{
    return audio_channels_;
}

AVSampleFormat StreamDecoder::audio_sample_format() const
{
    return audio_sample_format_;
}

size_t StreamDecoder::audio_frame_count() const
{
    return audio_frame_count_;
}

bool StreamDecoder::send_packet(const AVPacket *packet)
{
    if (!codec_context_)
    {
        return false;
    }

    if (!supports_frame_decoding())
    {
        return true;
    }

    int result = avcodec_send_packet(codec_context_, packet);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not send packet to decoder");
        return false;
    }

    return true;
}

bool StreamDecoder::receive_frames()
{
    if (!codec_context_ || !frame_)
    {
        return false;
    }

    if (!supports_frame_decoding())
    {
        return true;
    }

    int result = 0;
    while (result >= 0)
    {
        result = avcodec_receive_frame(codec_context_, frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        {
            return true;
        }

        if (result < 0)
        {
            FFmpegUtils::print_error(result, "Could not receive frame from decoder");
            return false;
        }

        if (media_type_ == AVMEDIA_TYPE_VIDEO)
        {
            if (!convert_video_frame())
            {
                av_frame_unref(frame_);
                return false;
            }
            ++video_frame_count_;

            VideoFrameView view = video_frame_view();
            if (video_frame_callback_)
            {
                if (!video_frame_callback_(view))
                {
                    av_frame_unref(frame_);
                    return false;
                }
            }

            if (kVerboseDecodeLogging)
            {
                std::cout
                    << "receive video frame, stream_index: " << stream_index_
                    << ", frame_count: " << video_frame_count_
                    << ", width: " << view.width
                    << ", height: " << view.height
                    << ", y_data: " << static_cast<const void *>(view.y_data)
                    << ", u_data: " << static_cast<const void *>(view.u_data)
                    << ", v_data: " << static_cast<const void *>(view.v_data)
                    << ", y_line_size: " << view.y_line_size
                    << ", u_line_size: " << view.u_line_size
                    << ", v_line_size: " << view.v_line_size
                    << ", pts: " << view.pts
                    << ", pts_seconds: " << view.pts_seconds
                    << std::endl;
            }
        }
        else if (media_type_ == AVMEDIA_TYPE_AUDIO)
        {
            if (!convert_audio_frame())
            {
                av_frame_unref(frame_);
                return false;
            }
            ++audio_frame_count_;

            const int64_t audio_pts = FrameTimestamp(frame_);
            const double audio_pts_seconds = stream_ ? FFmpegUtils::timestamp_to_seconds(audio_pts, stream_->time_base) : -1.0;
            if (kVerboseDecodeLogging)
            {
                std::cout
                    << "receive audio frame, stream_index: " << stream_index_
                    << ", frame_count: " << audio_frame_count_
                    << ", samples: " << frame_->nb_samples
                    << ", format: " << frame_->format
                    << ", pcm_bytes: " << audio_buffer_.size()
                    << ", sample_rate: " << audio_sample_rate_
                    << ", channels: " << audio_channels_
                    << ", pts: " << audio_pts
                    << ", pts_seconds: " << audio_pts_seconds
                    << std::endl;
            }

            if (audio_frame_callback_)
            {
                if (!audio_frame_callback_(audio_buffer_, audio_sample_rate_, audio_channels_, audio_sample_format_, audio_pts_seconds))
                {
                    av_frame_unref(frame_);
                    return false;
                }
            }
        }

        av_frame_unref(frame_);
    }

    return true;
}

bool StreamDecoder::decode_subtitle_packet(const AVPacket *packet)
{
    if (!codec_context_ || media_type_ != AVMEDIA_TYPE_SUBTITLE)
    {
        return true;
    }

    AVSubtitle subtitle = {};
    int got_subtitle = 0;
    int result = avcodec_decode_subtitle2(codec_context_, &subtitle, &got_subtitle, packet);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not decode subtitle packet");
        return false;
    }

    if (got_subtitle)
    {
        print_subtitle(subtitle, packet ? packet->pts : AV_NOPTS_VALUE);
        avsubtitle_free(&subtitle);
    }

    return true;
}

void StreamDecoder::flush()
{
    if (!codec_context_)
    {
        return;
    }

    if (media_type_ == AVMEDIA_TYPE_SUBTITLE)
    {
        AVPacket flush_packet = {};
        flush_packet.data = nullptr;
        flush_packet.size = 0;
        decode_subtitle_packet(&flush_packet);
        return;
    }

    if (!supports_frame_decoding())
    {
        return;
    }

    int result = avcodec_send_packet(codec_context_, nullptr);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not flush decoder");
        return;
    }

    receive_frames();
}

void StreamDecoder::flush_buffers()
{
    if (!codec_context_)
    {
        return;
    }

    avcodec_flush_buffers(codec_context_);
    if (frame_)
    {
        av_frame_unref(frame_);
    }

    video_buffer_.clear();
    audio_buffer_.clear();
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_sample_format_ = AV_SAMPLE_FMT_NONE;
    reset_audio_converter();
}

void StreamDecoder::set_video_frame_callback(VideoFrameCallback callback)
{
    video_frame_callback_ = std::move(callback);
}

void StreamDecoder::set_audio_frame_callback(AudioFrameCallback callback)
{
    audio_frame_callback_ = std::move(callback);
}

void StreamDecoder::print_subtitle(const AVSubtitle &subtitle, int64_t packet_pts) const
{
    std::cout
        << "receive subtitle, stream_index: " << stream_index_
        << ", packet_pts: " << packet_pts
        << ", subtitle_pts: " << subtitle.pts
        << ", start_ms: " << subtitle.start_display_time
        << ", end_ms: " << subtitle.end_display_time
        << ", rects: " << subtitle.num_rects
        << std::endl;

    for (unsigned int rect_index = 0; rect_index < subtitle.num_rects; ++rect_index)
    {
        const AVSubtitleRect *rect = subtitle.rects[rect_index];
        if (!rect)
        {
            continue;
        }

        std::cout << "  subtitle rect " << rect_index << ", type: " << rect->type;
        if (rect->type == SUBTITLE_TEXT && rect->text)
        {
            std::cout << ", text: " << rect->text;
        }
        else if (rect->type == SUBTITLE_ASS && rect->ass)
        {
            std::cout << ", ass: " << rect->ass;
        }
        else if (rect->type == SUBTITLE_BITMAP)
        {
            std::cout << ", bitmap: " << rect->w << "x" << rect->h
                      << " at (" << rect->x << "," << rect->y << ")";
        }
        std::cout << std::endl;
    }
}

bool StreamDecoder::convert_video_frame()
{
    if (!frame_ || frame_->width <= 0 || frame_->height <= 0)
    {
        return false;
    }

    sws_context_ = sws_getCachedContext(
        sws_context_,
        frame_->width,
        frame_->height,
        static_cast<AVPixelFormat>(frame_->format),
        frame_->width,
        frame_->height,
        kVideoOutputFormat,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!sws_context_)
    {
        std::cout << "Could not create SwsContext." << std::endl;
        return false;
    }

    int buffer_size = av_image_get_buffer_size(kVideoOutputFormat, frame_->width, frame_->height, 1);
    if (buffer_size < 0)
    {
        FFmpegUtils::print_error(buffer_size, "Could not get YUV420P16LE buffer size");
        return false;
    }

    video_buffer_.resize(static_cast<size_t>(buffer_size));
    uint8_t *destination_data[4] = {};
    int destination_line_size[4] = {};

    int result = av_image_fill_arrays(
        destination_data,
        destination_line_size,
        video_buffer_.data(),
        kVideoOutputFormat,
        frame_->width,
        frame_->height,
        1);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not fill YUV420P image arrays");
        return false;
    }

    int scaled_height = sws_scale(
        sws_context_,
        frame_->data,
        frame_->linesize,
        0,
        frame_->height,
        destination_data,
        destination_line_size);
    if (scaled_height != frame_->height)
    {
        std::cout << "Could not convert full video frame to YUV420P16LE." << std::endl;
        return false;
    }

    video_output_format_ = kVideoOutputFormat;
    video_width_ = frame_->width;
    video_height_ = frame_->height;
    video_line_size_ = destination_line_size[0];
    video_line_size_u_ = destination_line_size[1];
    video_line_size_v_ = destination_line_size[2];
    video_plane_offset_u_ = static_cast<size_t>(destination_data[1] - destination_data[0]);
    video_plane_offset_v_ = static_cast<size_t>(destination_data[2] - destination_data[0]);
    return true;
}

bool StreamDecoder::convert_audio_frame()
{
    if (!frame_ || frame_->nb_samples <= 0)
    {
        return false;
    }

    AVChannelLayout input_layout = frame_->ch_layout;
    if (input_layout.nb_channels <= 0)
    {
        av_channel_layout_default(&input_layout, codec_context_->ch_layout.nb_channels > 0 ? codec_context_->ch_layout.nb_channels : 2);
    }

    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVSampleFormat output_format = AV_SAMPLE_FMT_S16;
    int output_sample_rate = frame_->sample_rate > 0 ? frame_->sample_rate : codec_context_->sample_rate;

    if (!swr_context_)
    {
        int result = swr_alloc_set_opts2(
            &swr_context_,
            &output_layout,
            output_format,
            output_sample_rate,
            &input_layout,
            static_cast<AVSampleFormat>(frame_->format),
            frame_->sample_rate,
            0,
            nullptr);
        if (result < 0)
        {
            FFmpegUtils::print_error(result, "Could not allocate SwrContext");
            return false;
        }

        result = swr_init(swr_context_);
        if (result < 0)
        {
            FFmpegUtils::print_error(result, "Could not initialize SwrContext");
            swr_free(&swr_context_);
            return false;
        }

        if (has_audio_input_layout_)
        {
            av_channel_layout_uninit(&audio_input_layout_);
        }
        av_channel_layout_copy(&audio_input_layout_, &input_layout);
        has_audio_input_layout_ = true;
    }

    int output_samples = av_rescale_rnd(
        swr_get_delay(swr_context_, output_sample_rate) + frame_->nb_samples,
        output_sample_rate,
        output_sample_rate,
        AV_ROUND_UP);
    if (output_samples < 0)
    {
        return false;
    }

    int bytes_per_sample = av_get_bytes_per_sample(output_format);
    int output_channels = output_layout.nb_channels;
    int buffer_size = output_samples * output_channels * bytes_per_sample;
    audio_buffer_.resize(static_cast<size_t>(buffer_size));

    uint8_t *output_data[1] = {audio_buffer_.data()};
    int converted_samples = swr_convert(
        swr_context_,
        output_data,
        output_samples,
        const_cast<const uint8_t **>(frame_->extended_data),
        frame_->nb_samples);
    if (converted_samples < 0)
    {
        FFmpegUtils::print_error(converted_samples, "Could not convert audio frame to PCM");
        return false;
    }

    int converted_bytes = converted_samples * output_channels * bytes_per_sample;
    audio_buffer_.resize(static_cast<size_t>(converted_bytes));
    audio_sample_rate_ = output_sample_rate;
    audio_channels_ = output_channels;
    audio_sample_format_ = output_format;
    return true;
}

void StreamDecoder::reset_audio_converter()
{
    if (swr_context_)
    {
        swr_free(&swr_context_);
    }

    if (has_audio_input_layout_)
    {
        av_channel_layout_uninit(&audio_input_layout_);
        has_audio_input_layout_ = false;
    }
}
