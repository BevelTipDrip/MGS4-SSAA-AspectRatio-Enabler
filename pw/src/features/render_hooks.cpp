#include "pch.hpp"
#include "render_hooks.hpp"

#include "canvas.hpp"
#include "internal_size.hpp"
#include "log.hpp"
#include "render_policy.hpp"
#include "state_cache.hpp"

#include <mutex>
#include <unordered_map>

namespace
{
    // ID3D11Device / ID3D11DeviceContext / IDXGIFactory vtable slots (d3d11.h, dxgi.h order).
    constexpr size_t kDevCreateTexture2D = 5;
    constexpr size_t kDevCreateBlendState = 20;
    constexpr size_t kDevCreateDepthStencilState = 21;
    constexpr size_t kDevCreateRasterizerState = 22;
    constexpr size_t kDevCreateSamplerState = 23;
    constexpr size_t kDevCreateDeferredContext = 27;
    constexpr size_t kCtxMap = 14;
    constexpr size_t kCtxUnmap = 15;
    constexpr size_t kCtxCopyResource = 47;
    constexpr size_t kCtxResolveSubresource = 57;
    constexpr size_t kCtxSlotCount = 80;
    constexpr size_t kFactoryCreateSwapChain = 10;

    SafetyHookInline D3D11CreateDevice_hook {}, D3D11CreateDeviceAndSwapChain_hook {};
    SafetyHookInline CreateDXGIFactory_hook {}, CreateDXGIFactory1_hook {}, CreateDXGIFactory2_hook {};
    SafetyHookInline Device_CreateTexture2D_hook {}, Device_CreateSamplerState_hook {}, Device_CreateDeferredContext_hook {};
    // Hooked only to serve the state cache: these three carry no render policy of their own.
    SafetyHookInline Device_CreateBlendState_hook {}, Device_CreateDepthStencilState_hook {}, Device_CreateRasterizerState_hook {};
    SafetyHookInline Factory_CreateSwapChain_hook {};

    // Context hooks are vtable entry replacements (an inline hook on a deferred context's Map
    // crashed the driver); the immediate and the deferred class each have their own table.
    struct ContextHooks
    {
        void** vtable = nullptr;
        void* original[kCtxSlotCount] {};
    };
    ContextHooks g_Immediate, g_Deferred;

    ContextHooks& HooksFor(void* self)
    {
        void** vt = *reinterpret_cast<void***>(self);
        return vt == g_Deferred.vtable ? g_Deferred : g_Immediate;
    }
    template <class Fn>
    Fn Original(void* self, size_t slot) { return reinterpret_cast<Fn>(HooksFor(self).original[slot]); }

    // The pointer of every writable buffer map in flight, for the private module at Unmap.
    std::mutex g_Mutex;
    std::unordered_map<ID3D11Resource*, void*> g_Mapped;

