#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class AudioOutput
{
public:
    bool open(int sample_rate, int channels, bool start_paused = false);
    void request_stop();
    void close();
    void pause();
    void resume();
    bool push_pcm(const uint8_t *data, size_t size, double pts_seconds = -1.0);
    bool is_open() const;
    bool clock_seconds(double &position_seconds) const;

private:
    struct AudioBlock
    {
        WAVEHDR header = {};
        std::vector<uint8_t> data;
        bool done = false;
    };

    static void CALLBACK WaveOutCallback(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2);

    void cleanup_done_blocks_locked();
    bool wait_for_buffer_space_locked(std::unique_lock<std::mutex> &lock, size_t size);
    size_t playback_position_bytes_locked() const;

    HWAVEOUT device_ = nullptr;
    WAVEFORMATEX format_ = {};
    mutable std::mutex mutex_;
    std::condition_variable buffer_changed_;
    std::deque<std::unique_ptr<AudioBlock>> blocks_;
    bool closed_ = true;
    bool has_clock_ = false;
    double clock_base_seconds_ = 0.0;
    size_t queued_bytes_ = 0;
    int bytes_per_second_ = 0;
};
