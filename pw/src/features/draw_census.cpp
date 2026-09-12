#include "pch.hpp"
#include "draw_census.hpp"

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "ui_bias.hpp"
#include "internal_size.hpp"
#include "post_scale.hpp"

#include <unordered_set>
#include <map>
#include <cctype>

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
    SafetyHookInline Device_CreateComputeShader_hook {};
    SafetyHookInline Device_CreateTexture2D_hook {};

    // ID3D11Device vtable slots.
    constexpr size_t kDevCreateVertexShader = 12;
    constexpr size_t kDevCreatePixelShader = 15;
    constexpr size_t kDevCreateTexture2D = 5;
    constexpr size_t kDevCreateComputeShader = 18;
    constexpr size_t kDevCreateDeferredContext = 27;
    // ID3D11DeviceContext vtable slots.
    constexpr size_t kCtxVSSetConstantBuffers = 7;
    constexpr size_t kCtxPSSetShaderResources = 8;
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
    constexpr size_t kCtxDispatch = 41;
    constexpr size_t kCtxDispatchIndirect = 42;
    constexpr size_t kCtxCopySubresourceRegion = 46;
    constexpr size_t kCtxCopyResource = 47;
    constexpr size_t kCtxResolveSubresource = 57;
    constexpr size_t kCtxExecuteCommandList = 58;
    constexpr size_t kCtxCSSetShaderResources = 67;
    constexpr size_t kCtxCSSetUnorderedAccessViews = 68;
    constexpr size_t kCtxCSSetShader = 69;
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
    constexpr size_t kSlotCount = 80;
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
        ID3D11Buffer* vsCb[4] {};   // vertex shader constant buffer slots 0..3
        std::string texture = "-";      // pixel shader resource slot 0
        std::string csTexture = "-";    // compute shader resource slot 0
        std::string csTarget = "-";     // compute unordered access view slot 0
        void* cs = nullptr;
        std::vector<uint8_t> lastVertexUpload;   // head of the last non-constant buffer unmapped on this context
        size_t lastVertexUploadSize = 0;
        std::string lastVertexUploadCaller;
        std::string trace;              // call order on this context since its last draw (first draws of a census frame only)
        std::string lastStampedTarget;  // timing: the target of the last stamped draw on this context
        bool stampNext = false;         // timing: the op after a stamped op gets a closing stamp, so each pass is bounded
        int draws = 0;   // this frame (reset at Present for the immediate; at ExecuteCommandList for a deferred)
    };
    std::mutex g_Mutex;
    std::unordered_map<void*, ContextState> g_State;
    std::atomic<int> g_FramesToLog { 0 };
    std::atomic<uint64_t> g_Frame { 0 };
    std::atomic<uint64_t> g_DrawsSeen { 0 };
    std::atomic<uint64_t> g_ListsSeen { 0 };
    std::atomic<uint64_t> g_GpuLocalStripped { 0 };
    std::atomic<bool> g_TitleSeen { false };
    std::atomic<uint64_t> g_TextureMaps { 0 };       // Map calls on large textures (any time)
    std::atomic<uint64_t> g_TextureMapFails { 0 };

    // GPU timing of one census frame: a timestamp query is ended on the recording context just
    // before each pass-like operation (a draw onto a new target, a small full-screen draw, a
    // dispatch, copy or resolve, and each command-list execution), the disjoint query brackets
    // the frame on the immediate context, and three frames later the stamps are read back and
    // the intervals between consecutive stamps (in GPU order) are reported, largest first.
    struct Stamp { ID3D11Query* query; std::string what; uint64_t ticks; };
    ID3D11Device* g_Device = nullptr;
    ID3D11DeviceContext* g_ImmediateCtx = nullptr;
    ID3D11Query* g_Disjoint = nullptr;
    std::vector<Stamp> g_Stamps;                 // under g_Mutex
    std::vector<ID3D11Query*> g_StampPool;       // reused across frames
    size_t g_StampNext = 0;
    std::atomic<int> g_TimingState { 0 };        // 0 idle, 1 armed (start at next Present), 2 recording, 3 ended (wait), 4.. waiting frames
    int g_TimingFramesLeft = 0;                  // frames still to time in this census
    struct Acc { double sum = 0; int n = 0; double max = 0; };
    std::map<std::string, Acc> g_TimingAcc;      // per pass label (draw kind + shaders, sizes stripped), across timed frames
    std::vector<double> g_FrameTotals;           // work per timed frame (first stamp after 'frame start' to the last)
    std::chrono::steady_clock::time_point g_TimingWallStart {};
    uint64_t g_TimingFrameStart = 0;
    uint64_t g_TimingFrame = 0;
    constexpr size_t kStampPoolSize = 1200;
    std::string g_LastTarget;                    // for "new target" detection, immediate order only

    void EnsureStampPool()
    {
        if (!g_Device || !g_StampPool.empty()) { return; }
        D3D11_QUERY_DESC qd { D3D11_QUERY_TIMESTAMP, 0 };
        for (size_t i = 0; i < kStampPoolSize; i++)
        {
            ID3D11Query* q = nullptr;
            if (SUCCEEDED(g_Device->CreateQuery(&qd, &q)) && q) { g_StampPool.push_back(q); }
        }
        D3D11_QUERY_DESC dd { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        g_Device->CreateQuery(&dd, &g_Disjoint);
        spdlog::info("PW timing: {} timestamp queries and a disjoint query created.", g_StampPool.size());
    }

    // Caller holds g_Mutex. Records a stamp on `ctx` before the operation `what`.
    void StampBefore(ID3D11DeviceContext* ctx, std::string what)
    {
        if (g_TimingState.load() != 2 || g_StampNext >= g_StampPool.size()) { return; }
        ID3D11Query* q = g_StampPool[g_StampNext++];
        ctx->End(q);
        g_Stamps.push_back({ q, std::move(what), 0 });
    }

    // Strips object addresses from a stamp label so the same pass accumulates across frames.
    std::string PassKey(const std::string& what)
    {
        std::string k;
        for (size_t i = 0; i < what.size(); i++)
        {
            // an object address is " 0x" followed by hex digits; a size like 7680x4352 is not
            if (what.compare(i, 3, " 0x") == 0) { i += 3; while (i < what.size() && std::isxdigit(static_cast<unsigned char>(what[i]))) { i++; } i--; continue; }
            k += what[i];
        }
        return k;
    }

    void ReportTiming()
    {
        std::lock_guard lock(g_Mutex);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj {};
        const bool ready = g_ImmediateCtx->GetData(g_Disjoint, &dj, sizeof(dj), 0) == S_OK && dj.Frequency;
        if (ready)
        {
            for (Stamp& st : g_Stamps) { if (g_ImmediateCtx->GetData(st.query, &st.ticks, sizeof(st.ticks), 0) != S_OK) { st.ticks = 0; } }
            std::vector<Stamp*> order;
            for (Stamp& st : g_Stamps) { if (st.ticks) { order.push_back(&st); } }
            std::sort(order.begin(), order.end(), [](const Stamp* a, const Stamp* b) { return a->ticks < b->ticks; });
            double total = 0;
            for (size_t i = 0; i + 1 < order.size(); i++)
            {
                const double ms = static_cast<double>(order[i + 1]->ticks - order[i]->ticks) * 1000.0 / static_cast<double>(dj.Frequency);
                if (order[i]->what == "frame start") { continue; }   // the vsync wait before the frame's first work
                total += ms;
                Acc& a = g_TimingAcc[PassKey(order[i]->what)];
                a.sum += ms; a.n++; a.max = std::max(a.max, ms);
            }
            g_FrameTotals.push_back(total);
        }
        g_Stamps.clear();
        g_StampNext = 0;
        if (--g_TimingFramesLeft > 0) { g_TimingState.store(1); return; }
        g_TimingState.store(0);
        if (g_FrameTotals.empty()) { spdlog::warn("PW timing: no frames measured."); return; }
        double sum = 0, mx = 0; for (double t : g_FrameTotals) { sum += t; mx = std::max(mx, t); }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_TimingWallStart).count();
        const double fps = wall > 0 ? static_cast<double>(g_Frame.load() - g_TimingFrameStart) / wall : 0;
        spdlog::info("PW timing: {} frames timed over {:.1f} s at {:.1f} fps: mean GPU work {:.2f} ms per frame, max {:.2f} ms. Mean cost per pass (sum over the frame / frames; count = occurrences per frame):", g_FrameTotals.size(), wall, fps, sum / g_FrameTotals.size(), mx);
        std::vector<std::pair<std::string, Acc>> rows(g_TimingAcc.begin(), g_TimingAcc.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.sum > b.second.sum; });
        for (size_t i = 0; i < rows.size() && i < 24; i++)
        {
            spdlog::info("PW timing:   {:7.3f} ms/frame  x{:<4.1f} max {:6.3f}  {}", rows[i].second.sum / g_FrameTotals.size(), static_cast<double>(rows[i].second.n) / g_FrameTotals.size(), rows[i].second.max, rows[i].first);
        }
        g_TimingAcc.clear();
        g_FrameTotals.clear();
    }
    int g_DrawsThisFrame = 0;   // all contexts, for the frame line

    struct Snapshot { float f[64]; size_t count; bool valid; };
    std::unordered_map<void*, uint64_t> g_ShaderHash;             // shader object -> bytecode hash
    std::unordered_map<ID3D11Resource*, Snapshot> g_LatestUpload;  // constant buffer -> latest 16 floats
    struct Mapping { void* data; size_t size; };
    std::unordered_map<ID3D11Resource*, Mapping> g_Mapped;         // Map(): pData until Unmap()

    // Byte width of `res` when it is a buffer, else 0; `constant` says whether it binds as one.
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

    // Decodes the bounding rectangle of the positions of an upload, taking the position as the
    // last three floats of each vertex, for the first few vertices.
    std::string DecodeVertices(const void* data, size_t size, size_t stride)
    {
        if (stride < 12 || size < stride) { return "?"; }
        const size_t count = std::min<size_t>(size / stride, 64);
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f, z = 0;
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < count; i++)
        {
            float xyz[3];
            std::memcpy(xyz, p + i * stride + stride - 12, sizeof(xyz));
            minX = std::min(minX, xyz[0]); maxX = std::max(maxX, xyz[0]);
            minY = std::min(minY, xyz[1]); maxY = std::max(maxY, xyz[1]);
            z = xyz[2];
        }
        uint32_t colour = 0;
        std::memcpy(&colour, p + stride - 16, sizeof(colour));
        return std::format("stride={} rect=({:g},{:g})-({:g},{:g}) z={:g} colour={:08x}", stride, minX, minY, maxX, maxY, z, colour);
    }

    std::string DescribeResource(ID3D11Resource* res)
    {
        if (!res) { return "none"; }
        std::string out = "?";
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            D3D11_TEXTURE2D_DESC d {};
            tex->GetDesc(&d);
            out = std::format("{}x{} fmt{} {:#x}", d.Width, d.Height, static_cast<int>(d.Format), reinterpret_cast<uintptr_t>(res) & 0xffffff);
            tex->Release();
        }
        return out;
    }

    std::string DescribeUav(ID3D11UnorderedAccessView* view)
    {
        if (!view) { return "-"; }
        ID3D11Resource* res = nullptr;
        view->GetResource(&res);
        std::string out = DescribeResource(res);
        if (res) { res->Release(); }
        return out;
    }

    std::string DescribeTexture(ID3D11ShaderResourceView* view)
    {
        if (!view) { return "-"; }
        ID3D11Resource* res = nullptr;
        view->GetResource(&res);
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

    std::string Hex(const void* data, size_t n)
    {
        std::string out;
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; i++) { out += std::format("{}{:02x}", (i && i % 4 == 0) ? " " : "", p[i]); }
        return out;
    }

    // Lab key Dump Shaders: every shader's bytecode goes to C:\mgspf_tools\pw\shaders\<hash>.<kind>.cso
    // so the passes can be read with D3DDisassemble offline.
    void DumpShader(const void* code, size_t size, uint64_t hash, const char* kind)
    {
        if (!DrawCensus::bDumpShaders) { return; }
        CreateDirectoryA("C:/mgspf_tools/pw/shaders", nullptr);
                // The 8-hex-digit id used everywhere else is the first 8 digits of the 16-digit hash.
        const std::string byId = std::format("C:/mgspf_tools/pw/shaders/{}.{}.cso", std::format("{:016x}", hash).substr(0, 8), kind);
        HANDLE f = CreateFileA(byId.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { return; }
        DWORD written = 0;
        WriteFile(f, code, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(f);
    }

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
        const size_t n = std::min<size_t>(64, size / sizeof(float));
        if (n == 0) { return; }
        std::memcpy(s.f, data, n * sizeof(float));
        s.count = n;
        s.valid = true;
        g_LatestUpload[res] = s;
    }

    // Appends one token to the context's call trace while a census is on and the frame is young.
    void Trace(void* self, const std::string& token)
    {
        if (g_FramesToLog.load() <= 0) { return; }
        ContextState& st = g_State[self];   // caller holds g_Mutex
        if (st.draws < 40 && st.trace.size() < 600) { st.trace += (st.trace.empty() ? "" : " > ") + token; }
    }

    void LogDraw(void* self, const char* kind, UINT count, UINT start, UINT baseVertex)
    {
        g_DrawsSeen.fetch_add(1);
        if (g_FramesToLog.load() <= 0 && g_TimingState.load() != 2) { return; }
        std::lock_guard lock(g_Mutex);
        ContextState& st = g_State[self];
        if (g_TimingState.load() == 2)
        {
            // Pass-like draws only: onto a target different from this context's previous draw, or
            // a small full-screen quad/strip (post passes), never the thousands of world draws.
            // The post-chain shaders measured at the title and in the mission (pixel-shader
            // bytecode hashes, 8 hex digits): the HDR pass, the depth passes, the blur chain,
            // the composite and the final blit. HUD quads share none of them.
            static const std::unordered_set<std::string> kPostShaders = { "2752864a", "baa9f4b4", "fcb43266", "4c3c9a5f", "6c27b4cf", "07f12e1d", "1db0ebfb", "493494fd" };
            const std::string psHash = g_ShaderHash.count(st.ps) ? std::format("{:016x}", g_ShaderHash[st.ps]).substr(0, 8) : "?";
            const bool newTarget = st.target != st.lastStampedTarget;
            const bool postPass = count <= 12 && (kPostShaders.count(psHash) || (psHash == "7023c633" && count <= 12));
            if (newTarget || postPass)
            {
                st.lastStampedTarget = st.target;
                st.stampNext = true;
                StampBefore(static_cast<ID3D11DeviceContext*>(self), std::format("{} n={} rt={} tex={} vs={} ps={}", kind, count, st.target, st.texture,
                    g_ShaderHash.count(st.vs) ? std::format("{:016x}", g_ShaderHash[st.vs]).substr(0, 8) : "?", g_ShaderHash.count(st.ps) ? std::format("{:016x}", g_ShaderHash[st.ps]).substr(0, 8) : "?"));
            }
            else if (st.stampNext)
            {
                st.stampNext = false;
                StampBefore(static_cast<ID3D11DeviceContext*>(self), std::format("(scene draws from {} n={} ps={} ...)", kind, count, psHash));
            }
        }
        if (g_FramesToLog.load() <= 0) { return; }
        st.draws++;
        g_DrawsThisFrame++;
        // Every bound vertex constant buffer: slot, byte size, and its latest upload (slot 0 in
        // full, up to 64 floats; the others 16 floats), in rows of four.
        std::string cb;
        for (int slot = 0; slot < 4; slot++)
        {
            if (!st.vsCb[slot]) { continue; }
            D3D11_BUFFER_DESC d {};
            st.vsCb[slot]->GetDesc(&d);
            cb += std::format("{}cb{}[{}B]", cb.empty() ? "" : " ", slot, d.ByteWidth);
            const auto it = g_LatestUpload.find(st.vsCb[slot]);
            if (it == g_LatestUpload.end() || !it->second.valid) { cb += " -"; continue; }
            const size_t n = std::min<size_t>(it->second.count, slot == 0 ? 64 : 16);
            for (size_t i = 0; i < n; i++) { cb += std::format("{}{:.4g}", (i % 4 == 0) ? " |" : " ", it->second.f[i]); }
        }
        if (cb.empty()) { cb = "cb -"; }
        const auto vs = g_ShaderHash.find(st.vs);
        const auto ps = g_ShaderHash.find(st.ps);
        spdlog::info("PW census: f{} {}#{} {} n={} start={} base={} topo={} vp=({:.0f},{:.0f} {:.0f}x{:.0f}) sc=({},{})-({},{}) rt={} tex={} vs={} ps={} {} | {}",
            g_Frame.load(), HooksFor(self).name, st.draws, kind, count, start, baseVertex, static_cast<int>(st.topology),
            st.viewport.TopLeftX, st.viewport.TopLeftY, st.viewport.Width, st.viewport.Height,
            st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom, st.target, st.texture,
            vs == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", vs->second).substr(0, 8),
            ps == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", ps->second).substr(0, 8),
            cb, GameCallers(10));
        if (!st.trace.empty())
        {
            spdlog::info("PW census:   calls {} > {}", st.trace, kind);
            st.trace.clear();
        }
        if (!st.lastVertexUpload.empty())
        {
            // The stride follows from the draw: the upload holds exactly the vertices drawn.
            const size_t stride = count ? st.lastVertexUploadSize / count : 0;
            spdlog::info("PW census:   vertices {}B {} head {} | {}", st.lastVertexUploadSize,
                DecodeVertices(st.lastVertexUpload.data(), st.lastVertexUpload.size(), stride),
                Hex(st.lastVertexUpload.data(), std::min<size_t>(st.lastVertexUpload.size(), 24)), st.lastVertexUploadCaller);
            st.lastVertexUpload.clear();
        }
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
        { std::lock_guard lock(g_Mutex); g_State[self].vs = shader; Trace(self, "VSSetShader"); }
        Original<decltype(&Hooked_VSSetShader)>(self, kCtxVSSetShader)(self, shader, instances, n);
    }

    void STDMETHODCALLTYPE Hooked_PSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* shader, ID3D11ClassInstance* const* instances, UINT n)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].ps = shader; Trace(self, "PSSetShader"); }
        Original<decltype(&Hooked_PSSetShader)>(self, kCtxPSSetShader)(self, shader, instances, n);
    }

    void STDMETHODCALLTYPE Hooked_IASetPrimitiveTopology(ID3D11DeviceContext* self, D3D11_PRIMITIVE_TOPOLOGY topology)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].topology = topology; }
        Original<decltype(&Hooked_IASetPrimitiveTopology)>(self, kCtxIASetPrimitiveTopology)(self, topology);
    }

    void STDMETHODCALLTYPE Hooked_PSSetShaderResources(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11ShaderResourceView* const* views)
    {
        // Title marker: the first bind of the 2048x1152 title art (checked by description until
        // seen; the texture is created through a path the creation hook does not see).
        if (!g_TitleSeen.load() && start == 0 && count > 0 && views && views[0])
        {
            ID3D11Resource* res = nullptr;
            views[0]->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
                {
                    D3D11_TEXTURE2D_DESC d {};
                    tex->GetDesc(&d);
                    tex->Release();
                    if (d.Width == 2048 && d.Height == 1152 && !g_TitleSeen.exchange(true)) { spdlog::info("PW census: TITLE SCREEN: the title art is being drawn (frame {}).", g_Frame.load()); }
                }
                res->Release();
            }
        }
        if (start == 0 && count > 0 && views && (g_FramesToLog.load() > 0 || UiBias::Active()))
        {
            std::lock_guard lock(g_Mutex);
            g_State[self].texture = DescribeTexture(views[0]);
            Trace(self, "SRV0=" + g_State[self].texture.substr(0, g_State[self].texture.find(' ')));
        }
        Original<decltype(&Hooked_PSSetShaderResources)>(self, kCtxPSSetShaderResources)(self, start, count, views);
    }

    void STDMETHODCALLTYPE Hooked_VSSetConstantBuffers(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11Buffer* const* buffers)
    {
        if (buffers)
        {
            std::lock_guard lock(g_Mutex);
            for (UINT i = 0; i < count && start + i < 4; i++) { g_State[self].vsCb[start + i] = buffers[i]; }
            Trace(self, std::format("VSSetCB{}x{}", start, count));
        }
        Original<decltype(&Hooked_VSSetConstantBuffers)>(self, kCtxVSSetConstantBuffers)(self, start, count, buffers);
    }

    HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
    {
        const HRESULT r = Original<decltype(&Hooked_Map)>(self, kCtxMap)(self, res, sub, type, flags, mapped);
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d {};
                tex->GetDesc(&d);
                tex->Release();
                if (d.Width >= 1024)
                {
                    g_TextureMaps.fetch_add(1);
                    if (FAILED(r)) { g_TextureMapFails.fetch_add(1); }
                    static std::atomic<int> logged { 0 };
                    if (g_FramesToLog.load() > 0 || (FAILED(r) && logged.fetch_add(1) < 10))
                    {
                        std::lock_guard lock(g_Mutex);
                        spdlog::info("PW census: f{} {} Map texture {}x{} fmt{} cpu={:#x} type={} -> {:#x} | {}", g_Frame.load(), HooksFor(self).name, d.Width, d.Height, static_cast<int>(d.Format), d.CPUAccessFlags, static_cast<int>(type), static_cast<uint32_t>(r), GameCallers(5));
                    }
                }
            }
        }
        if (SUCCEEDED(r) && mapped && mapped->pData && sub == 0 && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE || type == D3D11_MAP_WRITE))
        {
            std::lock_guard lock(g_Mutex);
            g_Mapped[res] = { mapped->pData, mapped->RowPitch };
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
                bool constant = false;
                const size_t size = BufferSize(res, constant);
                if (constant) { UiBias::OnConstantUnmap(it->second.data, size); Snapshot16(res, it->second.data, size); Trace(self, std::format("Unmap cb {}B", size)); }
                if (!constant && size) { UiBias::OnVertexUnmap(g_State[self].texture, it->second.data, size); }
                if (!constant && size && g_FramesToLog.load() > 0)
                {
                    Trace(self, std::format("Unmap vb {}B", size));
                    ContextState& st = g_State[self];
                    const size_t keep = std::min<size_t>(size, 64 * 24);
                    st.lastVertexUpload.assign(static_cast<const uint8_t*>(it->second.data), static_cast<const uint8_t*>(it->second.data) + keep);
                    st.lastVertexUploadSize = size;
                    st.lastVertexUploadCaller = GameCallers(3);
                }
                g_Mapped.erase(it);
            }
        }
        Original<decltype(&Hooked_Unmap)>(self, kCtxUnmap)(self, res, sub);
    }

    void STDMETHODCALLTYPE Hooked_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
    {
        if (res && data && sub == 0 && !box)
        {
            bool constant = false;
            const size_t size = BufferSize(res, constant);
            if (constant)
            {
                std::lock_guard lock(g_Mutex);
                Snapshot16(res, data, size);
                Trace(self, std::format("UpdateSub cb {}B", size));
            }
            else if (size) { std::lock_guard lock(g_Mutex); Trace(self, std::format("UpdateSub vb {}B", size)); }
        }
        Original<decltype(&Hooked_UpdateSubresource)>(self, kCtxUpdateSubresource)(self, res, sub, box, data, rowPitch, depthPitch);
    }

    void STDMETHODCALLTYPE Hooked_CSSetShaderResources(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11ShaderResourceView* const* views)
    {
        if (start == 0 && count > 0 && views && g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); g_State[self].csTexture = DescribeTexture(views[0]); }
        Original<decltype(&Hooked_CSSetShaderResources)>(self, kCtxCSSetShaderResources)(self, start, count, views);
    }

    void STDMETHODCALLTYPE Hooked_CSSetUnorderedAccessViews(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11UnorderedAccessView* const* views, const UINT* counts)
    {
        if (start == 0 && count > 0 && views && g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); g_State[self].csTarget = DescribeUav(views[0]); }
        Original<decltype(&Hooked_CSSetUnorderedAccessViews)>(self, kCtxCSSetUnorderedAccessViews)(self, start, count, views, counts);
    }

    void STDMETHODCALLTYPE Hooked_CSSetShader(ID3D11DeviceContext* self, ID3D11ComputeShader* shader, ID3D11ClassInstance* const* instances, UINT n)
    {
        { std::lock_guard lock(g_Mutex); g_State[self].cs = shader; }
        Original<decltype(&Hooked_CSSetShader)>(self, kCtxCSSetShader)(self, shader, instances, n);
    }

    void STDMETHODCALLTYPE Hooked_Dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z)
    {
        g_DrawsSeen.fetch_add(1);
        if (g_TimingState.load() == 2) { std::lock_guard lock(g_Mutex); g_State[self].stampNext = true; StampBefore(self, std::format("Dispatch {}x{}x{} srv0={} uav0={}", x, y, z, g_State[self].csTexture, g_State[self].csTarget)); }
        if (g_FramesToLog.load() > 0)
        {
            std::lock_guard lock(g_Mutex);
            ContextState& st = g_State[self];
            st.draws++;
            g_DrawsThisFrame++;
            const auto cs = g_ShaderHash.find(st.cs);
            spdlog::info("PW census: f{} {}#{} Dispatch {}x{}x{} cs={} srv0={} uav0={} | {}", g_Frame.load(), HooksFor(self).name, st.draws, x, y, z,
                cs == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", cs->second).substr(0, 8), st.csTexture, st.csTarget, GameCallers(6));
        }
        Original<decltype(&Hooked_Dispatch)>(self, kCtxDispatch)(self, x, y, z);
    }

    void STDMETHODCALLTYPE Hooked_DispatchIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset)
    {
        if (g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); spdlog::info("PW census: f{} DispatchIndirect | {}", g_Frame.load(), GameCallers(6)); }
        Original<decltype(&Hooked_DispatchIndirect)>(self, kCtxDispatchIndirect)(self, args, offset);
    }

    void STDMETHODCALLTYPE Hooked_CopySubresourceRegion(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
    {
        if (g_TimingState.load() == 2) { std::lock_guard lock(g_Mutex); StampBefore(self, std::format("CopySubresourceRegion dst={} src={}", DescribeResource(dst), DescribeResource(src))); }
        if (g_FramesToLog.load() > 0)
        {
            std::lock_guard lock(g_Mutex);
            spdlog::info("PW census: f{} {} CopySubresourceRegion dst={} at ({},{}) src={} box={} | {}", g_Frame.load(), HooksFor(self).name, DescribeResource(dst), x, y, DescribeResource(src),
                box ? std::format("({},{})-({},{})", box->left, box->top, box->right, box->bottom) : std::string("all"), GameCallers(6));
        }
        Original<decltype(&Hooked_CopySubresourceRegion)>(self, kCtxCopySubresourceRegion)(self, dst, dstSub, x, y, z, src, srcSub, box);
    }

    void STDMETHODCALLTYPE Hooked_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src)
    {
        if (g_TimingState.load() == 2) { std::lock_guard lock(g_Mutex); g_State[self].stampNext = true; StampBefore(self, std::format("CopyResource dst={} src={}", DescribeResource(dst), DescribeResource(src))); }
        if (g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); spdlog::info("PW census: f{} {} CopyResource dst={} src={} | {}", g_Frame.load(), HooksFor(self).name, DescribeResource(dst), DescribeResource(src), GameCallers(6)); }
        Original<decltype(&Hooked_CopyResource)>(self, kCtxCopyResource)(self, dst, src);
    }

    void STDMETHODCALLTYPE Hooked_ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT fmt)
    {
        if (g_TimingState.load() == 2) { std::lock_guard lock(g_Mutex); g_State[self].stampNext = true; StampBefore(self, std::format("Resolve/copy dst={} src={}", DescribeResource(dst), DescribeResource(src))); }
        if (src && dst && PostScale::Downscale(self, dst, src)) { return; }
        if (InternalSize::bDisableMsaa && src && dst)
        {
            // A single-sampled source cannot be resolved; copy it instead (same size and format).
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d {};
                tex->GetDesc(&d);
                tex->Release();
                if (d.SampleDesc.Count == 1)
                {
                    Original<decltype(&Hooked_CopyResource)>(self, kCtxCopyResource)(self, dst, src);
                    return;
                }
            }
        }
        if (g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); spdlog::info("PW census: f{} {} ResolveSubresource dst={} src={} | {}", g_Frame.load(), HooksFor(self).name, DescribeResource(dst), DescribeResource(src), GameCallers(6)); }
        Original<decltype(&Hooked_ResolveSubresource)>(self, kCtxResolveSubresource)(self, dst, dstSub, src, srcSub, fmt);
    }

    void STDMETHODCALLTYPE Hooked_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore)
    {
        g_ListsSeen.fetch_add(1);
        const bool timing = g_TimingState.load() == 2;
        if (timing) { std::lock_guard lock(g_Mutex); StampBefore(self, std::format("list {:#x} begins", reinterpret_cast<uintptr_t>(list) & 0xffffff)); }
        if (g_FramesToLog.load() > 0) { spdlog::info("PW census: f{} ExecuteCommandList {:#x} | {}", g_Frame.load(), reinterpret_cast<uintptr_t>(list) & 0xffffff, GameCallers(4)); }
        Original<decltype(&Hooked_ExecuteCommandList)>(self, kCtxExecuteCommandList)(self, list, restore);
        if (timing) { std::lock_guard lock(g_Mutex); StampBefore(self, std::format("list {:#x} ended", reinterpret_cast<uintptr_t>(list) & 0xffffff)); }
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
            { kCtxPSSetShaderResources, reinterpret_cast<void*>(Hooked_PSSetShaderResources) },
            { kCtxMap, reinterpret_cast<void*>(Hooked_Map) },
            { kCtxUnmap, reinterpret_cast<void*>(Hooked_Unmap) },
            { kCtxUpdateSubresource, reinterpret_cast<void*>(Hooked_UpdateSubresource) },
            { kCtxIASetPrimitiveTopology, reinterpret_cast<void*>(Hooked_IASetPrimitiveTopology) },
            { kCtxExecuteCommandList, reinterpret_cast<void*>(Hooked_ExecuteCommandList) },
            { kCtxDispatch, reinterpret_cast<void*>(Hooked_Dispatch) },
            { kCtxDispatchIndirect, reinterpret_cast<void*>(Hooked_DispatchIndirect) },
            { kCtxCopySubresourceRegion, reinterpret_cast<void*>(Hooked_CopySubresourceRegion) },
            { kCtxCopyResource, reinterpret_cast<void*>(Hooked_CopyResource) },
            { kCtxResolveSubresource, reinterpret_cast<void*>(Hooked_ResolveSubresource) },
            { kCtxCSSetShaderResources, reinterpret_cast<void*>(Hooked_CSSetShaderResources) },
            { kCtxCSSetUnorderedAccessViews, reinterpret_cast<void*>(Hooked_CSSetUnorderedAccessViews) },
            { kCtxCSSetShader, reinterpret_cast<void*>(Hooked_CSSetShader) },
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
            // Light Hooks: only the resolve (for Disable MSAA and the post-scale draw); no
            // per-draw hooks at all, so the game's own CPU cost can be measured.
            if (DrawCensus::bLightHooks && sl.index != kCtxResolveSubresource) { continue; }
            void* current = vtable[sl.index];
            // Already our function: this class shares the entry with the immediate's table
            // (never seen, the tables are separate, but cheap to guard).
            if (current == sl.target) { h.original[sl.index] = g_Immediate.original[sl.index]; shared += std::format(" {}", sl.index); continue; }
            h.original[sl.index] = current;
            vtable[sl.index] = sl.target;
        }
        VirtualProtect(vtable, kSlotCount * sizeof(void*), old, &old);
        spdlog::info("PW census: {} context vtable at {:#x} patched ({} entries{}){}.", name, reinterpret_cast<uintptr_t>(vtable), std::size(slots), DrawCensus::bLightHooks ? ", light: resolve only" : "",
            shared.empty() ? "" : "; entries already ours:" + shared);
    }

    // ---- device hooks -------------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Hooked_CreateVertexShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out)
    {
        const HRESULT r = Device_CreateVertexShader_hook.stdcall<HRESULT>(self, code, size, linkage, out);
        if (SUCCEEDED(r) && out && *out && code) { std::lock_guard lock(g_Mutex); g_ShaderHash[*out] = Fnv1a(code, size); DumpShader(code, size, g_ShaderHash[*out], "vs"); }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreatePixelShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
    {
        const HRESULT r = Device_CreatePixelShader_hook.stdcall<HRESULT>(self, code, size, linkage, out);
        if (SUCCEEDED(r) && out && *out && code) { std::lock_guard lock(g_Mutex); g_ShaderHash[*out] = Fnv1a(code, size); DumpShader(code, size, g_ShaderHash[*out], "ps"); }
        return r;
    }

    // Render targets and depth buffers as they are created: size, format, samples, and who asked.
    HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
    {
        D3D11_TEXTURE2D_DESC single {};
        if (desc && ((InternalSize::bDisableMsaa && desc->SampleDesc.Count > 1) || (InternalSize::bGpuLocalTextures && desc->Usage == D3D11_USAGE_DEFAULT && desc->CPUAccessFlags && desc->Width >= 1024)))
        {
            single = *desc;
            if (InternalSize::bDisableMsaa && single.SampleDesc.Count > 1) { single.SampleDesc.Count = 1; single.SampleDesc.Quality = 0; }
            if (InternalSize::bGpuLocalTextures && single.Usage == D3D11_USAGE_DEFAULT && single.CPUAccessFlags && single.Width >= 1024)
            {
                single.CPUAccessFlags = 0;
                g_GpuLocalStripped.fetch_add(1);
            }
            desc = &single;
        }
        const HRESULT r = Device_CreateTexture2D_hook.stdcall<HRESULT>(self, desc, init, out);
        if (SUCCEEDED(r) && desc && (((desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)) && desc->Width >= 256) || desc->Width >= 1024))
        {
            spdlog::info("PW census: texture {}x{} fmt{} samples={} bind={:#x} usage={} cpu={:#x} misc={:#x} mips={} {} | {}", desc->Width, desc->Height, static_cast<int>(desc->Format), desc->SampleDesc.Count, desc->BindFlags,
                static_cast<int>(desc->Usage), desc->CPUAccessFlags, desc->MiscFlags, desc->MipLevels,
                out && *out ? std::format("{:#x}", reinterpret_cast<uintptr_t>(*out) & 0xffffff) : std::string("-"), GameCallers(4));
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateComputeShader(ID3D11Device* self, const void* code, SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11ComputeShader** out)
    {
        const HRESULT r = Device_CreateComputeShader_hook.stdcall<HRESULT>(self, code, size, linkage, out);
        if (SUCCEEDED(r) && out && *out && code) { std::lock_guard lock(g_Mutex); g_ShaderHash[*out] = Fnv1a(code, size); DumpShader(code, size, g_ShaderHash[*out], "cs"); }
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
            // Timing: the census's second frame (the one that carries the draws) is timed.
            {
                std::lock_guard lock(g_Mutex);
                const int ts = g_TimingState.load();
                if (ts == 2)
                {
                    StampBefore(g_ImmediateCtx, "Present");
                    g_ImmediateCtx->End(g_Disjoint);
                    g_TimingState.store(3);
                }
                else if (ts == 1 && g_ImmediateCtx && g_Disjoint)
                {
                    g_ImmediateCtx->Begin(g_Disjoint);
                    g_TimingFrame = g_Frame.load() + 1;
                    g_TimingState.store(2);
                    StampBefore(g_ImmediateCtx, "frame start");
                }
                else if (ts >= 3 && ts < 6) { g_TimingState.store(ts + 1); }
            }
            if (g_TimingState.load() == 6) { ReportTiming(); }
            const int left = g_FramesToLog.load();
            if (left == 2 && DrawCensus::bTimePasses && g_ImmediateCtx && g_TimingState.load() == 0) { EnsureStampPool(); if (!g_StampPool.empty()) { g_TimingFramesLeft = std::max(1, DrawCensus::iTimeFrames); g_TimingWallStart = std::chrono::steady_clock::now(); g_TimingFrameStart = g_Frame.load(); g_TimingState.store(1); } }
            if (left > 0)
            {
                spdlog::info("PW census: frame {} ended with {} draw(s) on all contexts; biases applied so far {} vertex, {} translation.", g_Frame.load(), g_DrawsThisFrame, UiBias::AppliedVertex(), UiBias::AppliedTranslation());
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
        if (desc && InternalSize::BackBufferWidth() > 0)
        {
            desc->BufferDesc.Width = InternalSize::BackBufferWidth();
            desc->BufferDesc.Height = InternalSize::BackBufferHeight();
            spdlog::info("PW census: swap chain buffers requested at {}x{} (flip model stretches them onto the window).", desc->BufferDesc.Width, desc->BufferDesc.Height);
        }
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
        Device_CreateComputeShader_hook = safetyhook::create_inline(dv[kDevCreateComputeShader], reinterpret_cast<void*>(Hooked_CreateComputeShader));
        Device_CreateTexture2D_hook = safetyhook::create_inline(dv[kDevCreateTexture2D], reinterpret_cast<void*>(Hooked_CreateTexture2D));

        ID3D11DeviceContext* immediate = context;
        if (!immediate) { device->GetImmediateContext(&immediate); }
        if (!immediate) { spdlog::error("PW census: no immediate context."); return; }
        g_Device = device;
        PostScale::SetDevice(device);
        g_ImmediateCtx = immediate;
        immediate->AddRef();
        InstallContextHooks(g_Immediate, *reinterpret_cast<void***>(immediate), "immediate");
        if (!context) { /* keep the reference taken above */ }

        // Draw rate for the first half minute, logged or not: which path the draws take.
        std::thread([]
        {
            uint64_t lastDraws = 0, lastLists = 0, lastFrames = 0;
            for (int i = 0; i < 30; i++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const uint64_t d = g_DrawsSeen.load(), l = g_ListsSeen.load(), f = g_Frame.load();
                spdlog::info("PW census: rate: {} draw(s), {} command list(s), {} frame(s) in the last second; large-texture maps so far {} ({} failed), CPU access stripped on {} textures.", d - lastDraws, l - lastLists, f - lastFrames, g_TextureMaps.load(), g_TextureMapFails.load(), g_GpuLocalStripped.load());
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
        else if (UiBias::Command(line)) {}
        else if (!cmd.empty())
        {
            spdlog::warn("PW live: unknown command '{}'.", line);
        }
    }

    void PollThread()
    {
        bool f11Down = false;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // F11: a two-frame census and the timing run, from now.
            const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (down && !f11Down)
            {
                spdlog::info("PW live: F11 pressed: census and timing start now (frame {}).", g_Frame.load());
                RunCommand("census 2");
            }
            f11Down = down;
            static int tick = 0;
            if (++tick % 5) { continue; }   // the command file every 250 ms
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
