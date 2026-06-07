# Nanami Player Handoff

## Current objective
Continue developing `D:\nanami_player`, a C++17 / CMake / MSYS2 UCRT64 FFmpeg player prototype. The current codebase uses a Win32 presentation layer with Direct3D 11 for video and `waveOut` for audio; older SDL2/OpenGL notes in this handoff are historical.

The current focus shifted from basic video pacing to playback architecture:
- video should continue while the window is being dragged or resized
- rendering should use more than one CPU core
- audio is wired through an SDL callback path, but sync still needs validation

## Project shape
- `MediaSource`
  - owns `AVFormatContext`
  - open / find stream info / dump / close
- `StreamDecoder`
  - owns one stream decoder
  - `AVCodecContext`, `AVFrame`
  - packet send / receive
  - subtitle decode with `avcodec_decode_subtitle2`
  - video conversion to `YUV420P`
  - audio conversion to `S16 stereo PCM`
- `MediaPlayer`
  - opens all stream decoders
  - reads packets from FFmpeg
  - dispatches video and audio frames through callbacks
- `VideoOutput`
  - owns SDL2 window + OpenGL context
  - uploads Y / U / V planes as `GL_RED` textures
  - shader converts YUV to RGB
  - maintains display aspect ratio

## Current implementation status
### Video rendering
- Video frames are converted to `YUV420P`
- `VideoOutput` supports:
  - `BT.709`
  - `BT.2020`
  - limited / full range handling
  - DAR letterbox / pillarbox
- The YUV shader matrix upload was fixed earlier so the matrix is passed correctly to OpenGL
- `VideoFrameView` exposes:
  - `y_data / u_data / v_data`
  - `pts`, `pts_seconds`
  - `width / height`
  - `y_line_size / u_line_size / v_line_size`
  - `display_aspect_ratio`
  - `color_range`
  - `color_space`
  - `color_primaries`

### Playback architecture change in progress
`src/main.cpp` was changed from a single-threaded decode/render loop into a split pipeline:
- decode thread: `MediaPlayer::decode_all()`
- render thread: drains a small queued set of copied video frames
- main thread: polls SDL events

The intent is:
- keep video decode and render moving on separate CPU cores
- avoid blocking playback when the OS enters the window drag / resize modal loop
- avoid fast-forward catch-up after the drag ends

### New thread model details
- `PlaybackState` holds:
  - `quit_requested`
  - `render_finished`
  - `decode_finished`
  - a bounded `std::deque<OwnedVideoFrame>`
  - condition variables for queue backpressure
- `OwnedVideoFrame` copies each decoded frame into owned storage before enqueueing
- `RenderThreadMain()` owns the GL context via `VideoOutput::make_current()`
- `VideoOutput` gained:
  - `make_current()`
  - `clear_current()`

## Important files
- [`src/main.cpp`](D:/nanami_player/src/main.cpp)
- [`src/media_source.cpp`](D:/nanami_player/src/media_source.cpp)
- [`src/media_player.cpp`](D:/nanami_player/src/media_player.cpp)
- [`src/stream_decoder.cpp`](D:/nanami_player/src/stream_decoder.cpp)
- [`src/stream_decoder.h`](D:/nanami_player/src/stream_decoder.h)
- [`src/video_output.cpp`](D:/nanami_player/src/video_output.cpp)
- [`src/video_output.h`](D:/nanami_player/src/video_output.h)
- [`aim.txt`](D:/nanami_player/aim.txt)

