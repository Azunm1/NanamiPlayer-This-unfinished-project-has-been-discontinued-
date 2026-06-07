# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`nanami_player` is a from-scratch C++17 media player prototype built on FFmpeg with a pure **Win32** presentation layer (**Direct3D 11** for video, `waveOut` for audio — no SDL/OpenGL). It demuxes a container, decodes video/audio/subtitle streams, and presents video through a split decode/render threading model. This is a learning/prototype codebase: a single executable target, Windows-only, no test suite yet.

## Build & Run

Toolchain is **MSYS2 UCRT64**. FFmpeg headers/libs are expected under `C:/msys64/ucrt64` (hardcoded as `FFMPEG_ROOT` in `CmakeLists.txt`). The UCRT64 `bin` dir must be on `PATH` for both building and running so the FFmpeg DLLs resolve at link and load time.

```powershell
# Build (PowerShell)
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cmake --build build -j 4

# Configure from scratch if build/ is missing
cmake -S . -B build -G "Unix Makefiles"

# Run — takes an optional media path arg (defaults to assets/test.mkv)
.\build\nanami_player.exe .\assets\test.mkv
```

`assets/test.mkv` is the standard test clip: HEVC 1920x1080 `yuv420p10le`, BT.709, limited (tv) range.

> The CMake file is named `CmakeLists.txt` (non-standard casing) — relevant on case-sensitive tooling.

## Architecture

A layered pull-based decode path feeds a threaded presenter. Each class owns exactly one FFmpeg resource tier:

- **`MediaSource`** — owns the `AVFormatContext`; opens the file, dumps format, exposes streams. Demuxing (`av_read_frame`) is driven by `MediaPlayer`, not here.
- **`StreamDecoder`** — owns one stream's `AVCodecContext`/`AVFrame`. Converts video to `YUV420P16LE` (swscale) and audio to interleaved **S16 stereo PCM** (swresample); also decodes subtitles (`avcodec_decode_subtitle2`). Emits results via callbacks (`VideoFrameCallback`, `AudioFrameCallback`) defined in `stream_decoder.h`. `VideoFrameView` is a **non-owning** view (valid only during the callback) carrying plane pointers, line sizes, sample bytes / component depth, PTS in seconds, DAR, and color metadata (range/space/primaries). Audio is delivered as a `std::vector<uint8_t>` of PCM.
- **`MediaPlayer`** — opens a `StreamDecoder` per stream. `decode_all(max_seconds)` is a blocking demux→decode loop that fans frames out through the per-decoder callbacks (set via `set_video_frame_callback` / `set_audio_frame_callback`). Also has `seek_to_seconds`, decode/skip/frame counters, and `video_width/height`.
- **`PlaybackPresenter`** — the threading seam. Constructed with `(MediaPlayer&, VideoOutput&)`; `run()` itself stays on the **main thread**, owns the single `AudioOutput`, polls events + keyboard, and manages playback **sessions**. Each `PlaybackSession` wraps a `PlaybackState` and spawns a **decode thread** (`MediaPlayer::decode_all`) and a **render thread** (`RenderThreadMain`) connected by a bounded `std::deque<OwnedVideoFrame>` (`kVideoQueueCapacity = 8`) with backpressure CVs `queue_not_full` / `queue_not_empty`, plus a separate `pause_changed` CV. The video callback **copies each frame's planes into an `OwnedVideoFrame`** (`CopyVideoFrame`) before enqueue — views don't outlive the callback. Audio PCM is pushed into `AudioOutput`.
  - **Seek = session restart.** There is no in-place seek on the playback path. Playing-state seeks are debounced for 180ms so rapid repeated key presses coalesce into one final seek. `run()` tears down the active session (`stop_active_session` → close `AudioOutput`), calls `MediaPlayer::seek_to_seconds`, then starts a fresh session (`start_session`) from the target time. Paused-state seeks only update the queued target and stop the active session; resume starts one fresh session at the final target. Seek deltas are `kSeekStepSeconds = 5.0`.
  - **Pause/clock.** Render timing uses a steady-clock `VideoPlaybackClock` that subtracts paused durations; pause is coordinated through `PlaybackState::paused` + `pause_changed`, gating both the decode-side enqueue and the render-side wait.
