#include "video_output.h"

#include "stream_decoder.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// Fullscreen-triangle vertex shader: no vertex buffer, positions/UVs derived
// from SV_VertexID. UV runs 0..1 across the bound viewport, so letterbox/
// pillarbox falls out of the viewport rectangle we set per frame.
const char *kVertexShaderSource = R"(
struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput output;
    output.uv  = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

// YUV420P -> RGB on the GPU. This mirrors ConvertYuv420ToBgra exactly, just in
// normalized [0,1] space: y/c scale+offset and the BT.601/709/2020 inverse
// matrix coefficients arrive through the ColorParams constant buffer.
const char *kPixelShaderSource = R"(
Texture2D    texY : register(t0);
Texture2D    texU : register(t1);
Texture2D    texV : register(t2);
SamplerState samp : register(s0);

cbuffer ColorParams : register(b0)
{
    float kr;
    float kb;
    float y_scale;
    float y_offset;
    float c_scale;
    float c_offset;
    float y_texel_x;
    float y_texel_y;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float yn = texY.Sample(samp, input.uv).r;
    float un = texU.Sample(samp, input.uv).r;
    float vn = texV.Sample(samp, input.uv).r;

    float kg = 1.0 - kr - kb;
    float y = (yn - y_offset) * y_scale;
    float y_left = (texY.Sample(samp, input.uv + float2(-y_texel_x, 0.0)).r - y_offset) * y_scale;
    float y_right = (texY.Sample(samp, input.uv + float2(y_texel_x, 0.0)).r - y_offset) * y_scale;
    float y_top = (texY.Sample(samp, input.uv + float2(0.0, -y_texel_y)).r - y_offset) * y_scale;
    float y_bottom = (texY.Sample(samp, input.uv + float2(0.0, y_texel_y)).r - y_offset) * y_scale;
    float y_blur = (y_left + y_right + y_top + y_bottom) * 0.25;
    y = saturate(y + (y - y_blur) * 0.18);

    float u = (un - c_offset) * c_scale;
    float v = (vn - c_offset) * c_scale;

    float r = y + 2.0 * (1.0 - kr) * v;
    float b = y + 2.0 * (1.0 - kb) * u;
    float g = y - (2.0 * kb * (1.0 - kb) / kg) * u
                - (2.0 * kr * (1.0 - kr) / kg) * v;

    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";

bool CompileShader(const char *source, const char *entry, const char *target, ComPtr<ID3DBlob> &blob)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        blob.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(result))
    {
        if (errors)
        {
            std::cout << "Shader compile failed: "
                      << static_cast<const char *>(errors->GetBufferPointer()) << std::endl;
        }
        else
        {
            std::cout << "Shader compile failed: 0x" << std::hex << result << std::dec << std::endl;
        }
        return false;
    }
    return true;
}
}

bool VideoOutput::open(const std::string &title, int width, int height)
{
    if (window_)
    {
        return true;
    }

    instance_ = GetModuleHandleA(nullptr);
    quit_requested_ = false;
    if (!register_window_class() || !create_window(title, width, height))
    {
        close();
        return false;
    }

    return true;
}

void VideoOutput::close()
{
    for (auto &srv : plane_srvs_)
    {
        srv.Reset();
    }
    for (auto &texture : plane_textures_)
    {
        texture.Reset();
    }
    color_params_buffer_.Reset();
    sampler_state_.Reset();
    pixel_shader_.Reset();
    vertex_shader_.Reset();
    render_target_view_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();

    device_ready_ = false;
    swap_chain_width_ = 0;
    swap_chain_height_ = 0;
    texture_width_ = 0;
    texture_height_ = 0;
    texture_sample_bytes_ = 1;
    has_frame_ = false;

    if (window_)
    {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool VideoOutput::make_current()
{
    return true;
}

bool VideoOutput::clear_current()
{
    return true;
}

bool VideoOutput::poll_events()
{
    MSG message = {};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    return !quit_requested_;
}

bool VideoOutput::ensure_device()
{
    if (device_ready_)
    {
        return true;
    }

    UINT device_flags = 0;
#if defined(_DEBUG)
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        device_flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        nullptr,
        context_.GetAddressOf());
    if (FAILED(result))
    {
        std::cout << "Could not create D3D11 device: 0x" << std::hex << result << std::dec << std::endl;
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device)))
    {
        std::cout << "Could not query IDXGIDevice." << std::endl;
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())))
    {
        std::cout << "Could not get DXGI adapter." << std::endl;
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        std::cout << "Could not get IDXGIFactory2." << std::endl;
        return false;
    }

    RECT client_rect = {};
    GetClientRect(window_, &client_rect);
    int client_width = std::max<int>(1, client_rect.right - client_rect.left);
    int client_height = std::max<int>(1, client_rect.bottom - client_rect.top);

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.Width = static_cast<UINT>(client_width);
    swap_chain_desc.Height = static_cast<UINT>(client_height);
    swap_chain_desc.Format = kSwapChainFormat;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    result = factory->CreateSwapChainForHwnd(
        device_.Get(),
        window_,
        &swap_chain_desc,
        nullptr,
        nullptr,
        swap_chain_.GetAddressOf());
    if (FAILED(result))
    {
        std::cout << "Could not create swap chain: 0x" << std::hex << result << std::dec << std::endl;
        return false;
    }

    // We drive presentation through the window message loop ourselves.
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

    if (!compile_shaders())
    {
        return false;
    }

    swap_chain_width_ = client_width;
    swap_chain_height_ = client_height;
    if (!ensure_render_target(client_width, client_height))
    {
        return false;
    }

    device_ready_ = true;
    return true;
}