## Recent changes worth knowing
- 2026-06-06: fixed a seek/audio regression by resetting `StreamDecoder`'s audio `SwrContext` and cached audio format state from `flush_buffers()`, so every seek starts with a clean resampler state.
- 2026-06-06: improved 1080p visual fidelity by opening the window at the source video size instead of fixed 1280x720, and by converting decoded video to `YUV420P16LE` uploaded as D3D11 `R16_UNORM` planes.
- 2026-06-06: restored the startup window to 1280x720 per follow-up issue, kept the 16-bit YUV path, and added a mild luma sharpen in the pixel shader for downscaled playback.
- 2026-06-06: fixed another seek/audio disappearance path by releasing paused audio when the first frame is processed by the render thread even if that frame is late-dropped.
- 2026-06-06: fixed pause focus/key handling by focusing the player window after creation and using window keydown events instead of `GetAsyncKeyState` for playback controls.
- 2026-06-06: changed paused seek semantics: seek while paused now only queues the target and stops the active session; resume starts one fresh session at the final target to avoid repeated paused audio/session churn.
- 2026-06-06: added playback seek debounce for rapid repeated seek; seek requests coalesce for 180ms and only the final target restarts the session. Video queue capacity increased from 4 to 8 frames.
- 2026-06-06: restored safe `GetAsyncKeyState` edge fallback for playback controls and added seek target logging; left seek was verified by a right-then-left smoke test.
- 2026-06-06: new standing workflow rule from the user: after code updates, commit changes with git. Also keep `CLAUDE.md` and this handoff updated when conventions, architecture, or test expectations change.
- `VideoOutput::make_current()` and `clear_current()` were added to move GL context ownership to the render thread
- `main.cpp` now has:
  - a bounded video queue
  - a render thread
  - a decode thread
  - copied video frames for thread safety
- The old PTS sleep loop was replaced by render-thread timing logic with late-frame dropping
- The working snapshot policy now keeps only the latest snapshot directory; after each small version, the previous snapshot is deleted and replaced by the new one
- The last successful build before this handoff was `cmake --build build -j 4`

## Known issues / risks
- The seek-heavy audio disappearance issue in `issue.md` item 4 is marked fixed after resetting resampler state on seek; it still deserves manual listening validation because the automated smoke test can only verify responsiveness.
- The blur/color issue in `issue.md` item 5 is marked fixed from the code side; it still needs manual A/B visual comparison against mpv.
- `issue.md` items 6-8 are marked fixed after pause/resume and repeated-seek smoke tests; manual listening/visual validation is still recommended.
- Additional paused-seek smoke test covers Space pause -> repeated left/right seek -> Space resume -> ESC, with `Paused.` / `Queued paused seek target` / `Resumed.` logs.
- Fast seek smoke test covers 12 rapid right-arrow presses and confirms they coalesce into a single final seek session.
- Left seek smoke test covers right seek to about 8.9s followed by left seek to about 4.3s, confirming the target moves backward.
- Required playback regression coverage after future code changes: build, right seek, left seek, pause/resume, rapid repeated seek, and whenever possible pause -> random/repeated left/right seek -> resume with A/V sync/manual listening check.
- The new multithreaded playback architecture is still in flight and needs runtime validation
- `VideoOutput::poll_events()` currently stays on the main thread, so the next step is to verify that Windows drag / resize no longer stalls the render thread in practice
- A/V sync is still not implemented
- Subtitle rendering is still not implemented
- `aim.txt` is still inconsistent around items 9-13; some entries are commented, some are plain numbered lines

## Build / run
Manual build:
```powershell
$env:PATH='C:\msys64\ucrt64\bin;' + $env:PATH
cmake --build build -j 4
```

Run:
```powershell
$env:PATH='C:\msys64\ucrt64\bin;' + $env:PATH
.\\build\\nanami_player.exe .\\assets\\test.mkv
```

## Observed media metadata for `assets/test.mkv`
- `hevc`
- `1920x1080`
- `yuv420p10le`
- `color_range=tv`
- `color_space=bt709`
- `color_transfer=bt709`
- `color_primaries=bt709`

## Suggested next steps
1. Run the new multithreaded pipeline interactively and verify window dragging no longer pauses video playback.
2. Fix any thread / context ownership bugs that appear under real SDL drag / resize behavior.
3. Decide whether the render queue should be the long-term seam or whether a dedicated presenter module is warranted.
4. Add audio clock and A/V sync.
5. Clean up `aim.txt` formatting so completed items are marked consistently.

## Suggested skills
- `diagnose` - for runtime issues in the new multithreaded playback path
- `tdd` - once a stable seam exists for queueing or timing behavior
- `improve-codebase-architecture` - for the new playback split if it needs a cleaner seam
- `zoom-out` - if a future agent needs a broader map before touching the playback pipeline

## Session note
This handoff follows the work on drag-induced playback stalling. The latest implementation attempt moved video decode and render off the main thread so that dragging the window should no longer block video playback.
For future work, preserve the rolling snapshot rule: keep one current snapshot only, delete the previous snapshot after each small version, and write the new state into the replacement snapshot before handing off.