- **`VideoOutput`** — owns the Win32 window and a **Direct3D 11** render pipeline. Opens **windowed at the requested size** (1280×720 from `main.cpp`) — it does not auto-fullscreen. The D3D11 device + flip-model swap chain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) are created **lazily on the render thread** in `ensure_device()` (first `render_frame`), so the device lives on the thread that uses it. Each YUV420P16LE frame's three planes are uploaded to `R16_UNORM` dynamic textures (`upload_planes`, row-by-row to handle swscale padding vs. GPU row pitch); a fullscreen-triangle vertex shader + YUV→RGB pixel shader do color conversion on the GPU and applies a mild luma sharpen for 1080p→720p downscale. The conversion math (BT.601/709/2020 coefficient selection, limited/full range scale+offset) is fed in via the `ColorParams` constant buffer (`update_color_params`) using the frame component bit depth. DAR letterbox/pillarbox is applied as the D3D viewport rect against a black-cleared RTV; linear sampling handles scaling. `present()` calls `Present(1, 0)` (vsync). `toggle_fullscreen()` saves/restores `WINDOWPLACEMENT` + window style for borderless fullscreen. `make_current()`/`clear_current()` are no-ops kept for interface compatibility; `poll_events()` stays on the main thread.
- **`AudioOutput`** — Win32 **`waveOut`** (`mmsystem`, `HWAVEOUT`, `WAVEHDR` blocks). `push_pcm()` queues S16 blocks with buffer-space backpressure; can `open(..., start_paused)`; `clock_seconds()` is the intended master clock for A/V sync. **Invariant**: the `waveOut` pause/resume/restart calls run **outside** the `AudioOutput` mutex — holding the mutex across them deadlocks against the `waveOut` callback (this caused an earlier pause-hang regression; see `issue.md`).

`main.cpp` is thin: open `MediaPlayer` → open `VideoOutput` → `clear_current()` (no-op now; kept for interface symmetry) → `PlaybackPresenter::run()` → `make_current()` (no-op) → close. The D3D11 device is created on the render thread on first frame, not here.

### Keyboard controls
`ESC` quit · `SPACE` or `P` pause/resume · `F11` toggle fullscreen · `←`/`→` seek ∓5s (debounced session restart). Key handling consumes Win32 `WM_KEYDOWN` events and also has a safe `GetAsyncKeyState` edge fallback; controls fire once per physical press. Console logs include `Paused.`, `Resumed.`, `Queued paused seek target: ...`, and `Seeking to: ... seconds.` for smoke-test verification.

### Threading rationale (preserve when editing the playback path)
The decode/render split exists specifically so **Windows window drag/resize (the OS modal loop) does not stall video**, and to spread decode vs. render across cores. Invariants to keep: frames are copied before crossing the queue; event polling stays on the main thread; the queue is bounded with CV backpressure.

## Known gaps (per handoff)
- **A/V sync is not implemented** — the audio-clock plumbing now exists (`AudioOutput::clock_seconds()`, `PlaybackSession::audio_released`, an `audio_clock_enabled` flag threaded into `RenderVideoFrame`), but `RenderVideoFrame` deliberately `(void)`s the audio output/flag out and still paces video off the steady-clock `VideoPlaybackClock` with late-frame dropping (`kLateFrameDropThreshold = 100ms`). Wiring the audio clock as the master is the open task.
- **Subtitle rendering is not implemented** — subtitles decode but are not presented.

> `nanami_player_handoff.md` predates the D3D11/waveOut rewrite and still describes SDL2/OpenGL — trust the `src/*.cpp` + the libs linked in `CmakeLists.txt` (`d3d11 dxgi d3dcompiler gdi32 user32 winmm`) over that doc.

## Repo conventions
- **Snapshots**: `snapshots/` keeps a single rolling snapshot of the source tree per "small version." Rule (from the handoff): keep only the latest — delete the previous snapshot and write the new state into its replacement before handing off.
- **`build/`** is a committed CMake output dir; treat it as generated.
- `nanami_player_handoff.md` is the running cross-session status log — update it when architecture or status changes.
- `issue.md` tracks open/closed bugs with the fix rationale (Traditional Chinese). Worth a read before touching pause/seek/scaling — the resolved entries record non-obvious deadlock/regression causes.
- **Git commits**: after code changes, update `issue.md` / `nanami_player_handoff.md` / this file when relevant, then create a git commit. Do not leave completed code work uncommitted unless the user explicitly asks.
- **Playback smoke tests after code changes**: at minimum run build plus right seek, left seek, pause/resume, and rapid seek tests. When possible, also test `Space pause -> random/repeated left/right seek -> Space resume` and check for A/V desync or audio stutter. Automated tests can prove responsiveness and log targets, but manual listening is still required for final A/V sync confidence.
- Code style: `snake_case` methods and `snake_case_` members (trailing underscore), `PascalCase` for free helper functions and structs, `#pragma once` headers. FFmpeg is included inside `extern "C"` blocks; FFmpeg error/timestamp helpers live in `FFmpegUtils` (`ffmpeg_utils.h`).
