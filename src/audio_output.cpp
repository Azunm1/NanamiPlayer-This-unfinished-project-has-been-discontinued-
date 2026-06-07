#include "audio_output.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace
{
constexpr size_t kMaxQueuedAudioBytes = 4 * 1024 * 1024;
constexpr auto kBufferWaitInterval = std::chrono::milliseconds(5);
}

bool AudioOutput::open(int sample_rate, int channels, bool start_paused)
{
    if (sample_rate <= 0 || channels <= 0)
    {
        return false;
    }

    HWAVEOUT existing_device = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_ && format_.nSamplesPerSec == static_cast<DWORD>(sample_rate) &&
            format_.nChannels == static_cast<WORD>(channels))
        {
            existing_device = device_;
        }
    }
    if (existing_device)
    {
        if (start_paused)
        {
            waveOutPause(existing_device);
        }
        else
        {
            waveOutRestart(existing_device);
        }
        return true;
    }

    close();

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEOUT device = nullptr;
    MMRESULT result = waveOutOpen(
        &device,
        WAVE_MAPPER,
        &format,
        reinterpret_cast<DWORD_PTR>(&AudioOutput::WaveOutCallback),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR)
    {
        std::cout << "Could not open WinMM waveOut device: " << result << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_ = device;
        format_ = format;
        bytes_per_second_ = static_cast<int>(format.nAvgBytesPerSec);
        queued_bytes_ = 0;
        has_clock_ = false;
        clock_base_seconds_ = 0.0;
        closed_ = false;
    }

    if (start_paused)
    {
        waveOutPause(device);
    }

    return true;
}

void AudioOutput::request_stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    buffer_changed_.notify_all();
}

void AudioOutput::close()
{
    HWAVEOUT device_to_close = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        device_to_close = device_;
    }

    if (device_to_close)
    {
        waveOutReset(device_to_close);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_done_blocks_locked();
        for (auto &block : blocks_)
        {
            if (block && (block->header.dwFlags & WHDR_PREPARED))
            {
                waveOutUnprepareHeader(device_to_close, &block->header, sizeof(WAVEHDR));
            }
        }
        blocks_.clear();
        queued_bytes_ = 0;
        has_clock_ = false;
        clock_base_seconds_ = 0.0;
        bytes_per_second_ = 0;
        format_ = {};
        device_ = nullptr;
    }

    if (device_to_close)
    {
        waveOutClose(device_to_close);
    }
    buffer_changed_.notify_all();
}

void AudioOutput::pause()
{
    HWAVEOUT device = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device = device_;
    }
    if (device)
    {
        waveOutPause(device);
    }
}

void AudioOutput::resume()
{
    HWAVEOUT device = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device = device_;
    }
    if (device)
    {
        waveOutRestart(device);
    }
}

bool AudioOutput::push_pcm(const uint8_t *data, size_t size, double pts_seconds)
{
    if (!data || size == 0)
    {
        return false;
    }

    auto block = std::make_unique<AudioBlock>();
    block->data.assign(data, data + size);
    block->header.lpData = reinterpret_cast<LPSTR>(block->data.data());
    block->header.dwBufferLength = static_cast<DWORD>(block->data.size());

    HWAVEOUT device = nullptr;
    AudioBlock *queued_block = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || !device_)
        {
            return false;
        }

        if (!wait_for_buffer_space_locked(lock, size))
        {
            return false;
        }

        if (pts_seconds >= 0.0 && (!has_clock_ || queued_bytes_ == 0))
        {
            const size_t device_bytes = playback_position_bytes_locked();
            has_clock_ = true;
            clock_base_seconds_ = pts_seconds -
                                  static_cast<double>(device_bytes) /
                                      static_cast<double>(bytes_per_second_);
        }

        device = device_;
        queued_bytes_ += block->data.size();
        queued_block = block.get();
        blocks_.push_back(std::move(block));
    }

    MMRESULT result = waveOutPrepareHeader(device, &queued_block->header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto iterator = blocks_.begin(); iterator != blocks_.end(); ++iterator)
        {
            if (iterator->get() == queued_block)
            {
                queued_bytes_ -= std::min(queued_bytes_, queued_block->data.size());
                blocks_.erase(iterator);
                break;
            }
        }
        std::cout << "Could not prepare waveOut buffer: " << result << std::endl;
        buffer_changed_.notify_all();
        return false;
    }

    result = waveOutWrite(device, &queued_block->header, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR)
    {
        waveOutUnprepareHeader(device, &queued_block->header, sizeof(WAVEHDR));
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto iterator = blocks_.begin(); iterator != blocks_.end(); ++iterator)
        {
            if (iterator->get() == queued_block)
            {
                queued_bytes_ -= std::min(queued_bytes_, queued_block->data.size());
                blocks_.erase(iterator);
                break;
            }
        }
        std::cout << "Could not write waveOut buffer: " << result << std::endl;
        buffer_changed_.notify_all();
        return false;
    }

    return true;
}

bool AudioOutput::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr;
}

bool AudioOutput::clock_seconds(double &position_seconds) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ || !has_clock_ || bytes_per_second_ <= 0)
    {
        return false;
    }

    const size_t device_bytes = playback_position_bytes_locked();
    position_seconds = clock_base_seconds_ +
                       static_cast<double>(device_bytes) /
                           static_cast<double>(bytes_per_second_);
    return true;
}

void CALLBACK AudioOutput::WaveOutCallback(HWAVEOUT, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
{
    if (message != WOM_DONE || !instance || !param1)
    {
        return;
    }

    auto *audio_output = reinterpret_cast<AudioOutput *>(instance);
    auto *completed_header = reinterpret_cast<WAVEHDR *>(param1);
    {
        std::lock_guard<std::mutex> lock(audio_output->mutex_);
        for (auto &block : audio_output->blocks_)
        {
            if (block && &block->header == completed_header)
            {
                block->done = true;
                break;
            }
        }
    }
    audio_output->buffer_changed_.notify_all();
}

void AudioOutput::cleanup_done_blocks_locked()
{
    for (auto iterator = blocks_.begin(); iterator != blocks_.end();)
    {
        AudioBlock *block = iterator->get();
        if (!block || !block->done)
        {
            ++iterator;
            continue;
        }

        if (device_ && (block->header.dwFlags & WHDR_PREPARED))
        {
            waveOutUnprepareHeader(device_, &block->header, sizeof(WAVEHDR));
        }
        queued_bytes_ -= std::min(queued_bytes_, block->data.size());
        iterator = blocks_.erase(iterator);
    }
}

bool AudioOutput::wait_for_buffer_space_locked(std::unique_lock<std::mutex> &lock, size_t size)
{
    while (!closed_)
    {
        cleanup_done_blocks_locked();
        if (queued_bytes_ + size <= kMaxQueuedAudioBytes)
        {
            return true;
        }
        buffer_changed_.wait_for(lock, kBufferWaitInterval);
    }
    return false;
}

size_t AudioOutput::playback_position_bytes_locked() const
{
    if (!device_)
    {
        return 0;
    }

    MMTIME time = {};
    time.wType = TIME_BYTES;
    if (waveOutGetPosition(device_, &time, sizeof(time)) != MMSYSERR_NOERROR)
    {
        return 0;
    }

    if (time.wType == TIME_BYTES)
    {
        return static_cast<size_t>(time.u.cb);
    }

    if (time.wType == TIME_SAMPLES && format_.nBlockAlign > 0)
    {
        return static_cast<size_t>(time.u.sample) * static_cast<size_t>(format_.nBlockAlign);
    }

    return 0;
}
