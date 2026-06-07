#pragma once

#include <string>

class MediaPlayer;
class VideoOutput;
class AudioOutput;

class PlaybackPresenter
{
public:
    PlaybackPresenter(MediaPlayer &player, VideoOutput &video_output);

    bool run();

private:
    MediaPlayer &player_;
    VideoOutput &video_output_;
};
