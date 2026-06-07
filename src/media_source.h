#pragma once

extern "C"
{
#include <libavformat/avformat.h>
}

#include <string>

class MediaSource
{
public:
    bool open(const std::string &file_path);
    void close();
    void dump_format() const;

    AVFormatContext *format_context() const;
    unsigned int stream_count() const;
    AVStream *stream(unsigned int index) const;

private:
    std::string file_path_;
    AVFormatContext *format_context_ = nullptr;
};
