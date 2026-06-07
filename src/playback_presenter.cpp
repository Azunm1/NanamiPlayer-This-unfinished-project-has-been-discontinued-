#include "playback_presenter.h"

#include "media_player.h"
#include "audio_output.h"
#include "video_output.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using SteadyClock = std::chrono::steady_clock;

constexpr auto kFrameWaitInterval = std::chrono::milliseconds(5);
constexpr auto kLateFrameDropThreshold = std::chrono::milliseconds(100);
constexpr auto kAudioClockStallFallback = std::chrono::milliseconds(250);
constexpr auto kSeekDebounceInterval = std::chrono::milliseconds(180);
constexpr size_t kVideoQueueCapacity = 8;
constexpr double kSeekStepSeconds = 5.0;
constexpr double kSeekPrerollToleranceSeconds = 0.002;

struct VideoPlaybackClock
{
    bool started = false;
    bool paused = false;
    double first_pts_seconds = 0.0;
    SteadyClock::time_point start_time;
    SteadyClock::time_point pause_time;
    std::chrono::steady_clock::duration paused_duration = std::chrono::steady_clock::duration::zero();

    void start(double pts_seconds, SteadyClock::time_point now)
    {
        started = true;
        paused = false;
        first_pts_seconds = pts_seconds;
        start_time = now;
        paused_duration = std::chrono::steady_clock::duration::zero();
    }

    void pause(SteadyClock::time_point now)
    {
        if (!started || paused)
        {
            return;
        }

        paused = true;
        pause_time = now;
    }

    void resume(SteadyClock::time_point now)
    {
        if (!started || !paused)
        {
            return;
        }

        paused_duration += now - pause_time;
        paused = false;
    }

    double elapsed_seconds(SteadyClock::time_point now) const
    {
        const SteadyClock::time_point effective_now = paused ? pause_time : now;
        return std::chrono::duration<double>(effective_now - start_time - paused_duration).count();
    }
};

struct OwnedVideoFrame
{
    std::vector<uint8_t> storage;
    VideoFrameView view;
};

struct PlaybackState
{
    std::atomic<bool> quit_requested = false;
    std::atomic<bool> render_finished = false;
    std::atomic<bool> decode_finished = false;
    std::mutex pause_mutex;
    std::condition_variable pause_changed;
    bool paused = false;
    std::mutex queue_mutex;
    std::condition_variable queue_not_empty;
    std::condition_variable queue_not_full;
    std::deque<OwnedVideoFrame> video_queue;
    bool queue_closed = false;
};

struct PlaybackSession
{
    PlaybackState state;
    std::thread render_thread;
    std::thread decode_thread;
    std::atomic<bool> audio_released = false;
    std::mutex position_mutex;
    bool has_latest_position = false;
    double latest_video_position_seconds = 0.0;
    std::mutex sync_start_mutex;
    bool has_sync_start = false;
    double sync_start_seconds = 0.0;
};

OwnedVideoFrame CopyVideoFrame(const VideoFrameView &frame)
{
    OwnedVideoFrame owned_frame;
    if (!frame.y_data || !frame.u_data || !frame.v_data ||
        frame.width <= 0 || frame.height <= 0 ||
        frame.y_line_size <= 0 || frame.u_line_size <= 0 || frame.v_line_size <= 0)
    {
        return owned_frame;
    }

    const int chroma_height = (frame.height + 1) / 2;
    const size_t y_bytes = static_cast<size_t>(frame.y_line_size) * static_cast<size_t>(frame.height);
    const size_t u_bytes = static_cast<size_t>(frame.u_line_size) * static_cast<size_t>(chroma_height);
    const size_t v_bytes = static_cast<size_t>(frame.v_line_size) * static_cast<size_t>(chroma_height);

    owned_frame.storage.resize(y_bytes + u_bytes + v_bytes);
    uint8_t *dst = owned_frame.storage.data();
    std::memcpy(dst, frame.y_data, y_bytes);
    std::memcpy(dst + y_bytes, frame.u_data, u_bytes);
    std::memcpy(dst + y_bytes + u_bytes, frame.v_data, v_bytes);

    owned_frame.view = frame;
    owned_frame.view.y_data = owned_frame.storage.data();
    owned_frame.view.u_data = owned_frame.storage.data() + y_bytes;
    owned_frame.view.v_data = owned_frame.storage.data() + y_bytes + u_bytes;
    return owned_frame;
}

