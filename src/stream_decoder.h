#pragma once

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct SwsContext;
struct SwrContext;

struct VideoFrameView
{
    const uint8_t *y_data = nullptr;
    const uint8_t *u_data = nullptr;
    const uint8_t *v_data = nullptr;

    int64_t pts = AV_NOPTS_VALUE;
    double pts_seconds = -1.0;

    int width = 0;
    int height = 0;

    int y_line_size = 0;
    int u_line_size = 0;
    int v_line_size = 0;
    int plane_sample_bytes = 1;
    int component_bits = 8;

    double display_aspect_ratio = 0.0;
    AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace color_space = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries color_primaries = AVCOL_PRI_UNSPECIFIED;
};

using VideoFrameCallback = std::function<bool(const VideoFrameView &frame)>;
using AudioFrameCallback = std::function<bool(const std::vector<uint8_t> &pcm, int sample_rate, int channels, AVSampleFormat sample_format, double pts_seconds)>;

class StreamDecoder
{
public:
    bool open(AVFormatContext *format_context, int stream_index);
    void close();

    bool is_open() const;
    bool supports_frame_decoding() const;
    int stream_index() const;
    AVMediaType media_type() const;
    AVStream *stream() const;

    const std::vector<uint8_t> &video_buffer() const;

    VideoFrameView video_frame_view() const;
    
    int video_width() const;
    int video_height() const;
    int video_line_size() const;
    int video_line_size_y() const;
    int video_line_size_u() const;
    int video_line_size_v() const;
    size_t video_plane_offset_u() const;
    size_t video_plane_offset_v() const;
    size_t video_frame_count() const;

    const std::vector<uint8_t> &audio_buffer() const;
    int audio_sample_rate() const;
    int audio_channels() const;
    AVSampleFormat audio_sample_format() const;
    size_t audio_frame_count() const;

    bool send_packet(const AVPacket *packet);
    bool receive_frames();
    bool decode_subtitle_packet(const AVPacket *packet);
    void flush_buffers();
    void flush();
    void set_video_frame_callback(VideoFrameCallback callback);
    void set_audio_frame_callback(AudioFrameCallback callback);

private:
    bool convert_video_frame();
    bool convert_audio_frame();
    void reset_audio_converter();
    void print_subtitle(const AVSubtitle &subtitle, int64_t packet_pts) const;

    int stream_index_ = -1;
    AVStream *stream_ = nullptr;
    AVCodecContext *codec_context_ = nullptr;
    AVMediaType media_type_ = AVMEDIA_TYPE_UNKNOWN;
    AVFrame *frame_ = nullptr;

    SwsContext *sws_context_ = nullptr;
    std::vector<uint8_t> video_buffer_;
    AVPixelFormat video_output_format_ = AV_PIX_FMT_YUV420P;
    int video_width_ = 0;
    int video_height_ = 0;
    int video_line_size_ = 0;
    int video_line_size_u_ = 0;
    int video_line_size_v_ = 0;
    size_t video_plane_offset_u_ = 0;
    size_t video_plane_offset_v_ = 0;
    size_t video_frame_count_ = 0;
    VideoFrameCallback video_frame_callback_;
    AudioFrameCallback audio_frame_callback_;

    SwrContext *swr_context_ = nullptr;
    std::vector<uint8_t> audio_buffer_;
    int audio_sample_rate_ = 0;
    int audio_channels_ = 0;
    AVSampleFormat audio_sample_format_ = AV_SAMPLE_FMT_NONE;
    AVChannelLayout audio_input_layout_ = {};
    bool has_audio_input_layout_ = false;
    size_t audio_frame_count_ = 0;
};
