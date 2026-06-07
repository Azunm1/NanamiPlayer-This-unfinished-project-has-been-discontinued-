#include "media_player.h"
#include "playback_presenter.h"
#include "video_output.h"

#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    std::string file_path = "D:/nanami_player/assets/test.mkv";
    if (argc >= 2)
    {
        file_path = argv[1];
    }

    std::cout << "Input file: " << file_path << std::endl;

    MediaPlayer player;
    if (!player.open(file_path))
    {
        return -1;
    }

    VideoOutput video_output;
    if (!video_output.open("Nanami Player", 1280, 720))
    {
        player.close();
        return -1;
    }

    player.print_media_info();
    if (!video_output.clear_current())
    {
        std::cout << "Could not release OpenGL context before starting render thread." << std::endl;
    }

    PlaybackPresenter presenter(player, video_output);
    presenter.run();

    if (!video_output.make_current())
    {
        std::cout << "Could not reacquire OpenGL context before shutdown." << std::endl;
    }

    player.close();
    video_output.close();

    return 0;
}