bool PushVideoFrame(PlaybackState &playback_state, const VideoFrameView &frame)
{
    {
        std::unique_lock<std::mutex> pause_lock(playback_state.pause_mutex);
        playback_state.pause_changed.wait(pause_lock, [&playback_state]()
        {
            return playback_state.quit_requested.load() || !playback_state.paused;
        });

        if (playback_state.quit_requested.load())
        {
            return false;
        }
    }

    OwnedVideoFrame owned_frame = CopyVideoFrame(frame);
    if (owned_frame.storage.empty())
    {
        std::cout << "Could not copy video frame for render queue." << std::endl;
        return false;
    }

    std::unique_lock<std::mutex> lock(playback_state.queue_mutex);
    playback_state.queue_not_full.wait(lock, [&playback_state]()
    {
        return playback_state.queue_closed || playback_state.video_queue.size() < kVideoQueueCapacity;
    });

    if (playback_state.queue_closed)
    {
        return false;
    }

    playback_state.video_queue.push_back(std::move(owned_frame));
    lock.unlock();
    playback_state.queue_not_empty.notify_one();
    return true;
}

bool PopVideoFrame(PlaybackState &playback_state, OwnedVideoFrame &frame)
{
    std::unique_lock<std::mutex> lock(playback_state.queue_mutex);
    playback_state.queue_not_empty.wait(lock, [&playback_state]()
    {
        return playback_state.queue_closed || !playback_state.video_queue.empty();
    });

    if (playback_state.video_queue.empty())
    {
        return false;
    }

    frame = std::move(playback_state.video_queue.front());
    playback_state.video_queue.pop_front();
    lock.unlock();
    playback_state.queue_not_full.notify_one();
    return true;
}

void CloseVideoQueue(PlaybackState &playback_state)
{
    std::lock_guard<std::mutex> lock(playback_state.queue_mutex);
    playback_state.queue_closed = true;
    playback_state.queue_not_empty.notify_all();
    playback_state.queue_not_full.notify_all();
}

void SetPaused(PlaybackState &playback_state, bool paused)
{
    {
        std::lock_guard<std::mutex> lock(playback_state.pause_mutex);
        playback_state.paused = paused;
    }
    playback_state.pause_changed.notify_all();
}

bool WaitIfPaused(PlaybackState &playback_state, VideoPlaybackClock *playback_clock = nullptr)
{
    std::unique_lock<std::mutex> lock(playback_state.pause_mutex);
    if (playback_state.paused && playback_clock)
    {
        playback_clock->pause(SteadyClock::now());
    }

    playback_state.pause_changed.wait(lock, [&playback_state]()
    {
        return playback_state.quit_requested.load() || !playback_state.paused;
    });

    if (playback_state.quit_requested.load())
    {
        return false;
    }

    if (playback_clock)
    {
        playback_clock->resume(SteadyClock::now());
    }

    return true;
}

bool IsBeforePlaybackStart(double pts_seconds, double start_seconds)
{
    return start_seconds > 0.0 &&
           pts_seconds >= 0.0 &&
           pts_seconds + kSeekPrerollToleranceSeconds < start_seconds;
}

void PublishSyncStart(PlaybackSession &session, double pts_seconds, double fallback_seconds)
{
    std::lock_guard<std::mutex> lock(session.sync_start_mutex);
    if (!session.has_sync_start)
    {
        session.sync_start_seconds = pts_seconds >= 0.0 ? pts_seconds : fallback_seconds;
        session.has_sync_start = true;
    }
}

bool TrySyncStart(PlaybackSession &session, double &sync_start_seconds)
{
    std::lock_guard<std::mutex> lock(session.sync_start_mutex);
    if (!session.has_sync_start)
    {
        return false;
    }

    sync_start_seconds = session.sync_start_seconds;
    return true;
}