    size_t BufferSize(ID3D11Resource* res, bool& constant)
    {
        constant = false;
        ID3D11Buffer* buf = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf))) || !buf) { return 0; }
        D3D11_BUFFER_DESC d {};
        buf->GetDesc(&d);
        buf->Release();
        constant = (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
        return d.ByteWidth;
    }

    HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
    {
        const HRESULT r = Original<decltype(&Hooked_Map)>(self, kCtxMap)(self, res, sub, type, flags, mapped);
        if (Canvas::Active() && SUCCEEDED(r) && res && mapped && mapped->pData && sub == 0
            && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_WRITE))
        {
            std::lock_guard lock(g_Mutex);
            g_Mapped[res] = mapped->pData;
        }
        return r;
    }

    void STDMETHODCALLTYPE Hooked_Unmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub)
    {
        if (Canvas::Active() && res)
        {
            void* data = nullptr;
            {
                std::lock_guard lock(g_Mutex);
                const auto it = g_Mapped.find(res);
                if (it != g_Mapped.end()) { data = it->second; g_Mapped.erase(it); }
            }
            if (data)
            {
                bool constant = false;
                const size_t size = BufferSize(res, constant);
                if (constant) { Canvas::OnConstantUnmap(data, size); }
                else if (size) { Canvas::OnVertexUnmap(data, size); }
            }
        }
        Original<decltype(&Hooked_Unmap)>(self, kCtxUnmap)(self, res, sub);
    }

    void STDMETHODCALLTYPE Hooked_ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT fmt)
    {
        if (dst && RenderPolicy::ResolveAsCopy(src))
        {
            using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
            Original<CopyFn>(self, kCtxCopyResource)(self, dst, src);
            return;
        }
        Original<decltype(&Hooked_ResolveSubresource)>(self, kCtxResolveSubresource)(self, dst, dstSub, src, srcSub, fmt);
    }

    void InstallContextHooks(ContextHooks& h, void** vtable, const char* name)
    {
        struct Slot { size_t index; void* target; };
        const Slot slots[] = {
            { kCtxMap, reinterpret_cast<void*>(Hooked_Map) },
            { kCtxUnmap, reinterpret_cast<void*>(Hooked_Unmap) },
            { kCtxResolveSubresource, reinterpret_cast<void*>(Hooked_ResolveSubresource) },
        };
        // Under a wrapping layer (RenderDoc) the immediate and deferred contexts are one class with one
        // vtable. Patching it twice would record our own hooks as the "original" and recurse until the
        // stack is gone, so the second set just aliases the first.
        if (&h != &g_Immediate && g_Immediate.vtable == vtable)
        {
            h = g_Immediate;
            spdlog::info("PW render: the {} context shares the immediate context's vtable; hooks shared.", name);
            return;
        }
        h.vtable = vtable;
        DWORD old = 0;
        if (!VirtualProtect(vtable, kCtxSlotCount * sizeof(void*), PAGE_READWRITE, &old))
        {
            spdlog::error("PW render: the {} context could not be hooked (error {}); MSAA and the canvas uploads are off on it.", name, GetLastError());
            h.vtable = nullptr;
            return;
        }
        h.original[kCtxCopyResource] = vtable[kCtxCopyResource];
        for (const Slot& sl : slots)
        {
            h.original[sl.index] = vtable[sl.index];
            vtable[sl.index] = sl.target;
        }
        VirtualProtect(vtable, kCtxSlotCount * sizeof(void*), old, &old);
        if (mgs4e::log::Verbose()) { spdlog::info("PW render: {} context hooked.", name); }
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
    {
        D3D11_TEXTURE2D_DESC changed {};
        if (desc)
        {
            changed = *desc;
            bool stripped = false;
            if (RenderPolicy::TextureDesc(changed, stripped)) { desc = &changed; }
        }
        return Device_CreateTexture2D_hook.stdcall<HRESULT>(self, desc, init, out);
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSamplerState(ID3D11Device* self, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
    {
        // The cache is keyed on what the GAME asked for, kept before the render policy rewrites it,
        // so the next identical request is recognised and still gets our anisotropy override.
        const bool cache = StateCache::Wants(StateCache::kSamplers) && desc && out;
        D3D11_SAMPLER_DESC asked {};
        if (cache)
        {
            asked = *desc;
            if (StateCache::Serve(self, asked, reinterpret_cast<IUnknown**>(out))) { return S_OK; }
        }
        D3D11_SAMPLER_DESC changed {};
        if (desc)
        {
            changed = *desc;
            if (RenderPolicy::SamplerDesc(changed)) { desc = &changed; }
        }
        const HRESULT r = Device_CreateSamplerState_hook.stdcall<HRESULT>(self, desc, out);
        if (cache && SUCCEEDED(r) && *out) { StateCache::Keep(self, asked, *out); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateBlendState(ID3D11Device* self, const D3D11_BLEND_DESC* desc, ID3D11BlendState** out)
    {
        const bool cache = StateCache::Wants(StateCache::kObjects) && desc && out;
        if (cache && StateCache::Serve(self, *desc, reinterpret_cast<IUnknown**>(out))) { return S_OK; }
        const HRESULT r = Device_CreateBlendState_hook.stdcall<HRESULT>(self, desc, out);
        if (cache && SUCCEEDED(r) && *out) { StateCache::Keep(self, *desc, *out); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateDepthStencilState(ID3D11Device* self, const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** out)
    {
        const bool cache = StateCache::Wants(StateCache::kObjects) && desc && out;
        if (cache && StateCache::Serve(self, *desc, reinterpret_cast<IUnknown**>(out))) { return S_OK; }
        const HRESULT r = Device_CreateDepthStencilState_hook.stdcall<HRESULT>(self, desc, out);
        if (cache && SUCCEEDED(r) && *out) { StateCache::Keep(self, *desc, *out); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateRasterizerState(ID3D11Device* self, const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** out)
    {
        const bool cache = StateCache::Wants(StateCache::kObjects) && desc && out;
        if (cache && StateCache::Serve(self, *desc, reinterpret_cast<IUnknown**>(out))) { return S_OK; }
        const HRESULT r = Device_CreateRasterizerState_hook.stdcall<HRESULT>(self, desc, out);
        if (cache && SUCCEEDED(r) && *out) { StateCache::Keep(self, *desc, *out); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateDeferredContext(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out)
    {
        const HRESULT r = Device_CreateDeferredContext_hook.stdcall<HRESULT>(self, flags, out);
        if (SUCCEEDED(r) && out && *out && !g_Deferred.vtable) { InstallContextHooks(g_Deferred, *reinterpret_cast<void***>(*out), "deferred"); }
        return r;
    }

    void HookDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        static bool hooked = false;
        if (hooked || !device) { return; }
        hooked = true;
        void** dv = *reinterpret_cast<void***>(device);
        Device_CreateTexture2D_hook = safetyhook::create_inline(dv[kDevCreateTexture2D], reinterpret_cast<void*>(Hooked_CreateTexture2D));
        Device_CreateSamplerState_hook = safetyhook::create_inline(dv[kDevCreateSamplerState], reinterpret_cast<void*>(Hooked_CreateSamplerState));
        Device_CreateDeferredContext_hook = safetyhook::create_inline(dv[kDevCreateDeferredContext], reinterpret_cast<void*>(Hooked_CreateDeferredContext));
        if (StateCache::Wants(StateCache::kObjects))
        {
            Device_CreateBlendState_hook = safetyhook::create_inline(dv[kDevCreateBlendState], reinterpret_cast<void*>(Hooked_CreateBlendState));
            Device_CreateDepthStencilState_hook = safetyhook::create_inline(dv[kDevCreateDepthStencilState], reinterpret_cast<void*>(Hooked_CreateDepthStencilState));
            Device_CreateRasterizerState_hook = safetyhook::create_inline(dv[kDevCreateRasterizerState], reinterpret_cast<void*>(Hooked_CreateRasterizerState));
            spdlog::info("PW render: state cache on (blend {}, depth-stencil {}, rasterizer {}).",
                Device_CreateBlendState_hook ? "ok" : "FAILED", Device_CreateDepthStencilState_hook ? "ok" : "FAILED", Device_CreateRasterizerState_hook ? "ok" : "FAILED");
        }
        ID3D11DeviceContext* immediate = context;
        if (!immediate) { device->GetImmediateContext(&immediate); }
        if (immediate)
        {
            InstallContextHooks(g_Immediate, *reinterpret_cast<void***>(immediate), "immediate");
            if (!context) { immediate->Release(); }
        }
        if (mgs4e::log::Verbose())
        {
            spdlog::info("PW render: device hooked (textures {}, samplers {}, deferred contexts {}).",
                Device_CreateTexture2D_hook ? "ok" : "FAILED", Device_CreateSamplerState_hook ? "ok" : "FAILED", Device_CreateDeferredContext_hook ? "ok" : "FAILED");
        }
    }

    HRESULT WINAPI Hooked_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
        ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
    {
        const HRESULT r = D3D11CreateDevice_hook.stdcall<HRESULT>(adapter, driverType, software, flags, levels, levelCount, sdk, device, level, context);
        if (SUCCEEDED(r) && device && *device) { HookDevice(*device, context ? *context : nullptr); }
        return r;
    }

    HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc,
        IDXGISwapChain** chain, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
    {
        DXGI_SWAP_CHAIN_DESC changed {};
        if (desc) { changed = *desc; RenderPolicy::SwapChainDesc(changed); desc = &changed; }
        const HRESULT r = D3D11CreateDeviceAndSwapChain_hook.stdcall<HRESULT>(adapter, driverType, software, flags, levels, levelCount, sdk, desc, chain, device, level, context);
        if (SUCCEEDED(r) && device && *device) { HookDevice(*device, context ? *context : nullptr); }
        if (SUCCEEDED(r) && desc) { RenderPolicy::AfterSwapChain(desc->OutputWindow, *desc); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
    {
        if (desc) { RenderPolicy::SwapChainDesc(*desc); }
        const HRESULT r = Factory_CreateSwapChain_hook.stdcall<HRESULT>(self, device, desc, chain);
        if (SUCCEEDED(r) && desc) { RenderPolicy::AfterSwapChain(desc->OutputWindow, *desc); }
        return r;
    }

    void HookFactory(IUnknown* factory)
    {
        static bool hooked = false;
        if (hooked || !factory) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(factory);
        Factory_CreateSwapChain_hook = safetyhook::create_inline(vtable[kFactoryCreateSwapChain], reinterpret_cast<void*>(Hooked_CreateSwapChain));
        if (mgs4e::log::Verbose()) { spdlog::info("PW render: DXGI factory hooked ({}).", Factory_CreateSwapChain_hook ? "ok" : "FAILED"); }
    }

    HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void** out)
    {
        const HRESULT r = CreateDXGIFactory_hook.stdcall<HRESULT>(riid, out);
        if (SUCCEEDED(r) && out && *out) { HookFactory(static_cast<IUnknown*>(*out)); }
        return r;
    }
    HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void** out)
    {
        const HRESULT r = CreateDXGIFactory1_hook.stdcall<HRESULT>(riid, out);
        if (SUCCEEDED(r) && out && *out) { HookFactory(static_cast<IUnknown*>(*out)); }
        return r;
    }
    HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
    {
        const HRESULT r = CreateDXGIFactory2_hook.stdcall<HRESULT>(flags, riid, out);
        if (SUCCEEDED(r) && out && *out) { HookFactory(static_cast<IUnknown*>(*out)); }
        return r;
    }
}

namespace RenderHooks
{
    void Install()
    {
        if (const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"))) { D3D11CreateDevice_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDevice)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))) { D3D11CreateDeviceAndSwapChain_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDeviceAndSwapChain)); }
        }
        if (const HMODULE dxgi = LoadLibraryW(L"dxgi.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory"))) { CreateDXGIFactory_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) { CreateDXGIFactory1_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory1)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2"))) { CreateDXGIFactory2_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory2)); }
        }
        if (!D3D11CreateDevice_hook || !CreateDXGIFactory1_hook)
        {
            spdlog::warn("PW render: Direct3D hooks incomplete (device {}, factory {}/{}/{}); the rendering settings may not all apply.",
                D3D11CreateDevice_hook ? "ok" : "FAILED", CreateDXGIFactory_hook ? "ok" : "-", CreateDXGIFactory1_hook ? "ok" : "-", CreateDXGIFactory2_hook ? "ok" : "-");
        }
    }
}
