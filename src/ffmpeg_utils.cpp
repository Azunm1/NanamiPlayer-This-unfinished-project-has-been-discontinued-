#include "ffmpeg_utils.h"

#include <iostream>

namespace FFmpegUtils
{
void print_error(int error_code, const char *message)
{
    char error_buffer[256];
    av_strerror(error_code, error_buffer, sizeof(error_buffer));
    std::cout << message << ": " << error_buffer << std::endl;
}

double timestamp_to_seconds(int64_t timestamp, AVRational time_base)
{
    if (timestamp == AV_NOPTS_VALUE)
    {
        return -1.0;
    }

    return timestamp * av_q2d(time_base);
}
}