bool PushTrimmedAudio(
    AudioOutput &audio_output,
    const std::vector<uint8_t> &pcm,
    int sample_rate,
    int channels,
    AVSampleFormat sample_format,
    double pts_seconds,
    double start_seconds,
    bool start_audio_paused)
{
    if (sample_format != AV_SAMPLE_FMT_S16)
    {
        std::cout << "Unexpected audio sample format." << std::endl;
        return false;
    }

    if (sample_rate <= 0 || channels <= 0)
    {
        return false;
    }

    const int bytes_per_sample = av_get_bytes_per_sample(sample_format);
    if (bytes_per_sample <= 0)
    {
        return false;
    }

    const size_t bytes_per_audio_sample = static_cast<size_t>(bytes_per_sample) * static_cast<size_t>(channels);
    if (bytes_per_audio_sample == 0)
    {
        return false;
    }

    size_t trim_bytes = 0;
    if (start_seconds > 0.0 && pts_seconds >= 0.0)
    {
        const size_t sample_count = pcm.size() / bytes_per_audio_sample;
        const double duration_seconds = static_cast<double>(sample_count) / static_cast<double>(sample_rate);
        if (pts_seconds + duration_seconds <= start_seconds + kSeekPrerollToleranceSeconds)
        {
            return true;
        }

        const double trim_seconds = start_seconds - pts_seconds;
        if (trim_seconds > kSeekPrerollToleranceSeconds)
        {
            const size_t trim_samples = std::min(
                sample_count,
                static_cast<size_t>(std::ceil(trim_seconds * static_cast<double>(sample_rate))));
            trim_bytes = trim_samples * bytes_per_audio_sample;
        }
    }

    if (trim_bytes >= pcm.size())
    {
        return true;
    }

    double effective_pts_seconds = pts_seconds;
    if (effective_pts_seconds >= 0.0 && trim_bytes > 0)
    {
        const size_t trim_samples = trim_bytes / bytes_per_audio_sample;
        effective_pts_seconds += static_cast<double>(trim_samples) / static_cast<double>(sample_rate);
    }

    if (!audio_output.is_open())
    {
        if (!audio_output.open(sample_rate, channels, start_audio_paused))
        {
            return false;
        }
    }

    return audio_output.push_pcm(pcm.data() + trim_bytes, pcm.size() - trim_bytes, effective_pts_seconds);
}

enum class FrameRenderResult
{
    rendered,
    dropped,
    stopped,
    failed,
};

FrameRenderResult RenderVideoFrame(
    VideoOutput &video_output,
    AudioOutput &audio_output,
    const VideoFrameView &frame,
    VideoPlaybackClock &playback_clock,
    PlaybackState &playback_state,
    bool audio_clock_enabled)
{
    (void)audio_output;
    (void)audio_clock_enabled;
    if (frame.pts != AV_NOPTS_VALUE)
    {
        const auto now = SteadyClock::now();
        if (!playback_clock.started)
        {
            playback_clock.start(frame.pts_seconds, now);
        }

        const double late_threshold_seconds = std::chrono::duration<double>(kLateFrameDropThreshold).count();

        while (true)
        {
            if (playback_state.quit_requested.load())
            {
                return FrameRenderResult::stopped;
            }

            if (!WaitIfPaused(playback_state, &playback_clock))
            {
                return FrameRenderResult::stopped;
            }

            const auto current = SteadyClock::now();
            const double clock_seconds = playback_clock.elapsed_seconds(current);
            const double target_seconds = frame.pts_seconds - playback_clock.first_pts_seconds;

            if (clock_seconds >= target_seconds)
            {
                if (clock_seconds - target_seconds > late_threshold_seconds)
                {
                    return FrameRenderResult::dropped;
                }
                break;
            }

            const auto remaining = std::chrono::duration<double>(target_seconds - clock_seconds);
            const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            const auto sleep_time = std::max(std::chrono::milliseconds(1), std::min(kFrameWaitInterval, remaining_ms));
            std::this_thread::sleep_for(sleep_time);
        }
    }

    if (playback_state.quit_requested.load())
    {
        return FrameRenderResult::stopped;
    }

    if (!video_output.render_frame(frame))
    {
        return FrameRenderResult::failed;
    }

    video_output.present();
    return FrameRenderResult::rendered;
}

