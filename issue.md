# 問題

1. [x] 畫面鋸齒感嚴重，未實現1080p
   - 啟動窗口保持 1280x720，不再打開即全屏。
   - 1080p 片源仍按原始分辨率解碼，窗口內按比例縮放顯示。x
   - 普通窗口可用系統最大化按鈕最大化，F11 可切換全屏。
   - GDI 縮放使用 HALFTONE，降低縮放時的鋸齒。

2. [x] pause使用之後恢復畫面 音頻卡死 程序未響應
   - WinMM pause/resume/restart 已移出 AudioOutput mutex，避免和 waveOut 回調互相等待。
   - 已用自動按鍵煙測驗證 pause/resume 後程序仍響應。

3. [x] seek問題未知
   - 已用自動按鍵煙測驗證連續兩次右方向 seek 後程序仍響應，且解碼會話確實重啟。
   
4. [x] 處理seek過多導致音頻消失問題（儅音頻消失後再使用一邊seek會修復）
   - seek 會重啟 playback session 並對 FFmpeg decoder 呼叫 `avcodec_flush_buffers()`。
   - 音頻 decoder 的 `SwrContext` 之前沒有在 seek flush 時重置，可能保留 seek 前的重採樣 delay/state，導致多次時間線跳躍後音頻狀態不乾淨。
   - 已在 `StreamDecoder::flush_buffers()` 中清理 `SwrContext`、音頻格式快取與 input layout，讓每次 seek 後的第一個音頻 frame 重新建立乾淨的 resampler。
   - 已通過 build，並用自動按鍵 smoke test 連續多次右方向 seek 後 ESC 退出，程序未崩潰或卡死。

5. [x] 解決畫面仍偏糊問題 與mpv相比較video流并沒有達到1080p 同時稍稍偏色
   - 原因之一是 `main.cpp` 固定用 1280x720 client size 開窗口，1080p 片源一啟動就被下採樣顯示，與 mpv 原始尺寸窗口比較會顯得偏糊。
   - 已讓 `MediaPlayer` 暴露視頻原始寬高，`main.cpp` 優先用片源解析度建立窗口；`assets/test.mkv` 會以 1920x1080 client size 啟動。
   - 原視頻中間格式固定轉成 8-bit `YUV420P`，會讓 10-bit 片源在進 GPU 前丟失精度。
   - 已改成 `YUV420P16LE` 中間格式，D3D11 plane texture 改用 `R16_UNORM`，shader 的 limited/full range offset 和 scale 也改為依 component bit depth 計算。
   - 已通過 build 和短時間啟動/ESC smoke test；清晰度與偏色仍需人工對 mpv 做視覺確認。
   - 後續按第 7 項要求恢復 1280x720 啟動窗口，保留 16-bit YUV 路徑。

6. [x] pause 無法使用
   - 已讓播放器窗口在建立後主動 `SetForegroundWindow` / `SetActiveWindow` / `SetFocus`，避免從 Zed terminal 啟動後焦點留在 terminal，導致 Space 沒進播放器。
   - 快捷鍵判定改為只消費窗口 `WM_KEYDOWN` 收到的按鍵事件，不再混用 `GetAsyncKeyState` 的全域狀態。
   - Space 和 P 都可 pause/resume；控制台會輸出 `Paused.` / `Resumed.` 方便驗證。
   - 已用自動按鍵 smoke test 驗證 Space pause、Space resume、ESC 退出，程序未崩潰或卡死。

7. [x] 畫面仍偏糊 建議不要使用原始尺寸打開 仍保留1280x720打開
   - 已按要求把 `main.cpp` 啟動窗口恢復為 1280x720。
   - 保留 `YUV420P16LE` + `R16_UNORM` 的 16-bit YUV 路徑，避免 10-bit 片源提前降成 8-bit。
   - 在 pixel shader 加入輕量 luma sharpen，改善 1920x1080 片源縮放到 1280x720 時的糊感。

8. [x] seek又出現音頻消失問題
   - seek 重啟 session 時，音頻會先 paused，等首個視頻 frame 處理後再 release。
   - 原邏輯只在 frame 實際 rendered 時 release audio；如果 seek 後首個 frame 被 late-drop，音頻可能保持 paused，表現為音頻消失。
   - 已改成首個 frame 只要被 render thread 處理過，無論 rendered 或 dropped，都會 release audio。
   - 已用自動按鍵 smoke test 連續 16 次右方向 seek 後 ESC 退出，程序未崩潰或卡死。
   - 針對「pause 後多次前後 seek，恢復時音頻卡頓/不同步」又改了一次：paused 狀態下 seek 只更新目標時間並停掉當前 session，不再反覆建立 paused session；恢復播放時才從最後目標位置啟動一次新 session。
   - 已用自動按鍵 smoke test 驗證：Space pause -> 多次左右 seek -> Space resume -> ESC 退出，輸出有 `Paused.`、多次 `Queued paused seek target`、`Resumed.`。

9. [x] 快速多次 seek 時需要緩衝，避免每次按鍵都卡頓一段時間才播放
   - 原因是播放中每按一次方向鍵都會立即 stop session、seek、重啟 decode/render/audio，快速連按會造成多次管線重啟。
   - 已加入 seek debounce：播放中連續 seek 只更新目標時間，等 180ms 沒有新的 seek 後才執行最後一次 seek。
   - 視頻隊列容量從 4 提升到 8，給 render thread 更大的短期緩衝。
   - 已用自動按鍵 smoke test 驗證播放中 12 次快速右方向 seek 只觸發最後一次 seek session，程序未崩潰或卡死。

10. [x] 向左 seek 失效
   - 快捷鍵處理在改為只消費窗口 `WM_KEYDOWN` 後，部分情況下方向鍵事件可能不穩。
   - 已加入安全的 `GetAsyncKeyState` 邊緣 fallback：只在按鍵從未按下變為按下時觸發，不使用容易殘留的 low-bit sticky 狀態。
   - 已加入 `Seeking to: ... seconds.` 日誌方便確認 seek 方向與目標時間。
   - 已用自動按鍵 smoke test 驗證：先右 seek 到 `8.921s`，再左 seek 到 `4.343s`，左 seek 目標時間確實變小。
9.  
