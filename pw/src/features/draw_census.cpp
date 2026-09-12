#include "pch.hpp"
#include "draw_census.hpp"

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"

#if MGS4E_LAB_BUILD

// Peace Walker records its frames on deferred contexts (three command lists a frame at the
// title, measured 2026-09-12) and replays them with ExecuteCommandList on the immediate
// context, so the immediate context sees no draws of its own once the game is up. Every hook
// here therefore keys its state by the context it was called on, and the same hook set is
// installed on the immediate context's vtable and on the deferred contexts' (a different
// class, hooked when the first one is created).

namespace
{
    // ---- entry-point hooks ----------------------------------------------------------------------
    SafetyHookInline D3D11CreateDevice_hook {};
    SafetyHookInline D3D11CreateDeviceAndSwapChain_hook {};
    SafetyHookInline CreateDXGIFactory_hook {};
    SafetyHookInline CreateDXGIFactory1_hook {};
    SafetyHookInline CreateDXGIFactory2_hook {};
    SafetyHookInline Factory_CreateSwapChain_hook {};
    SafetyHookInline Factory2_CreateSwapChainForHwnd_hook {};
    SafetyHookInline SwapChain_Present_hook {};
    SafetyHookInline Device_CreateVertexShader_hook {};
    SafetyHookInline Device_CreatePixelShader_hook {};
    SafetyHookInline Device_CreateDeferredContext_hook {};

    // ID3D11Device vtable slots.
    constexpr size_t kDevCreateVertexShader = 12;
    constexpr size_t kDevCreatePixelShader = 15;
    constexpr size_t kDevCreateDeferredContext = 27;
    // ID3D11DeviceContext vtable slots.
    constexpr size_t kCtxVSSetConstantBuffers = 7;
    constexpr size_t kCtxPSSetShader = 9;
    constexpr size_t kCtxVSSetShader = 11;
    constexpr size_t kCtxDrawIndexed = 12;
    constexpr size_t kCtxDraw = 13;
    constexpr size_t kCtxMap = 14;
    constexpr size_t kCtxUnmap = 15;
    constexpr size_t kCtxDrawIndexedInstanced = 20;
    constexpr size_t kCtxDrawInstanced = 21;
    constexpr size_t kCtxIASetPrimitiveTopology = 24;
    constexpr size_t kCtxOMSetRenderTargets = 33;
    constexpr size_t kCtxRSSetViewports = 44;
    constexpr size_t kCtxRSSetScissorRects = 45;
    constexpr size_t kCtxUpdateSubresource = 48;
    constexpr size_t kCtxExecuteCommandList = 58;
    // IDXGIFactory / IDXGIFactory2 / IDXGISwapChain slots.
    constexpr size_t kFactoryCreateSwapChain = 10;
    constexpr size_t kFactory2CreateSwapChainForHwnd = 15;
    constexpr size_t kSwapChainPresent = 8;

    // One hook set per context class (immediate, deferred). The context methods are hooked by
    // replacing the entries of the class vtable in d3d11.dll (every context of that class goes
    // through the same table), not by inline trampolines: an inline hook on the deferred
    // context's Map corrupted the driver's mapping bookkeeping (crash in nvwgf2umx.dll under
    // CResource::Map from the game's UI vertex upload, 2026-09-12), which a vtable swap cannot
    // do since the original function runs untouched. A hooked method finds its set by the
    // vtable of the context it was called on and calls the original entry from there.
    constexpr size_t kSlotCount = 64;
    struct ContextHooks
    {
        void** vtable = nullptr;
        const char* name = "";
        void* original[kSlotCount] {};
    };
    ContextHooks g_Immediate;
    ContextHooks g_Deferred;

    ContextHooks& HooksFor(void* self)
    {
        void** vt = *reinterpret_cast<void***>(self);
        return vt == g_Deferred.vtable ? g_Deferred : g_Immediate;
    }

    template <class Fn>
    Fn Original(void* self, size_t slot) { return reinterpret_cast<Fn>(HooksFor(self).original[slot]); }