bool VideoOutput::compile_shaders()
{
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    if (!CompileShader(kVertexShaderSource, "main", "vs_4_0", vs_blob) ||
        !CompileShader(kPixelShaderSource, "main", "ps_4_0", ps_blob))
    {
        return false;
    }

    if (FAILED(device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, vertex_shader_.GetAddressOf())))
    {
        std::cout << "Could not create vertex shader." << std::endl;
        return false;
    }

    if (FAILED(device_->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, pixel_shader_.GetAddressOf())))
    {
        std::cout << "Could not create pixel shader." << std::endl;
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0.0f;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, sampler_state_.GetAddressOf())))
    {
        std::cout << "Could not create sampler state." << std::endl;
        return false;
    }

    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(ColorParams);
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&buffer_desc, nullptr, color_params_buffer_.GetAddressOf())))
    {
        std::cout << "Could not create color params constant buffer." << std::endl;
        return false;
    }

    return true;
}

bool VideoOutput::ensure_render_target(int width, int height)
{
    render_target_view_.Reset();

    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()))))
    {
        std::cout << "Could not get swap chain back buffer." << std::endl;
        return false;
    }

    if (FAILED(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target_view_.GetAddressOf())))
    {
        std::cout << "Could not create render target view." << std::endl;
        return false;
    }

    swap_chain_width_ = width;
    swap_chain_height_ = height;
    return true;
}

bool VideoOutput::ensure_plane_textures(int width, int height, int sample_bytes)
{
    sample_bytes = sample_bytes > 1 ? 2 : 1;
    if (plane_textures_[0] && texture_width_ == width && texture_height_ == height && texture_sample_bytes_ == sample_bytes)
    {
        return true;
    }

    for (auto &srv : plane_srvs_)
    {
        srv.Reset();
    }
    for (auto &texture : plane_textures_)
    {
        texture.Reset();
    }

    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const int plane_widths[3] = {width, chroma_width, chroma_width};
    const int plane_heights[3] = {height, chroma_height, chroma_height};

    for (int i = 0; i < 3; ++i)
    {
        D3D11_TEXTURE2D_DESC texture_desc = {};
        texture_desc.Width = static_cast<UINT>(plane_widths[i]);
        texture_desc.Height = static_cast<UINT>(plane_heights[i]);
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = sample_bytes > 1 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DYNAMIC;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device_->CreateTexture2D(&texture_desc, nullptr, plane_textures_[i].GetAddressOf())))
        {
            std::cout << "Could not create plane texture " << i << "." << std::endl;
            return false;
        }

        if (FAILED(device_->CreateShaderResourceView(
                plane_textures_[i].Get(), nullptr, plane_srvs_[i].GetAddressOf())))
        {
            std::cout << "Could not create plane SRV " << i << "." << std::endl;
            return false;
        }
    }

    texture_width_ = width;
    texture_height_ = height;
    texture_sample_bytes_ = sample_bytes;
    return true;
}

