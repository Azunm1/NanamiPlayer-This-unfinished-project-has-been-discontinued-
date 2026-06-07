#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct VideoFrameView;

class VideoOutput
{
public:
    bool open(const std::string &title, int width, int height);
    void close();
    bool make_current();
    bool clear_current();
    bool poll_events();
    bool render_frame(const VideoFrameView &frame);
    void present();
    bool toggle_fullscreen();
    bool consume_key_press(int virtual_key);

    bool is_open() const;

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // GPU-side color conversion parameters, laid out as two float4 registers to
    // satisfy the 16-byte constant-buffer alignment rule. Mirrors the CPU math
    // that previously lived in ConvertYuv420ToBgra.
    struct ColorParams
    {
        float kr = 0.2126f;
        float kb = 0.0722f;
        float y_scale = 1.0f;  // a_y
        float y_offset = 0.0f; // b_y (normalized)
        float c_scale = 1.0f;  // a_c
        float c_offset = 0.0f; // b_c (normalized)
        float y_texel_x = 0.0f;
        float y_texel_y = 0.0f;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    bool register_window_class();
    bool create_window(const std::string &title, int width, int height);

    bool ensure_device();
    bool compile_shaders();
    bool ensure_render_target(int width, int height);
    bool ensure_plane_textures(int width, int height, int sample_bytes);
    bool upload_planes(const VideoFrameView &frame);
    void update_color_params(const VideoFrameView &frame);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    bool quit_requested_ = false;
    std::mutex input_mutex_;
    std::vector<int> key_presses_;
    bool fullscreen_ = false;
    DWORD windowed_style_ = 0;
    WINDOWPLACEMENT windowed_placement_ = {sizeof(WINDOWPLACEMENT)};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_view_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    ComPtr<ID3D11Buffer> color_params_buffer_;
    ComPtr<ID3D11Texture2D> plane_textures_[3];
    ComPtr<ID3D11ShaderResourceView> plane_srvs_[3];

    bool device_ready_ = false;
    int swap_chain_width_ = 0;
    int swap_chain_height_ = 0;
    int texture_width_ = 0;  // Y-plane width
    int texture_height_ = 0; // Y-plane height
    int texture_sample_bytes_ = 1;
    double display_aspect_ratio_ = 0.0;
    bool has_frame_ = false;
};