    // ---- per-context state ------------------------------------------------------------------------
    struct ContextState
    {
        D3D11_VIEWPORT viewport {};
        D3D11_RECT scissor {};
        std::string target = "none";
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        void* vs = nullptr;
        void* ps = nullptr;
        ID3D11Buffer* vsCb0 = nullptr;
        int draws = 0;   // this frame (reset at Present for the immediate; at ExecuteCommandList for a deferred)
    };
    std::mutex g_Mutex;
    std::unordered_map<void*, ContextState> g_State;
    std::atomic<int> g_FramesToLog { 0 };
    std::atomic<uint64_t> g_Frame { 0 };
    std::atomic<uint64_t> g_DrawsSeen { 0 };
    std::atomic<uint64_t> g_ListsSeen { 0 };
    int g_DrawsThisFrame = 0;   // all contexts, for the frame line

    struct Snapshot { float f[16]; bool valid; };
    std::unordered_map<void*, uint64_t> g_ShaderHash;             // shader object -> bytecode hash
    std::unordered_map<ID3D11Resource*, Snapshot> g_LatestUpload;  // constant buffer -> latest 16 floats
    std::unordered_map<ID3D11Resource*, void*> g_Mapped;           // Map(): pData until Unmap()

    uint64_t Fnv1a(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    // The game-code frames above a call, as module-relative offsets, up to `max`.
    std::string GameCallers(int max)
    {
        void* frames[32] {};
        const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        std::string out;
        int printed = 0;
        for (USHORT i = 0; i < n && printed < max; i++)
        {
            const auto a = reinterpret_cast<uintptr_t>(frames[i]);
            HMODULE owner = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(a), &owner) || owner != mgs4e::game::Module())
            {
                continue;
            }
            out += (out.empty() ? "+" : " < +") + std::format("{:X}", a - base);
            printed++;
        }
        return out.empty() ? "(no game frames)" : out;
    }

    std::string DescribeTarget(ID3D11RenderTargetView* rtv)
    {
        if (!rtv) { return "none"; }
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (!res) { return "?"; }
        std::string out = "?";
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            D3D11_TEXTURE2D_DESC d {};
            tex->GetDesc(&d);
            out = std::format("{}x{} fmt{} {:#x}", d.Width, d.Height, static_cast<int>(d.Format), reinterpret_cast<uintptr_t>(res) & 0xffffff);
            tex->Release();
        }
        res->Release();
        return out;
    }

    void Snapshot16(ID3D11Resource* res, const void* data, size_t size)
    {
        Snapshot s {};
        const size_t n = std::min<size_t>(16, size / sizeof(float));
        if (n == 0) { return; }
        std::memcpy(s.f, data, n * sizeof(float));
        s.valid = true;
        g_LatestUpload[res] = s;
    }

    // Byte width of `res` when it is a constant buffer, else 0.
    size_t ConstantBufferSize(ID3D11Resource* res)
    {
        ID3D11Buffer* buf = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf))) || !buf) { return 0; }
        D3D11_BUFFER_DESC d {};
        buf->GetDesc(&d);
        buf->Release();
        return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? d.ByteWidth : 0;
    }

    void LogDraw(void* self, const char* kind, UINT count, UINT start, UINT baseVertex)
    {
        g_DrawsSeen.fetch_add(1);
        if (g_FramesToLog.load() <= 0) { return; }
        std::lock_guard lock(g_Mutex);
        ContextState& st = g_State[self];
        st.draws++;
        g_DrawsThisFrame++;
        std::string cb = "cb0 -";
        if (st.vsCb0)
        {
            const auto it = g_LatestUpload.find(st.vsCb0);
            if (it != g_LatestUpload.end() && it->second.valid)
            {
                cb = "cb0";
                for (int i = 0; i < 16; i++) { cb += std::format(" {:.4g}", it->second.f[i]); }
            }
        }
        const auto vs = g_ShaderHash.find(st.vs);
        const auto ps = g_ShaderHash.find(st.ps);
        spdlog::info("PW census: f{} {}#{} {} n={} start={} base={} topo={} vp=({:.0f},{:.0f} {:.0f}x{:.0f}) sc=({},{})-({},{}) rt={} vs={} ps={} {} | {}",
            g_Frame.load(), HooksFor(self).name, st.draws, kind, count, start, baseVertex, static_cast<int>(st.topology),
            st.viewport.TopLeftX, st.viewport.TopLeftY, st.viewport.Width, st.viewport.Height,
            st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom, st.target,
            vs == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", vs->second).substr(0, 8),
            ps == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", ps->second).substr(0, 8),
            cb, GameCallers(5));
    }

    // ---- context hooks (both classes) ------------------------------------------------------------
    void STDMETHODCALLTYPE Hooked_Draw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertex)
    {
        LogDraw(self, "Draw", vertexCount, startVertex, 0);
        Original<decltype(&Hooked_Draw)>(self, kCtxDraw)(self, vertexCount, startVertex);
    }

    void STDMETHODCALLTYPE Hooked_DrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndex, INT baseVertex)
    {
        LogDraw(self, "DrawIndexed", indexCount, startIndex, static_cast<UINT>(baseVertex));
        Original<decltype(&Hooked_DrawIndexed)>(self, kCtxDrawIndexed)(self, indexCount, startIndex, baseVertex);
    }

    void STDMETHODCALLTYPE Hooked_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCount, UINT instances, UINT startIndex, INT baseVertex, UINT startInstance)
    {
        LogDraw(self, "DrawIndexedInstanced", indexCount, startIndex, static_cast<UINT>(baseVertex));
        Original<decltype(&Hooked_DrawIndexedInstanced)>(self, kCtxDrawIndexedInstanced)(self, indexCount, instances, startIndex, baseVertex, startInstance);
    }

    void STDMETHODCALLTYPE Hooked_DrawInstanced(ID3D11DeviceContext* self, UINT vertexCount, UINT instances, UINT startVertex, UINT startInstance)
    {
        LogDraw(self, "DrawInstanced", vertexCount, startVertex, 0);
        Original<decltype(&Hooked_DrawInstanced)>(self, kCtxDrawInstanced)(self, vertexCount, instances, startVertex, startInstance);
    }

    void STDMETHODCALLTYPE Hooked_RSSetViewports(ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports)
    {
        if (count > 0 && viewports) { std::lock_guard lock(g_Mutex); g_State[self].viewport = viewports[0]; }
        Original<decltype(&Hooked_RSSetViewports)>(self, kCtxRSSetViewports)(self, count, viewports);
    }

    void STDMETHODCALLTYPE Hooked_RSSetScissorRects(ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects)
    {
        if (count > 0 && rects) { std::lock_guard lock(g_Mutex); g_State[self].scissor = rects[0]; }
        Original<decltype(&Hooked_RSSetScissorRects)>(self, kCtxRSSetScissorRects)(self, count, rects);
    }

    void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
    {
        if (g_FramesToLog.load() > 0)
        {
            std::lock_guard lock(g_Mutex);
            ContextState& st = g_State[self];
            st.target = (count > 0 && views) ? DescribeTarget(views[0]) : std::string("none");
            st.target += depth ? " +depth" : "";
        }
        Original<decltype(&Hooked_OMSetRenderTargets)>(self, kCtxOMSetRenderTargets)(self, count, views, depth);
    }

    void STDMETHODCALLTYPE Hooked_VSSetShader(ID3D11DeviceContext* self, ID3D11VertexShader* shader, ID3D11ClassInstance* const* instances, UINT n)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].vs = shader; }
        Original<decltype(&Hooked_VSSetShader)>(self, kCtxVSSetShader)(self, shader, instances, n);
    }

    void STDMETHODCALLTYPE Hooked_PSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* shader, ID3D11ClassInstance* const* instances, UINT n)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].ps = shader; }
        Original<decltype(&Hooked_PSSetShader)>(self, kCtxPSSetShader)(self, shader, instances, n);
    }

    void STDMETHODCALLTYPE Hooked_IASetPrimitiveTopology(ID3D11DeviceContext* self, D3D11_PRIMITIVE_TOPOLOGY topology)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].topology = topology; }
        Original<decltype(&Hooked_IASetPrimitiveTopology)>(self, kCtxIASetPrimitiveTopology)(self, topology);
    }

    void STDMETHODCALLTYPE Hooked_VSSetConstantBuffers(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11Buffer* const* buffers)
    {
        if (start == 0 && count > 0 && buffers) { std::lock_guard lock(g_Mutex); g_State[self].vsCb0 = buffers[0]; }
        Original<decltype(&Hooked_VSSetConstantBuffers)>(self, kCtxVSSetConstantBuffers)(self, start, count, buffers);
    }

    HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
    {
        const HRESULT r = Original<decltype(&Hooked_Map)>(self, kCtxMap)(self, res, sub, type, flags, mapped);
        if (SUCCEEDED(r) && mapped && mapped->pData && sub == 0 && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_WRITE))
        {
            std::lock_guard lock(g_Mutex);
            g_Mapped[res] = mapped->pData;
        }
        return r;
    }

    void STDMETHODCALLTYPE Hooked_Unmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub)
    {
        {
            std::lock_guard lock(g_Mutex);
            const auto it = g_Mapped.find(res);
            if (it != g_Mapped.end())
            {
                if (const size_t size = ConstantBufferSize(res)) { Snapshot16(res, it->second, size); }
                g_Mapped.erase(it);
            }
        }
        Original<decltype(&Hooked_Unmap)>(self, kCtxUnmap)(self, res, sub);
    }

    void STDMETHODCALLTYPE Hooked_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
    {
        if (res && data && sub == 0 && !box)
        {
            if (const size_t size = ConstantBufferSize(res))
            {
                std::lock_guard lock(g_Mutex);
                Snapshot16(res, data, size);
            }
        }
        Original<decltype(&Hooked_UpdateSubresource)>(self, kCtxUpdateSubresource)(self, res, sub, box, data, rowPitch, depthPitch);
    }

    void STDMETHODCALLTYPE Hooked_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore)
    {
        g_ListsSeen.fetch_add(1);
        if (g_FramesToLog.load() > 0) { spdlog::info("PW census: f{} ExecuteCommandList {:#x} | {}", g_Frame.load(), reinterpret_cast<uintptr_t>(list) & 0xffffff, GameCallers(4)); }
        Original<decltype(&Hooked_ExecuteCommandList)>(self, kCtxExecuteCommandList)(self, list, restore);
    }

    void InstallContextHooks(ContextHooks& h, void** vtable, const char* name)
    {
        struct Slot { size_t index; void* target; };
        const Slot slots[] = {
            { kCtxDraw, reinterpret_cast<void*>(Hooked_Draw) },
            { kCtxDrawIndexed, reinterpret_cast<void*>(Hooked_DrawIndexed) },
            { kCtxDrawIndexedInstanced, reinterpret_cast<void*>(Hooked_DrawIndexedInstanced) },
            { kCtxDrawInstanced, reinterpret_cast<void*>(Hooked_DrawInstanced) },
            { kCtxRSSetViewports, reinterpret_cast<void*>(Hooked_RSSetViewports) },
            { kCtxRSSetScissorRects, reinterpret_cast<void*>(Hooked_RSSetScissorRects) },
            { kCtxOMSetRenderTargets, reinterpret_cast<void*>(Hooked_OMSetRenderTargets) },
            { kCtxVSSetShader, reinterpret_cast<void*>(Hooked_VSSetShader) },
            { kCtxPSSetShader, reinterpret_cast<void*>(Hooked_PSSetShader) },
            { kCtxVSSetConstantBuffers, reinterpret_cast<void*>(Hooked_VSSetConstantBuffers) },
            { kCtxMap, reinterpret_cast<void*>(Hooked_Map) },
            { kCtxUnmap, reinterpret_cast<void*>(Hooked_Unmap) },
            { kCtxUpdateSubresource, reinterpret_cast<void*>(Hooked_UpdateSubresource) },
            { kCtxIASetPrimitiveTopology, reinterpret_cast<void*>(Hooked_IASetPrimitiveTopology) },
            { kCtxExecuteCommandList, reinterpret_cast<void*>(Hooked_ExecuteCommandList) },
        };
        h.vtable = vtable;
        h.name = name;
        DWORD old = 0;
        if (!VirtualProtect(vtable, kSlotCount * sizeof(void*), PAGE_READWRITE, &old))
        {
            spdlog::error("PW census: {} context vtable at {:#x} could not be unprotected (error {}); not hooked.", name, reinterpret_cast<uintptr_t>(vtable), GetLastError());
            h.vtable = nullptr;
            return;
        }
        std::string shared;
        for (const Slot& sl : slots)
        {
            void* current = vtable[sl.index];
            // Already our function: this class shares the entry with the immediate's table
            // (never seen, the tables are separate, but cheap to guard).
            if (current == sl.target) { h.original[sl.index] = g_Immediate.original[sl.index]; shared += std::format(" {}", sl.index); continue; }
            h.original[sl.index] = current;
            vtable[sl.index] = sl.target;
        }
        VirtualProtect(vtable, kSlotCount * sizeof(void*), old, &old);
        spdlog::info("PW census: {} context vtable at {:#x} patched ({} entries){}.", name, reinterpret_cast<uintptr_t>(vtable), std::size(slots),
            shared.empty() ? "" : "; entries already ours:" + shared);
    }

    // ---- device hooks -------------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Hooked_CreateVertexShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out)
    {
        const HRESULT r = Device_CreateVertexShader_hook.stdcall<HRESULT>(self, code, size, linkage, out);
        if (SUCCEEDED(r) && out && *out && code) { std::lock_guard lock(g_Mutex); g_ShaderHash[*out] = Fnv1a(code, size); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreatePixelShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
    {
        const HRESULT r = Device_CreatePixelShader_hook.stdcall<HRESULT>(self, code, size, linkage, out);
        if (SUCCEEDED(r) && out && *out && code) { std::lock_guard lock(g_Mutex); g_ShaderHash[*out] = Fnv1a(code, size); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateDeferredContext(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out)
    {
        const HRESULT r = Device_CreateDeferredContext_hook.stdcall<HRESULT>(self, flags, out);
        if (SUCCEEDED(r) && out && *out)
        {
            static int created = 0;
            created++;
            if (!g_Deferred.vtable)
            {
                InstallContextHooks(g_Deferred, *reinterpret_cast<void***>(*out), "deferred");
            }
            spdlog::info("PW census: deferred context #{} created ({:#x}) | {}", created, reinterpret_cast<uintptr_t>(*out) & 0xffffff, GameCallers(3));
        }
        return r;
    }

    // ---- frame boundary --------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT sync, UINT flags)
    {
        if ((flags & DXGI_PRESENT_TEST) == 0)
        {
            const int left = g_FramesToLog.load();
            if (left > 0)
            {
                spdlog::info("PW census: frame {} ended with {} draw(s) on all contexts.", g_Frame.load(), g_DrawsThisFrame);
                g_FramesToLog.store(left - 1);
                if (left - 1 == 0) { spdlog::info("PW census: done."); }
            }
            g_Frame.fetch_add(1);
            std::lock_guard lock(g_Mutex);
            g_DrawsThisFrame = 0;
            for (auto& [ctx, st] : g_State) { st.draws = 0; }
        }
        return SwapChain_Present_hook.stdcall<HRESULT>(self, sync, flags);
    }

    void HookSwapChain(IUnknown* chain)
    {
        static bool hooked = false;
        if (hooked || !chain) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(chain);
        SwapChain_Present_hook = safetyhook::create_inline(vtable[kSwapChainPresent], reinterpret_cast<void*>(Hooked_Present));
        spdlog::info("PW census: swap chain Present hook {}.", SwapChain_Present_hook ? "ok" : "FAILED");
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
    {
        const HRESULT r = Factory_CreateSwapChain_hook.stdcall<HRESULT>(self, device, desc, chain);
        if (SUCCEEDED(r) && chain && *chain)
        {
            if (desc) { spdlog::info("PW census: swap chain {}x{} fmt{} windowed={} buffers={} effect={} window={:#x}.", desc->BufferDesc.Width, desc->BufferDesc.Height, static_cast<int>(desc->BufferDesc.Format), desc->Windowed, desc->BufferCount, static_cast<int>(desc->SwapEffect), reinterpret_cast<uintptr_t>(desc->OutputWindow)); }
            HookSwapChain(*chain);
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict, IDXGISwapChain1** chain)
    {
        const HRESULT r = Factory2_CreateSwapChainForHwnd_hook.stdcall<HRESULT>(self, device, window, desc, fs, restrict, chain);
        if (SUCCEEDED(r) && chain && *chain)
        {
            if (desc) { spdlog::info("PW census: swap chain (ForHwnd) {}x{} fmt{} buffers={} effect={} window={:#x}.", desc->Width, desc->Height, static_cast<int>(desc->Format), desc->BufferCount, static_cast<int>(desc->SwapEffect), reinterpret_cast<uintptr_t>(window)); }
            HookSwapChain(*chain);
        }
        return r;
    }

    void HookFactory(IUnknown* factory)
    {
        static bool hooked = false;
        if (hooked || !factory) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(factory);
        Factory_CreateSwapChain_hook = safetyhook::create_inline(vtable[kFactoryCreateSwapChain], reinterpret_cast<void*>(Hooked_CreateSwapChain));
        IDXGIFactory2* two = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&two))) && two)
        {
            two->Release();
            Factory2_CreateSwapChainForHwnd_hook = safetyhook::create_inline(vtable[kFactory2CreateSwapChainForHwnd], reinterpret_cast<void*>(Hooked_CreateSwapChainForHwnd));
        }
        spdlog::info("PW census: DXGI factory hooked (CreateSwapChain {}, CreateSwapChainForHwnd {}).",
            Factory_CreateSwapChain_hook ? "ok" : "FAILED", Factory2_CreateSwapChainForHwnd_hook ? "ok" : "absent");
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

    // ---- device / immediate context -----------------------------------------------------------------
    void HookDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        static bool hooked = false;
        if (hooked || !device) { return; }
        hooked = true;
        spdlog::info("PW census: D3D11 device created, feature level {:#x}.", static_cast<int>(device->GetFeatureLevel()));

        void** dv = *reinterpret_cast<void***>(device);
        Device_CreateVertexShader_hook = safetyhook::create_inline(dv[kDevCreateVertexShader], reinterpret_cast<void*>(Hooked_CreateVertexShader));
        Device_CreatePixelShader_hook = safetyhook::create_inline(dv[kDevCreatePixelShader], reinterpret_cast<void*>(Hooked_CreatePixelShader));
        Device_CreateDeferredContext_hook = safetyhook::create_inline(dv[kDevCreateDeferredContext], reinterpret_cast<void*>(Hooked_CreateDeferredContext));

        ID3D11DeviceContext* immediate = context;
        if (!immediate) { device->GetImmediateContext(&immediate); }
        if (!immediate) { spdlog::error("PW census: no immediate context."); return; }
        InstallContextHooks(g_Immediate, *reinterpret_cast<void***>(immediate), "immediate");
        if (!context) { immediate->Release(); }

        // Draw rate for the first half minute, logged or not: which path the draws take.
        std::thread([]
        {
            uint64_t lastDraws = 0, lastLists = 0, lastFrames = 0;
            for (int i = 0; i < 30; i++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const uint64_t d = g_DrawsSeen.load(), l = g_ListsSeen.load(), f = g_Frame.load();
                spdlog::info("PW census: rate: {} draw(s), {} command list(s), {} frame(s) in the last second.", d - lastDraws, l - lastLists, f - lastFrames);
                lastDraws = d; lastLists = l; lastFrames = f;
            }
        }).detach();
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
        const HRESULT r = D3D11CreateDeviceAndSwapChain_hook.stdcall<HRESULT>(adapter, driverType, software, flags, levels, levelCount, sdk, desc, chain, device, level, context);
        if (SUCCEEDED(r) && device && *device) { HookDevice(*device, context ? *context : nullptr); }
        if (SUCCEEDED(r) && chain && *chain)
        {
            if (desc) { spdlog::info("PW census: swap chain (with device) {}x{} fmt{} windowed={} buffers={} effect={}.", desc->BufferDesc.Width, desc->BufferDesc.Height, static_cast<int>(desc->BufferDesc.Format), desc->Windowed, desc->BufferCount, static_cast<int>(desc->SwapEffect)); }
            HookSwapChain(*chain);
        }
        return r;
    }

    // ---- triggers ----------------------------------------------------------------------------------
    constexpr const char* kLiveFile = "C:/mgspf_tools/pw/live.txt";

    void RunCommand(const std::string& line)
    {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "census")
        {
            int frames = DrawCensus::iCensusFrames;
            in >> frames;
            g_FramesToLog.store(std::max(1, frames));
            spdlog::info("PW census: logging the next {} frame(s) (command).", std::max(1, frames));
        }
        else if (!cmd.empty())
        {
            spdlog::warn("PW live: unknown command '{}'.", line);
        }
    }

    void PollThread()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            HANDLE file = CreateFileA(kLiveFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE) { continue; }
            char buf[512] {};
            DWORD read = 0;
            ReadFile(file, buf, sizeof(buf) - 1, &read, nullptr);
            CloseHandle(file);
            if (read == 0) { continue; }
            std::string text(buf, read);
            HANDLE clear = CreateFileA(kLiveFile, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
            if (clear != INVALID_HANDLE_VALUE) { CloseHandle(clear); }
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                if (!line.empty()) { RunCommand(line); }
            }
        }
    }
}