void RenderThreadMain(VideoOutput &video_output, AudioOutput &audio_output, PlaybackSession &session)
{
    if (!video_output.make_current())
    {
        session.state.quit_requested.store(true);
        session.state.render_finished.store(true);
        CloseVideoQueue(session.state);
        return;
    }

    VideoPlaybackClock playback_clock;
    OwnedVideoFrame frame;
    while (!session.state.quit_requested.load())
    {
        if (!WaitIfPaused(session.state, &playback_clock))
        {
            break;
        }

        if (!PopVideoFrame(session.state, frame))
        {
            break;
        }

        const bool release_audio_after_render = !session.audio_released.load();

        const FrameRenderResult result = RenderVideoFrame(
            video_output,
            audio_output,
            frame.view,
            playback_clock,
            session.state,
            !release_audio_after_render);

        if (result == FrameRenderResult::failed)
        {
            std::cout << "Video render failed." << std::endl;
            session.state.quit_requested.store(true);
            CloseVideoQueue(session.state);
            break;
        }

        if (result == FrameRenderResult::rendered || result == FrameRenderResult::dropped)
        {
            if (release_audio_after_render)
            {
                session.audio_released.store(true);
                audio_output.resume();
            }
        }

        if (result == FrameRenderResult::rendered)
        {
            std::lock_guard<std::mutex> lock(session.position_mutex);
            session.latest_video_position_seconds = frame.view.pts_seconds;
            session.has_latest_position = true;
        }

        if (result == FrameRenderResult::stopped)
        {
            break;
        }
    }

    if (!video_output.clear_current())
    {
        std::cout << "Could not release OpenGL context from render thread." << std::endl;
    }

    session.state.render_finished.store(true);
}

void StartPlaybackSession(
    MediaPlayer &player,
    VideoOutput &video_output,
    AudioOutput &audio_output,
    PlaybackSession &session,
    double start_seconds,
    bool sync_audio_to_first_video)
{
    player.set_video_frame_callback(
        [&session, start_seconds](const VideoFrameView &frame)
        {
            if (session.state.quit_requested.load())
            {
                return false;
            }

            if (IsBeforePlaybackStart(frame.pts_seconds, start_seconds))
            {
                return true;
            }

            PublishSyncStart(session, frame.pts_seconds, start_seconds);
            return PushVideoFrame(session.state, frame);
        });

    player.set_audio_frame_callback(
        [&session, &audio_output, start_seconds, sync_audio_to_first_video](const std::vector<uint8_t> &pcm, int sample_rate, int channels, AVSampleFormat sample_format, double pts_seconds)
        {
            if (!WaitIfPaused(session.state))
            {
                return false;
            }

            double audio_start_seconds = start_seconds;
            if (sync_audio_to_first_video)
            {
                if (!TrySyncStart(session, audio_start_seconds))
                {
                    return true;
                }
            }

            const bool start_audio_paused = !session.audio_released.load();
            return PushTrimmedAudio(audio_output, pcm, sample_rate, channels, sample_format, pts_seconds, audio_start_seconds, start_audio_paused);
        });

    session.render_thread = std::thread(
        [&video_output, &audio_output, &session]()
        {
            RenderThreadMain(video_output, audio_output, session);
        });

    session.decode_thread = std::thread(
        [&player, &session]()
        {
            const bool decode_success = player.decode_all();
            session.state.decode_finished.store(true);
            if (!decode_success && !session.state.quit_requested.load())
            {
                std::cout << "Decode loop stopped unexpectedly." << std::endl;
                session.state.quit_requested.store(true);
            }

            CloseVideoQueue(session.state);
        });
}

void StopPlaybackSession(PlaybackSession &session)
{
    session.state.quit_requested.store(true);
    session.state.pause_changed.notify_all();
    CloseVideoQueue(session.state);

    if (session.decode_thread.joinable())
    {
        session.decode_thread.join();
    }

    if (session.render_thread.joinable())
    {
        session.render_thread.join();
    }
}

bool TryLatestVideoPositionSeconds(PlaybackSession &session, double &position_seconds)
{
    std::lock_guard<std::mutex> lock(session.position_mutex);
    if (session.has_latest_position)
    {
        position_seconds = session.latest_video_position_seconds;
        return true;
    }
    return false;
}

double PlaybackPositionOrFallback(PlaybackSession *session, double fallback_seconds)
{
    if (!session)
    {
        return fallback_seconds;
    }

    double latest_seconds = fallback_seconds;
    if (TryLatestVideoPositionSeconds(*session, latest_seconds))
    {
        return latest_seconds;
    }

    return fallback_seconds;
}

bool WasKeyPressed(VideoOutput &video_output, int virtual_key, bool &was_down)
{
    const bool queued_press = video_output.consume_key_press(virtual_key);
    const SHORT state = GetAsyncKeyState(virtual_key);
    const bool is_down = (state & 0x8000) != 0;
    const bool edge_press = is_down && !was_down;
    was_down = is_down;
    return queued_press || edge_press;
}
}

PlaybackPresenter::PlaybackPresenter(MediaPlayer &player, VideoOutput &video_output)
    : player_(player),
      video_output_(video_output)
{
}

