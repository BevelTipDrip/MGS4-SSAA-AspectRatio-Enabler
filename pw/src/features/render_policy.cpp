#include "pch.hpp"
#include "render_policy.hpp"

#include "internal_size.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>

namespace
{
    std::atomic<uint32_t> g_Stripped { 0 }, g_Anisotropic { 0 }, g_Refresh { 0 };
}

namespace RenderPolicy
{
    bool TextureDesc(D3D11_TEXTURE2D_DESC& desc, bool& stripped)
    {
        stripped = false;
        bool changed = false;
        if (InternalSize::bDisableMsaa && desc.SampleDesc.Count > 1)
        {
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            changed = true;
        }
        if (InternalSize::bGpuLocalTextures && desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags && desc.Width >= 1024)
        {
            desc.CPUAccessFlags = 0;
            stripped = true;
            changed = true;
            if (g_Stripped.fetch_add(1) == 0 && mgs4e::log::Verbose()) { spdlog::info("PW render: large frame textures kept in video memory from here on."); }
        }
        return changed;
    }

    bool SamplerDesc(D3D11_SAMPLER_DESC& desc)
    {
        if (InternalSize::iAnisotropy < 2) { return false; }
        const bool comparison = desc.Filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        const bool linear = desc.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR || desc.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT || desc.Filter == D3D11_FILTER_ANISOTROPIC;
        if (comparison || !linear) { return false; }
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = static_cast<UINT>(std::min(16, InternalSize::iAnisotropy));
        if (g_Anisotropic.fetch_add(1) == 0 && mgs4e::log::Verbose()) { spdlog::info("PW render: linear samplers created anisotropic {}x from here on.", desc.MaxAnisotropy); }
        return true;
    }

    void SwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc)
    {
        // Exclusive fullscreen: the game asks for 60/1 whatever the display runs at, and on a
        // 120 Hz desktop the mode it lands on drops one frame a second (measured 2026-09-13).
        // The display's current refresh rate is requested instead.
        if (desc.Windowed) { return; }
        DEVMODEW dm {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        {
            if (g_Refresh.fetch_add(1) == 0) { spdlog::info("PW render: exclusive fullscreen at the display's {} Hz (the game asked for {}/{}).", dm.dmDisplayFrequency, desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator); }
            desc.BufferDesc.RefreshRate.Numerator = dm.dmDisplayFrequency;
            desc.BufferDesc.RefreshRate.Denominator = 1;
        }
    }

    void AfterSwapChain(HWND hwnd, const DXGI_SWAP_CHAIN_DESC& desc)
    {
        // Windowed (the game's mode 1): the game makes a normal window at its saved or preset
        // size; the client is set to the selected screen resolution and centred on the
        // monitor. Borderless (0) and Fullscreen (2) are the game's own and need nothing here.
        if (!desc.Windowed || !hwnd || !IsWindow(hwnd)) { return; }
        if (InternalSize::iWindowMode != 1 || InternalSize::iOutputWidth <= 0 || InternalSize::iOutputHeight <= 0) { return; }
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) { return; }
        const RECT m = mi.rcMonitor;
        const LONG style = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        RECT r { 0, 0, InternalSize::iOutputWidth, InternalSize::iOutputHeight };
        AdjustWindowRectEx(&r, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        const int w = r.right - r.left, h = r.bottom - r.top;
        const int x = m.left + std::max(0L, ((m.right - m.left) - w) / 2), y = m.top + std::max(0L, ((m.bottom - m.top) - h) / 2);
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE);
        spdlog::info("PW render: windowed: client {}x{} at {},{}.", InternalSize::iOutputWidth, InternalSize::iOutputHeight, x, y);
    }

    bool ResolveAsCopy(ID3D11Resource* src)
    {
        if (!InternalSize::bDisableMsaa || !src) { return false; }
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) { return false; }
        D3D11_TEXTURE2D_DESC d {};
        tex->GetDesc(&d);
        tex->Release();
        return d.SampleDesc.Count == 1;
    }
}
