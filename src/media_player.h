#pragma once

#include "media_source.h"
#include "stream_decoder.h"

#include <cstddef>
#include <string>
#include <vector>

class MediaPlayer
{
public:
    bool open(const std::string &file_path);
    void print_media_info() const;
    bool decode_all(double max_seconds = -1.0);
    bool seek_to_seconds(double seconds);
    void close();
    void set_video_frame_callback(VideoFrameCallback callback);
    void set_audio_frame_callback(AudioFrameCallback callback);

    size_t decoded_packet_count() const;
    size_t skipped_packet_count() const;
    size_t subtitle_packet_count() const;
    size_t decoded_video_frame_count() const;
    size_t decoded_audio_frame_count() const;
    int video_width() const;
    int video_height() const;

private:
    MediaSource source_;
    std::vector<StreamDecoder> decoders_;
    size_t decoded_packet_count_ = 0;
    size_t skipped_packet_count_ = 0;
    size_t subtitle_packet_count_ = 0;
    size_t decoded_video_frame_count_ = 0;
    size_t decoded_audio_frame_count_ = 0;
    int video_width_ = 0;
    int video_height_ = 0;
};