bool PlaybackPresenter::run()
{
    if (!video_output_.clear_current())
    {
        std::cout << "Could not release OpenGL context before starting render thread." << std::endl;
    }

    bool quit_requested = false;
    bool paused = false;
    double requested_position_seconds = 0.0;
    bool seek_pending = false;
    SteadyClock::time_point last_seek_request_time = SteadyClock::now();
    bool escape_down = false;
    bool space_down = false;
    bool f11_down = false;
    bool left_down = false;
    bool right_down = false;
    std::unique_ptr<PlaybackSession> session;

    AudioOutput audio_output;

    auto stop_active_session = [&]()
    {
        if (session)
        {
            SetPaused(session->state, false);
            session->state.quit_requested.store(true);
            session->state.pause_changed.notify_all();
            CloseVideoQueue(session->state);
            audio_output.request_stop();
            StopPlaybackSession(*session);
            audio_output.close();
            session.reset();
        }
    };

    auto start_session = [&](double start_seconds, bool start_paused, bool force_seek)
    {
        if (force_seek || start_seconds > 0.0)
        {
            if (!player_.seek_to_seconds(start_seconds))
            {
                return false;
            }
        }

        session = std::make_unique<PlaybackSession>();
        SetPaused(session->state, start_paused);
        StartPlaybackSession(player_, video_output_, audio_output, *session, start_seconds, force_seek);
        return true;
    };

    if (!start_session(0.0, false, false))
    {
        return false;
    }

    auto toggle_pause = [&]()
    {
        if (!paused)
        {
            if (!session)
            {
                return;
            }

            requested_position_seconds = PlaybackPositionOrFallback(session.get(), requested_position_seconds);
            SetPaused(session->state, true);
            audio_output.pause();
            paused = true;
            std::cout << "Paused." << std::endl;
            return;
        }

        if (!session)
        {
            if (!start_session(requested_position_seconds, false, true))
            {
                quit_requested = true;
                return;
            }
            paused = false;
            std::cout << "Resumed." << std::endl;
            return;
        }

        audio_output.resume();
        SetPaused(session->state, false);
        paused = false;
        std::cout << "Resumed." << std::endl;
    };

    while (!quit_requested)
    {
        if (!video_output_.poll_events())
        {
            quit_requested = true;
            break;
        }

        if (WasKeyPressed(video_output_, VK_ESCAPE, escape_down))
        {
            quit_requested = true;
            break;
        }

        if (WasKeyPressed(video_output_, VK_SPACE, space_down))
        {
            toggle_pause();
        }

        if (WasKeyPressed(video_output_, 'P', space_down))
        {
            toggle_pause();
        }

        if (WasKeyPressed(video_output_, VK_F11, f11_down))
        {
            video_output_.toggle_fullscreen();
        }

        auto queue_seek = [&](double delta)
        {
            if (!seek_pending)
            {
                requested_position_seconds = paused
                                                 ? requested_position_seconds
                                                 : PlaybackPositionOrFallback(session.get(), requested_position_seconds);
            }

            requested_position_seconds += delta;
            if (requested_position_seconds < 0.0)
            {
                requested_position_seconds = 0.0;
            }
            last_seek_request_time = SteadyClock::now();
            seek_pending = true;
        };

        if (WasKeyPressed(video_output_, VK_LEFT, left_down))
        {
            queue_seek(-kSeekStepSeconds);
        }
        if (WasKeyPressed(video_output_, VK_RIGHT, right_down))
        {
            queue_seek(kSeekStepSeconds);
        }

        if (seek_pending &&
            (paused || SteadyClock::now() - last_seek_request_time >= kSeekDebounceInterval))
        {
            const double target_seconds = requested_position_seconds;
            seek_pending = false;

            if (paused)
            {
                stop_active_session();
                requested_position_seconds = target_seconds;
                std::cout << "Queued paused seek target: " << requested_position_seconds << " seconds." << std::endl;
                continue;
            }

            stop_active_session();
            std::cout << "Seeking to: " << target_seconds << " seconds." << std::endl;
            if (!start_session(target_seconds, paused, true))
            {
                quit_requested = true;
                break;
            }
        }

        if (!session)
        {
            Sleep(1);
            continue;
        }

        if (session->state.render_finished.load())
        {
            quit_requested = true;
            break;
        }

        Sleep(1);
    }

    stop_active_session();
    return true;
}
