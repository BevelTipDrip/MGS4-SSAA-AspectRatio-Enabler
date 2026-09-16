#include "pch.hpp"
#include "state_cache.hpp"

#include "log.hpp"

namespace
{
    // Keys are built field by field, never from the raw struct bytes: the game's descriptions sit
    // on its own stack and their padding is whatever happened to be there before, so hashing the
    // bytes would treat identical requests as different ones. The device pointer is part of the
    // key so objects can never be handed to a device that did not make them.
    std::mutex g_Mutex;
    std::map<std::string, IUnknown*> g_Objects;
    std::atomic<uint64_t> g_Hits { 0 };

    void KeyPart(std::string& key, uint32_t v) { key.append(reinterpret_cast<const char*>(&v), sizeof(v)); }
    void KeyPart(std::string& key, float v) { key.append(reinterpret_cast<const char*>(&v), sizeof(v)); }

    std::string Begin(char kind, const void* device)
    {
        std::string key(1, kind);
        key.append(reinterpret_cast<const char*>(&device), sizeof(device));
        return key;
    }

    std::string CacheKey(const void* device, const D3D11_SAMPLER_DESC& d)
    {
        std::string key = Begin('S', device);
        KeyPart(key, static_cast<uint32_t>(d.Filter));
        KeyPart(key, static_cast<uint32_t>(d.AddressU));
        KeyPart(key, static_cast<uint32_t>(d.AddressV));
        KeyPart(key, static_cast<uint32_t>(d.AddressW));
        KeyPart(key, d.MipLODBias);
        KeyPart(key, static_cast<uint32_t>(d.MaxAnisotropy));
        KeyPart(key, static_cast<uint32_t>(d.ComparisonFunc));
        for (float c : d.BorderColor) { KeyPart(key, c); }
        KeyPart(key, d.MinLOD);
        KeyPart(key, d.MaxLOD);
        return key;
    }

    void KeyStencilOp(std::string& key, const D3D11_DEPTH_STENCILOP_DESC& o)
    {
        KeyPart(key, static_cast<uint32_t>(o.StencilFailOp));
        KeyPart(key, static_cast<uint32_t>(o.StencilDepthFailOp));
        KeyPart(key, static_cast<uint32_t>(o.StencilPassOp));
        KeyPart(key, static_cast<uint32_t>(o.StencilFunc));
    }

    std::string CacheKey(const void* device, const D3D11_DEPTH_STENCIL_DESC& d)
    {
        std::string key = Begin('D', device);
        KeyPart(key, static_cast<uint32_t>(d.DepthEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.DepthWriteMask));
        KeyPart(key, static_cast<uint32_t>(d.DepthFunc));
        KeyPart(key, static_cast<uint32_t>(d.StencilEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.StencilReadMask));
        KeyPart(key, static_cast<uint32_t>(d.StencilWriteMask));
        KeyStencilOp(key, d.FrontFace);
        KeyStencilOp(key, d.BackFace);
        return key;
    }

    std::string CacheKey(const void* device, const D3D11_BLEND_DESC& d)
    {
        std::string key = Begin('B', device);
        KeyPart(key, static_cast<uint32_t>(d.AlphaToCoverageEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.IndependentBlendEnable ? 1 : 0));
        for (const D3D11_RENDER_TARGET_BLEND_DESC& t : d.RenderTarget)
        {
            KeyPart(key, static_cast<uint32_t>(t.BlendEnable ? 1 : 0));
            KeyPart(key, static_cast<uint32_t>(t.SrcBlend));
            KeyPart(key, static_cast<uint32_t>(t.DestBlend));
            KeyPart(key, static_cast<uint32_t>(t.BlendOp));
            KeyPart(key, static_cast<uint32_t>(t.SrcBlendAlpha));
            KeyPart(key, static_cast<uint32_t>(t.DestBlendAlpha));
            KeyPart(key, static_cast<uint32_t>(t.BlendOpAlpha));
            KeyPart(key, static_cast<uint32_t>(t.RenderTargetWriteMask));
        }
        return key;
    }

    std::string CacheKey(const void* device, const D3D11_RASTERIZER_DESC& d)
    {
        std::string key = Begin('R', device);
        KeyPart(key, static_cast<uint32_t>(d.FillMode));
        KeyPart(key, static_cast<uint32_t>(d.CullMode));
        KeyPart(key, static_cast<uint32_t>(d.FrontCounterClockwise ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.DepthBias));
        KeyPart(key, d.DepthBiasClamp);
        KeyPart(key, d.SlopeScaledDepthBias);
        KeyPart(key, static_cast<uint32_t>(d.DepthClipEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.ScissorEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.MultisampleEnable ? 1 : 0));
        KeyPart(key, static_cast<uint32_t>(d.AntialiasedLineEnable ? 1 : 0));
        return key;
    }

    // The cache keeps its own reference for the life of the process, so the object cannot die
    // between the lookup and the AddRef; the lock is released before calling into the runtime.
    bool ServeKey(const std::string& key, IUnknown** out)
    {
        IUnknown* found = nullptr;
        {
            std::lock_guard lock(g_Mutex);
            const auto it = g_Objects.find(key);
            if (it == g_Objects.end()) { return false; }
            found = it->second;
        }
        found->AddRef();
        *out = found;
        g_Hits.fetch_add(1);
        return true;
    }

    void KeepKey(const std::string& key, IUnknown* made)
    {
        bool inserted = false;
        size_t held = 0;
        {
            std::lock_guard lock(g_Mutex);
            inserted = g_Objects.emplace(key, made).second;
            held = g_Objects.size();
        }
        if (!inserted) { return; }
        made->AddRef();
        if (held <= 48) { spdlog::info("PW render: state cache holds {} object(s).", held); }
    }
}

namespace StateCache
{
    bool Serve(const void* device, const D3D11_SAMPLER_DESC& d, IUnknown** out) { return ServeKey(CacheKey(device, d), out); }
    bool Serve(const void* device, const D3D11_DEPTH_STENCIL_DESC& d, IUnknown** out) { return ServeKey(CacheKey(device, d), out); }
    bool Serve(const void* device, const D3D11_BLEND_DESC& d, IUnknown** out) { return ServeKey(CacheKey(device, d), out); }
    bool Serve(const void* device, const D3D11_RASTERIZER_DESC& d, IUnknown** out) { return ServeKey(CacheKey(device, d), out); }

    void Keep(const void* device, const D3D11_SAMPLER_DESC& d, IUnknown* made) { KeepKey(CacheKey(device, d), made); }
    void Keep(const void* device, const D3D11_DEPTH_STENCIL_DESC& d, IUnknown* made) { KeepKey(CacheKey(device, d), made); }
    void Keep(const void* device, const D3D11_BLEND_DESC& d, IUnknown* made) { KeepKey(CacheKey(device, d), made); }
    void Keep(const void* device, const D3D11_RASTERIZER_DESC& d, IUnknown* made) { KeepKey(CacheKey(device, d), made); }

    uint64_t Hits() { return g_Hits.load(); }
    size_t Held() { std::lock_guard lock(g_Mutex); return g_Objects.size(); }
}
