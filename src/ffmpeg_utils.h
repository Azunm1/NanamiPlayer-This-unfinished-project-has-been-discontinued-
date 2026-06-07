#pragma once

extern "C"
{
#include <libavutil/avutil.h>
}

namespace FFmpegUtils
{
void print_error(int error_code, const char *message);
double timestamp_to_seconds(int64_t timestamp, AVRational time_base);
}