bool VideoOutput::upload_planes(const VideoFrameView &frame)
{
    const int chroma_width = (frame.width + 1) / 2;
    const int chroma_height = (frame.height + 1) / 2;
    const uint8_t *plane_data[3] = {frame.y_data, frame.u_data, frame.v_data};
    const int plane_line_sizes[3] = {frame.y_line_size, frame.u_line_size, frame.v_line_size};
    const int plane_widths[3] = {frame.width, chroma_width, chroma_width};
    const int plane_heights[3] = {frame.height, chroma_height, chroma_height};

    for (int i = 0; i < 3; ++i)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(plane_textures_[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::cout << "Could not map plane texture " << i << "." << std::endl;
            return false;
        }

        // Source line size carries swscale padding and the GPU row pitch carries
        // its own alignment, so copy exactly the visible width of each row.
        const size_t copy_bytes = static_cast<size_t>(plane_widths[i]) * static_cast<size_t>(std::max(1, frame.plane_sample_bytes));
        const uint8_t *src = plane_data[i];
        auto *dst = static_cast<uint8_t *>(mapped.pData);
        for (int row = 0; row < plane_heights[i]; ++row)
        {
            std::memcpy(
                dst + static_cast<size_t>(row) * mapped.RowPitch,
                src + static_cast<size_t>(row) * plane_line_sizes[i],
                copy_bytes);
        }

        context_->Unmap(plane_textures_[i].Get(), 0);
    }

    return true;
}

void VideoOutput::update_color_params(const VideoFrameView &frame)
{
    // Coefficient selection matches SelectYuvCoefficients in the old CPU path.
    AVColorSpace color_space = frame.color_space;
    if (color_space == AVCOL_SPC_UNSPECIFIED)
    {
        color_space = frame.color_primaries == AVCOL_PRI_BT2020
                          ? AVCOL_SPC_BT2020_NCL
                          : AVCOL_SPC_BT709;
    }

    ColorParams params;
    params.kr = 0.2126f;
    params.kb = 0.0722f;
    if (color_space == AVCOL_SPC_BT470BG || color_space == AVCOL_SPC_SMPTE170M)
    {
        params.kr = 0.299f;
        params.kb = 0.114f;
    }
    else if (color_space == AVCOL_SPC_BT2020_NCL || color_space == AVCOL_SPC_BT2020_CL)
    {
        params.kr = 0.2627f;
        params.kb = 0.0593f;
    }

    const bool limited_range = frame.color_range != AVCOL_RANGE_JPEG;
    const int component_bits = std::max(8, frame.component_bits);
    const float code_max = static_cast<float>((1ULL << component_bits) - 1ULL);
    const float code_scale = static_cast<float>(1ULL << (component_bits - 8));
    params.y_scale = limited_range ? code_max / (219.0f * code_scale) : 1.0f;
    params.y_offset = limited_range ? (16.0f * code_scale) / code_max : 0.0f;
    params.c_scale = limited_range ? code_max / (224.0f * code_scale) : 1.0f;
    params.c_offset = (128.0f * code_scale) / code_max;
    params.y_texel_x = frame.width > 0 ? 1.0f / static_cast<float>(frame.width) : 0.0f;
    params.y_texel_y = frame.height > 0 ? 1.0f / static_cast<float>(frame.height) : 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(color_params_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &params, sizeof(params));
        context_->Unmap(color_params_buffer_.Get(), 0);
    }
}

bool VideoOutput::render_frame(const VideoFrameView &frame)
{
    if (!is_open() || !frame.y_data || !frame.u_data || !frame.v_data || frame.width <= 0 || frame.height <= 0)
    {
        return false;
    }

    if (!ensure_device())
    {
        return false;
    }

    RECT client_rect = {};
    GetClientRect(window_, &client_rect);
    const int window_width = client_rect.right - client_rect.left;
    const int window_height = client_rect.bottom - client_rect.top;
    if (window_width <= 0 || window_height <= 0)
    {
        return true;
    }

    if (window_width != swap_chain_width_ || window_height != swap_chain_height_)
    {
        render_target_view_.Reset();
        const HRESULT result = swap_chain_->ResizeBuffers(
            0,
            static_cast<UINT>(window_width),
            static_cast<UINT>(window_height),
            DXGI_FORMAT_UNKNOWN,
            0);
        if (FAILED(result))
        {
            std::cout << "Could not resize swap chain buffers: 0x" << std::hex << result << std::dec << std::endl;
            return false;
        }
        if (!ensure_render_target(window_width, window_height))
        {
            return false;
        }
    }

    if (!ensure_plane_textures(frame.width, frame.height, frame.plane_sample_bytes))
    {
        return false;
    }

    if (!upload_planes(frame))
    {
        return false;
    }

    update_color_params(frame);

    // DAR letterbox/pillarbox, identical to the previous StretchDIBits math.
    const double display_aspect_ratio = frame.display_aspect_ratio > 0.0
                                            ? frame.display_aspect_ratio
                                            : static_cast<double>(frame.width) / static_cast<double>(frame.height);
    const double window_aspect_ratio = static_cast<double>(window_width) / static_cast<double>(window_height);

    float viewport_x = 0.0f;
    float viewport_y = 0.0f;
    float viewport_width = static_cast<float>(window_width);
    float viewport_height = static_cast<float>(window_height);
    if (window_aspect_ratio > display_aspect_ratio)
    {
        viewport_width = static_cast<float>(window_height * display_aspect_ratio);
        viewport_x = (static_cast<float>(window_width) - viewport_width) / 2.0f;
    }
    else if (window_aspect_ratio < display_aspect_ratio)
    {
        viewport_height = static_cast<float>(window_width / display_aspect_ratio);
        viewport_y = (static_cast<float>(window_height) - viewport_height) / 2.0f;
    }

    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(render_target_view_.Get(), clear_color);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = viewport_x;
    viewport.TopLeftY = viewport_y;
    viewport.Width = viewport_width;
    viewport.Height = viewport_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView *rtv = render_target_view_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);

    ID3D11ShaderResourceView *srvs[3] = {
        plane_srvs_[0].Get(), plane_srvs_[1].Get(), plane_srvs_[2].Get()};
    context_->PSSetShaderResources(0, 3, srvs);
    ID3D11SamplerState *sampler = sampler_state_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer *cbuffer = color_params_buffer_.Get();
    context_->PSSetConstantBuffers(0, 1, &cbuffer);

    context_->Draw(3, 0);

    texture_width_ = frame.width;
    texture_height_ = frame.height;
    display_aspect_ratio_ = display_aspect_ratio;
    has_frame_ = true;
    return true;
}