namespace DrawCensus
{
    void Install()
    {
        if (const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")))
            {
                D3D11CreateDevice_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDevice));
            }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
            {
                D3D11CreateDeviceAndSwapChain_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDeviceAndSwapChain));
            }
        }
        if (const HMODULE dxgi = LoadLibraryW(L"dxgi.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory"))) { CreateDXGIFactory_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) { CreateDXGIFactory1_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory1)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2"))) { CreateDXGIFactory2_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory2)); }
        }
        spdlog::info("PW census: D3D11CreateDevice {}, D3D11CreateDeviceAndSwapChain {}, CreateDXGIFactory {}/{}/{}.",
            D3D11CreateDevice_hook ? "ok" : "FAILED", D3D11CreateDeviceAndSwapChain_hook ? "ok" : "FAILED",
            CreateDXGIFactory_hook ? "ok" : "-", CreateDXGIFactory1_hook ? "ok" : "-", CreateDXGIFactory2_hook ? "ok" : "-");

        if (iCensusAtSeconds > 0)
        {
            const int at = iCensusAtSeconds;
            const int frames = iCensusFrames;
            std::thread([at, frames]
            {
                std::this_thread::sleep_for(std::chrono::seconds(at));
                g_FramesToLog.store(std::max(1, frames));
                spdlog::info("PW census: logging the next {} frame(s) ({} s after init).", std::max(1, frames), at);
            }).detach();
        }
        if (bLiveCommands)
        {
            spdlog::info("PW live: polling {}.", kLiveFile);
            std::thread(PollThread).detach();
        }
    }
}

#endif
