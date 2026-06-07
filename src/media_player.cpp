#include "media_player.h"

#include "ffmpeg_utils.h"

#include <iostream>

namespace
{
constexpr bool kVerbosePacketLogging = false;
}

bool MediaPlayer::open(const std::string &file_path)
{
    close();
    decoded_packet_count_ = 0;
    skipped_packet_count_ = 0;
    subtitle_packet_count_ = 0;
    decoded_video_frame_count_ = 0;
    decoded_audio_frame_count_ = 0;
    video_width_ = 0;
    video_height_ = 0;

    if (!source_.open(file_path))
    {
        return false;
    }

    source_.dump_format();
    std::cout << "best video_stream_index: "
              << av_find_best_stream(source_.format_context(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)
              << std::endl;
    std::cout << "best audio_stream_index: "
              << av_find_best_stream(source_.format_context(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
              << std::endl;

    decoders_.clear();
    decoders_.resize(source_.stream_count());

    int decoder_count = 0;
    for (unsigned int stream_index = 0; stream_index < source_.stream_count(); ++stream_index)
    {
        AVStream *stream = source_.stream(stream_index);
        if (!stream)
        {
            continue;
        }

        AVMediaType media_type = stream->codecpar->codec_type;
        if (media_type == AVMEDIA_TYPE_VIDEO && (video_width_ <= 0 || video_height_ <= 0))
        {
            video_width_ = stream->codecpar->width;
            video_height_ = stream->codecpar->height;
        }
        if (!decoders_[stream_index].open(source_.format_context(), static_cast<int>(stream_index)))
        {
            std::cout << "Skip stream_index: " << stream_index << std::endl;
            continue;
        }

        ++decoder_count;
        std::cout
            << "open decoder, stream_index: " << stream_index
            << ", media_type: " << av_get_media_type_string(media_type)
            << std::endl;

        if (!decoders_[stream_index].supports_frame_decoding())
        {
            std::cout
                << "stream_index: " << stream_index
                << " opened, but frame decoding is skipped for this media type now."
                << std::endl;
        }
    }

    if (decoder_count == 0)
    {
        std::cout << "Could not open any stream decoder." << std::endl;
        close();
        return false;
    }

    return true;
}

void MediaPlayer::print_media_info() const
{
    std::cout << "stream count: " << source_.stream_count() << std::endl;
}

bool MediaPlayer::decode_all(double max_seconds)
{
    if (!source_.format_context())
    {
        return false;
    }

    decoded_packet_count_ = 0;
    skipped_packet_count_ = 0;
    subtitle_packet_count_ = 0;
    decoded_video_frame_count_ = 0;
    decoded_audio_frame_count_ = 0;

    AVPacket *packet = av_packet_alloc();
    if (!packet)
    {
        std::cout << "Could not allocate AVPacket." << std::endl;
        return false;
    }

    std::cout << "\nStart reading packets..." << std::endl;

    int result = 0;
    while ((result = av_read_frame(source_.format_context(), packet)) >= 0)
    {
        int stream_index = packet->stream_index;
        if (stream_index < 0 || stream_index >= static_cast<int>(decoders_.size()) || !decoders_[stream_index].is_open())
        {
            ++skipped_packet_count_;
            av_packet_unref(packet);
            continue;
        }

        StreamDecoder &decoder = decoders_[stream_index];

        if (!decoder.supports_frame_decoding())
        {
            if (decoder.media_type() == AVMEDIA_TYPE_SUBTITLE)
            {
                std::cout
                    << "send subtitle packet, stream_index: " << packet->stream_index
                    << ", size: " << packet->size
                    << ", pts: " << packet->pts
                    << ", dts: " << packet->dts
                    << std::endl;

                if (!decoder.decode_subtitle_packet(packet))
                {
                    av_packet_unref(packet);
                    break;
                }

                ++decoded_packet_count_;
                ++subtitle_packet_count_;
                av_packet_unref(packet);
                continue;
            }

            ++skipped_packet_count_;
            av_packet_unref(packet);
            continue;
        }

        AVStream *stream = decoder.stream();
        double pts_seconds = -1.0;
        double dts_seconds = -1.0;
        if (stream)
        {
            pts_seconds = FFmpegUtils::timestamp_to_seconds(packet->pts, stream->time_base);
            dts_seconds = FFmpegUtils::timestamp_to_seconds(packet->dts, stream->time_base);
        }

        if (max_seconds > 0.0 && pts_seconds >= max_seconds)
        {
            std::cout << "Reached decode time limit: " << max_seconds << " seconds." << std::endl;
            av_packet_unref(packet);
            break;
        }

        if (kVerbosePacketLogging)
        {
            std::cout
                << "send packet, stream_index: " << packet->stream_index
                << ", media_type: " << av_get_media_type_string(decoder.media_type())
                << ", size: " << packet->size
                << ", pts: " << packet->pts
                << ", pts_seconds: " << pts_seconds
                << ", dts: " << packet->dts
                << ", dts_seconds: " << dts_seconds
                << ", flags: " << packet->flags
                << std::endl;
        }

        if (!decoder.send_packet(packet))
        {
            av_packet_unref(packet);
            break;
        }
        ++decoded_packet_count_;

        av_packet_unref(packet);

        if (!decoder.receive_frames())
        {
            break;
        }
    }

    for (StreamDecoder &decoder : decoders_)
    {
        if (decoder.is_open() && decoder.supports_frame_decoding())
        {
            decoder.flush();
            decoded_video_frame_count_ += decoder.video_frame_count();
            decoded_audio_frame_count_ += decoder.audio_frame_count();
        }
    }

    std::cout << "Finished reading packets." << std::endl;
    std::cout
        << "Decode summary:"
        << " packets_sent=" << decoded_packet_count_
        << ", packets_skipped=" << skipped_packet_count_
        << ", subtitle_packets=" << subtitle_packet_count_
        << ", video_frames=" << decoded_video_frame_count_
        << ", audio_frames=" << decoded_audio_frame_count_
        << std::endl;

    av_packet_free(&packet);
    return true;
}

bool MediaPlayer::seek_to_seconds(double seconds)
{
    AVFormatContext *format_context = source_.format_context();
    if (!format_context)
    {
        return false;
    }

    int stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int64_t timestamp = static_cast<int64_t>(seconds * AV_TIME_BASE);
    if (stream_index >= 0 && stream_index < static_cast<int>(format_context->nb_streams))
    {
        AVStream *stream = format_context->streams[stream_index];
        timestamp = av_rescale_q(timestamp, AV_TIME_BASE_Q, stream->time_base);
    }
    else
    {
        stream_index = -1;
    }

    int result = av_seek_frame(format_context, stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Could not seek media source");
        return false;
    }

    avformat_flush(format_context);
    for (StreamDecoder &decoder : decoders_)
    {
        if (decoder.is_open())
        {
            decoder.flush_buffers();
        }
    }

    return true;
}

void MediaPlayer::close()
{
    for (StreamDecoder &decoder : decoders_)
    {
        decoder.close();
    }

    decoders_.clear();
    source_.close();
}

void MediaPlayer::set_video_frame_callback(VideoFrameCallback callback)
{
    for (StreamDecoder &decoder : decoders_)
    {
        decoder.set_video_frame_callback(callback);
    }
}

void MediaPlayer::set_audio_frame_callback(AudioFrameCallback callback)
{
    for (StreamDecoder &decoder : decoders_)
    {
        decoder.set_audio_frame_callback(callback);
    }
}

size_t MediaPlayer::decoded_packet_count() const
{
    return decoded_packet_count_;
}

size_t MediaPlayer::skipped_packet_count() const
{
    return skipped_packet_count_;
}

size_t MediaPlayer::subtitle_packet_count() const
{
    return subtitle_packet_count_;
}

size_t MediaPlayer::decoded_video_frame_count() const
{
    return decoded_video_frame_count_;
}

size_t MediaPlayer::decoded_audio_frame_count() const
{
    return decoded_audio_frame_count_;
}

int MediaPlayer::video_width() const
{
    return video_width_;
}

int MediaPlayer::video_height() const
{
    return video_height_;
}