void VideoOutput::present()
{
    if (swap_chain_ && has_frame_)
    {
        // Sync interval 1: tear-free, paced against the display. Frame timing is
        // still owned by the render thread's PTS clock upstream.
        swap_chain_->Present(1, 0);
    }
}

bool VideoOutput::toggle_fullscreen()
{
    if (!window_)
    {
        return false;
    }

    if (!fullscreen_)
    {
        windowed_style_ = static_cast<DWORD>(GetWindowLongPtrA(window_, GWL_STYLE));
        windowed_placement_.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(window_, &windowed_placement_);

        MONITORINFO monitor_info = {};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoA(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor_info))
        {
            return false;
        }

        SetWindowLongPtrA(window_, GWL_STYLE, windowed_style_ & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(
            window_,
            HWND_TOP,
            monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.top,
            monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
        return true;
    }

    SetWindowLongPtrA(window_, GWL_STYLE, windowed_style_);
    SetWindowPlacement(window_, &windowed_placement_);
    SetWindowPos(
        window_,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    fullscreen_ = false;
    return true;
}

bool VideoOutput::consume_key_press(int virtual_key)
{
    std::lock_guard<std::mutex> lock(input_mutex_);
    auto iterator = std::find(key_presses_.begin(), key_presses_.end(), virtual_key);
    if (iterator == key_presses_.end())
    {
        return false;
    }

    key_presses_.erase(iterator);
    return true;
}

bool VideoOutput::is_open() const
{
    return window_ != nullptr && !quit_requested_;
}

LRESULT CALLBACK VideoOutput::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTA *>(lparam);
        auto *video_output = static_cast<VideoOutput *>(create->lpCreateParams);
        SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(video_output));
    }

    auto *video_output = reinterpret_cast<VideoOutput *>(GetWindowLongPtrA(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (video_output && (lparam & (1LL << 30)) == 0)
        {
            std::lock_guard<std::mutex> lock(video_output->input_mutex_);
            video_output->key_presses_.push_back(static_cast<int>(wparam));
        }
        break;

    case WM_CLOSE:
        if (video_output)
        {
            video_output->quit_requested_ = true;
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (video_output)
        {
            video_output->quit_requested_ = true;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(window, message, wparam, lparam);
}

bool VideoOutput::register_window_class()
{
    constexpr const char *kWindowClassName = "NanamiPlayerWindow";
    WNDCLASSEXA window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = 0;
    window_class.lpfnWndProc = &VideoOutput::WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExA(&window_class) == 0)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::cout << "Could not register Win32 window class: " << error << std::endl;
            return false;
        }
    }

    return true;
}

bool VideoOutput::create_window(const std::string &title, int width, int height)
{
    constexpr const char *kWindowClassName = "NanamiPlayerWindow";
    const DWORD window_style = WS_OVERLAPPEDWINDOW;

    RECT window_rect = {0, 0, width, height};
    AdjustWindowRect(&window_rect, window_style, FALSE);

    window_ = CreateWindowExA(
        0,
        kWindowClassName,
        title.c_str(),
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!window_)
    {
        std::cout << "Could not create Win32 window: " << GetLastError() << std::endl;
        return false;
    }

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    SetForegroundWindow(window_);
    SetActiveWindow(window_);
    SetFocus(window_);
    return true;
}
