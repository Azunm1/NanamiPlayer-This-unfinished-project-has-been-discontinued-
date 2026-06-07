#include "media_source.h"

#include "ffmpeg_utils.h"

#include <iostream>

bool MediaSource::open(const std::string &file_path)
{
    close();

    file_path_ = file_path;

    int result = avformat_open_input(&format_context_, file_path_.c_str(), nullptr, nullptr);
    if (result < 0)
    {
        FFmpegUtils::print_error(result, "Error opening file");
        return false;
    }

    result = avformat_find_stream_info(format_context_, nullptr);
    if (result < 0)
    {
        std::cout << "Could not find stream information." << std::endl;
        close();
        return false;
    }

    std::cout << "File opened successfully: " << file_path_ << std::endl;
    return true;
}

void MediaSource::close()
{
    if (format_context_)
    {
        avformat_close_input(&format_context_);
    }
}

void MediaSource::dump_format() const
{
    if (format_context_)
    {
        av_dump_format(format_context_, 0, file_path_.c_str(), 0);
    }
}

AVFormatContext *MediaSource::format_context() const
{
    return format_context_;
}

unsigned int MediaSource::stream_count() const
{
    return format_context_ ? format_context_->nb_streams : 0;
}

AVStream *MediaSource::stream(unsigned int index) const
{
    if (!format_context_ || index >= format_context_->nb_streams)
    {
        return nullptr;
    }

    return format_context_->streams[index];
}
