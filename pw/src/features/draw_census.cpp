#include "pch.hpp"
#include <set>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <dwmapi.h>
#include <mutex>
#include <intrin.h>
#include "draw_census.hpp"

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "ui_bias.hpp"
#include "internal_size.hpp"
#include "render_policy.hpp"
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
    constexpr size_t kDevCreateSamplerState = 23;
    constexpr size_t kDevCreateComputeShader = 18;
    constexpr size_t kDevCreateDeferredContext = 27;
    // ID3D11DeviceContext vtable slots.
    constexpr size_t kCtxVSSetConstantBuffers = 7;
    constexpr size_t kCtxPSSetShaderResources = 8;
    constexpr size_t kCtxPSSetShader = 9;
    constexpr size_t kCtxPSSetSamplers = 10;
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
    constexpr size_t kCtxFlush = 111;
    constexpr size_t kCtxFinishCommandList = 114;
    constexpr size_t kCtxCSSetShaderResources = 67;
    constexpr size_t kCtxCSSetUnorderedAccessViews = 68;
    constexpr size_t kCtxCSSetShader = 69;
    // IDXGIFactory / IDXGIFactory2 / IDXGISwapChain slots.
    constexpr size_t kFactoryCreateSwapChain = 10;
    constexpr size_t kFactory2CreateSwapChainForHwnd = 15;
    constexpr size_t kSwapChainPresent = 8;
    constexpr size_t kSwapChainResizeBuffers = 13;
    constexpr UINT kSwapChainFlagAllowTearing = 0x800;   // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
    constexpr UINT kPresentAllowTearing = 0x200;         // DXGI_PRESENT_ALLOW_TEARING
    SafetyHookInline SwapChain_ResizeBuffers_hook {};
    std::atomic<bool> g_TearingChain { false };          // the chain was created with the flag
    std::atomic<uint32_t> g_TearingPresents { 0 };

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
        std::string sampler = "-";      // pixel shader sampler slot 0 (filter/anisotropy, as the game created it)
        std::string csTexture = "-";    // compute shader resource slot 0
        std::string csTarget = "-";     // compute unordered access view slot 0
        void* cs = nullptr;
        std::vector<uint8_t> lastVertexUpload;   // head of the last non-constant buffer unmapped on this context
        size_t lastVertexUploadSize = 0;
        std::string lastVertexUploadCaller;
        std::string trace;              // call order on this context since its last draw (first draws of a census frame only)
        std::string lastStampedTarget;  // timing: the target of the last stamped draw on this context
        bool stampNext = false;         // timing: the op after a stamped op gets a closing stamp, so each pass is bounded
        bool uiUpload = false;          // the last constant upload on this context was a UI one (the private module says)
        int draws = 0;   // this frame (reset at Present for the immediate; at ExecuteCommandList for a deferred)
    };
    std::mutex g_Mutex;
    std::unordered_map<void*, ContextState> g_State;
    void STDMETHODCALLTYPE Hooked_PSSetSamplers(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11SamplerState* const* samplers);
    std::atomic<int> g_FramesToLog { 0 };
    std::atomic<uint64_t> g_Frame { 0 };

    // Hitch log: every frame's present interval against a running average; a frame that takes
    // twice the average (and over 25 ms) is logged with what happened inside it, so a regular
    // stutter can be attributed (resource creation, maps that wait on the GPU, the present
    // itself, our own log lines).
    struct FrameWork
    {
        std::atomic<uint32_t> textures { 0 }, texturesLarge { 0 }, buffers { 0 }, maps { 0 }, mapWaits { 0 }, logLines { 0 };
        std::atomic<uint64_t> textureTicks { 0 }, mapTicks { 0 }, bufferTicks { 0 };
        std::string lastTexture;   // under g_Mutex
        // Frame pacing: time the game spends in Sleep and WaitForSingleObject(Ex) this frame,
        // with the last caller of each (IAT hooks on the game's own imports, Lab only).
        std::atomic<uint32_t> sleeps { 0 }, waits { 0 };
        std::atomic<uint64_t> sleepTicks { 0 }, waitTicks { 0 };
        std::atomic<uintptr_t> sleepCaller { 0 }, waitCaller { 0 };
        void Reset() { textures = 0; texturesLarge = 0; buffers = 0; maps = 0; mapWaits = 0; logLines = 0; textureTicks = 0; mapTicks = 0; bufferTicks = 0; sleeps = 0; waits = 0; sleepTicks = 0; waitTicks = 0; }
    };
    FrameWork g_Work;
    // Pacing summary: totals over a window of frames, printed every few seconds.
    struct PacingWindow { uint32_t frames = 0; int64_t start = 0, sleep = 0, wait = 0, present = 0; double maxFrame = 0; uint32_t over25 = 0; } g_Pacing;
    std::atomic<uint32_t> g_PresentKinds { 0 };
    std::atomic<DWORD> g_PresentThread { 0 };
    struct CallerStat { uint32_t calls = 0; int64_t ticks = 0; uint32_t maxArg = 0; };
    std::map<uintptr_t, CallerStat> g_MainSleepers, g_MainWaiters;   // under g_PacingMutex
    std::mutex g_PacingMutex;
    std::atomic<uint64_t> g_OtherThreadWaits { 0 };
    int64_t Ticks();
    double TicksToMs(int64_t t);
    void* PatchImport(const char* dll, const char* name, void* replacement);
    // ---- the game's 60 Hz ticker ------------------------------------------------------------
    // Thread +75FD0 (started at +775B9): QPC start; loop { elapsed = (QPC - start) / freq; while
    // period > elapsed: timeBeginPeriod(1), Sleep((period - elapsed) * 1000), timeEndPeriod(1);
    // tick (the callback at [+17D20]); start += freq * period }. The period is the constant 1/60
    // loaded into xmm7 once before the loop, so the game ticks at exactly 60.000 Hz by the CPU
    // clock while the display runs at its own rate: the beat is one duplicated frame each time
    // the two drift a whole frame apart (about once a second on a 59 Hz panel, every 16 s at
    // 59.94). Frame Pacing = Display sets xmm7 each iteration to k / refresh, k = round(refresh
    // / 60), so the ticker and the display run at the same rate.
    constexpr uintptr_t kTickerCompare = 0x7604B;   // comisd xmm7, xmm6 (period vs elapsed)
    constexpr uintptr_t kTickerTick = 0x76080;      // call +17D20 (once per tick)
    SafetyHookMid g_TickerCompare {}, g_TickerTick {};
    std::atomic<uint64_t> g_Ticks { 0 };
    double g_TickPeriod = 1.0 / 60.0;
    double g_DisplayRefresh = 0;
    // Frame Pacing = VBlank: the ticker thread waits for the display's vertical blank (k of them
    // on a k x 60 Hz display) instead of sleeping to the CPU clock, so the game thread's tick
    // and the display never drift apart in phase. The output comes from the swap chain once it
    // exists; until then the game's own wait runs.
    IDXGIOutput* g_VBlankOutput = nullptr;   // referenced
    std::atomic<int> g_VBlankPerTick { 0 };
    std::atomic<uint64_t> g_VBlankWaits { 0 };
    std::atomic<uint64_t> g_VBlankLong { 0 }, g_VBlankShort { 0 }, g_VBlankWaitTicks { 0 }, g_VBlankWaitMax { 0 };
    int64_t g_LastVBlankTick = 0;   // ticker thread only
    // The primary display's exact vertical sync frequency from the display configuration (the
    // target's video signal, a ratio such as 60000/1001), when the path can be read.
    double PrimaryVSyncFrequency()
    {
        UINT32 nPaths = 0, nModes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS || !nPaths) { return 0; }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) != ERROR_SUCCESS) { return 0; }
        double first = 0;
        for (UINT32 i = 0; i < nPaths; i++)
        {
            const auto& path = paths[i];
            const UINT32 t = path.targetInfo.modeInfoIdx, src = path.sourceInfo.modeInfoIdx;
            if (t >= nModes || modes[t].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) { continue; }
            const auto& v = modes[t].targetMode.targetVideoSignalInfo.vSyncFreq;
            if (!v.Numerator || !v.Denominator) { continue; }
            const double hz = static_cast<double>(v.Numerator) / v.Denominator;
            if (!first) { first = hz; }
            if (src < nModes && modes[src].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE && modes[src].sourceMode.position.x == 0 && modes[src].sourceMode.position.y == 0) { return hz; }
        }
        return first;
    }
    double MeasureDisplayRefresh()
    {
        if (const double hz = PrimaryVSyncFrequency(); hz > 1) { return hz; }
        // The compositor's refresh rate (the primary display's real rate as a ratio).
        using Fn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
        if (const HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        {
            if (const auto fn = reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo")))
            {
                DWM_TIMING_INFO info {};
                info.cbSize = sizeof(info);
                if (SUCCEEDED(fn(nullptr, &info)) && info.rateRefresh.uiDenominator > 0 && info.rateRefresh.uiNumerator > 0)
                {
                    return static_cast<double>(info.rateRefresh.uiNumerator) / info.rateRefresh.uiDenominator;
                }
            }
        }
        DEVMODEW dm {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) { return dm.dmDisplayFrequency; }
        return 0;
    }
    void InstallTickerHooks()
    {
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uint8_t* c = reinterpret_cast<const uint8_t*>(base + kTickerCompare);
        const uint8_t* k = reinterpret_cast<const uint8_t*>(base + kTickerTick);
        if (!(c[0] == 0x66 && c[1] == 0x0F && c[2] == 0x2F && c[3] == 0xFE && k[0] == 0xE8))
        {
            spdlog::warn("PW pacing: ticker sites +{:X}/+{:X} do not look as expected; not hooked.", kTickerCompare, kTickerTick);
            return;
        }
        g_DisplayRefresh = MeasureDisplayRefresh();
        if (DrawCensus::bPacingLog) { g_TickerTick = safetyhook::create_mid(base + kTickerTick, [](SafetyHookContext&) { g_Ticks.fetch_add(1); }); }
        if (DrawCensus::iFramePacing == 2 && g_DisplayRefresh > 1)
        {
            const int k = std::max(1, static_cast<int>(std::lround(g_DisplayRefresh / 60.0)));
            const bool multiple = std::abs(g_DisplayRefresh - 60.0 * k) < 1.5;
            if (multiple)
            {
                g_VBlankPerTick.store(k);
                g_TickerCompare = safetyhook::create_mid(base + kTickerCompare, [](SafetyHookContext& ctx)
                {
                    IDXGIOutput* out = g_VBlankOutput;
                    const int k = g_VBlankPerTick.load();
                    if (!out || k <= 0) { return; }   // no chain yet: the game's own wait
                    const int64_t t0 = Ticks();
                    for (int i = 0; i < k; i++) { if (FAILED(out->WaitForVBlank())) { return; } }
                    const int64_t t1 = Ticks();
                    g_VBlankWaits.fetch_add(1);
                    g_VBlankWaitTicks.fetch_add(t1 - t0);
                    { uint64_t m = g_VBlankWaitMax.load(); while (static_cast<uint64_t>(t1 - t0) > m && !g_VBlankWaitMax.compare_exchange_weak(m, static_cast<uint64_t>(t1 - t0))) {} }
                    if (g_LastVBlankTick)
                    {
                        const double ms = TicksToMs(t1 - g_LastVBlankTick);
                        if (ms > 25.0 * k) { g_VBlankLong.fetch_add(1); } else if (ms < 8.0 * k) { g_VBlankShort.fetch_add(1); }
                    }
                    g_LastVBlankTick = t1;
                    ctx.xmm7.f64[0] = 0.0;   // period 0: no sleep, tick now
                });
                spdlog::info("PW pacing: ticker locked to the display's vblank: {:.4f} Hz, {} vblank(s) a tick ({}).", g_DisplayRefresh, k, g_TickerCompare ? "hooked" : "hook FAILED");
            }
            else { spdlog::info("PW pacing: display at {:.4f} Hz is not a multiple of 60; the ticker stays the game's.", g_DisplayRefresh); }
        }
        else if (DrawCensus::iFramePacing == 1 && g_DisplayRefresh > 1)
        {
            const int k = std::max(1, static_cast<int>(std::lround(g_DisplayRefresh / 60.0)));
            g_TickPeriod = k / g_DisplayRefresh;
            g_TickerCompare = safetyhook::create_mid(base + kTickerCompare, [](SafetyHookContext& ctx) { ctx.xmm7.f64[0] = g_TickPeriod; });
            spdlog::info("PW pacing: ticker paced to the display: {:.4f} Hz / {} = {:.4f} ms a tick ({}).", g_DisplayRefresh, k, g_TickPeriod * 1000.0, g_TickerCompare ? "hooked" : "hook FAILED");
        }
        else
        {
            spdlog::info("PW pacing: ticker left at the game's 60.000 Hz; the display reports {:.4f} Hz (tick counter {}).", g_DisplayRefresh, g_TickerTick ? "hooked" : "hook FAILED");
        }
    }
    UINT g_PresentSync = 0xFFFF, g_PresentFlags = 0xFFFF;
    int64_t g_LastPresentEnd = 0, g_LastPresentTicks = 0;
    double g_AvgFrameMs = 0;
    uint32_t g_Hitches = 0;
    int64_t Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    double TicksToMs(int64_t t) { static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }(); return static_cast<double>(t) * f; }
    // ---- frame pacing: the game's waits -----------------------------------------------------
    using SleepFn = void(WINAPI*)(DWORD);
    using WaitFn = DWORD(WINAPI*)(HANDLE, DWORD);
    using WaitExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
    SleepFn g_RealSleep = nullptr; WaitFn g_RealWait = nullptr; WaitExFn g_RealWaitEx = nullptr;
    uintptr_t CallerRva(void* ret)
    {
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const auto r = reinterpret_cast<uintptr_t>(ret);
        return (r > base && r - base < 0x2000000) ? r - base : 0;
    }
    bool OnPresentThread() { const DWORD t = g_PresentThread.load(); return t != 0 && t == GetCurrentThreadId(); }

    // ---- hitch sampler --------------------------------------------------------------------------
    // Every thread that enters a real wait (Sleep >= 1 ms, WaitForSingleObject) records what it
    // waits in; when the render thread has spun 20 ms for the game thread's frame, every other
    // thread is suspended, its instruction pointer and the game-code return addresses on its
    // stack read, and resumed; one log block per hitch, a few per run.
    struct ThreadWait { DWORD tid = 0; std::atomic<uintptr_t> caller { 0 }; std::atomic<int64_t> since { 0 }; std::atomic<uint32_t> ms { 0 }; };
    thread_local ThreadWait* t_Wait = nullptr;
    std::mutex g_WaitTableMutex;
    std::vector<ThreadWait*> g_WaitTable;
    ThreadWait& MyWait()
    {
        if (!t_Wait)
        {
            t_Wait = new ThreadWait;
            t_Wait->tid = GetCurrentThreadId();
            std::lock_guard lock(g_WaitTableMutex);
            g_WaitTable.push_back(t_Wait);
        }
        return *t_Wait;
    }
    struct ModuleRange { uintptr_t base, end; std::string name; };
    std::vector<ModuleRange> g_Modules;   // snapshot taken before the first sample, no API calls while threads are suspended
    void SnapshotModules()
    {
        g_Modules.clear();
        HMODULE mods[512];
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) { return; }
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; i++)
        {
            MODULEINFO mi {};
            if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) { continue; }
            wchar_t path[MAX_PATH] {};
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            std::string name;
            for (const wchar_t* c = path; *c; c++) { name += static_cast<char>(*c); }
            if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) { name = name.substr(slash + 1); }
            g_Modules.push_back({ reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), reinterpret_cast<uintptr_t>(mi.lpBaseOfDll) + mi.SizeOfImage, name });
        }
    }
    std::string Where(uintptr_t a)
    {
        for (const auto& m : g_Modules) { if (a >= m.base && a < m.end) { return fmt::format("{}+{:X}", m.name, a - m.base); } }
        return fmt::format("{:#x}", a);
    }
    std::atomic<int> g_SampleBudget { 0 };   // armed by the live command "sample N" (start-up and loading waits would spend a fixed budget)
    std::atomic<uint32_t> g_SpinHitches { 0 };
    void SampleThreads(double spunMs)
    {
        if (g_SampleBudget.load() <= 0) { return; }
        g_SampleBudget.fetch_sub(1);
        if (g_Modules.empty()) { SnapshotModules(); }
        struct Row { DWORD tid; uintptr_t rip; std::vector<uintptr_t> stack; };
        auto inModule = [](uintptr_t a) { for (const auto& m : g_Modules) { if (a >= m.base && a < m.end) { return true; } } return false; };
        std::vector<Row> rows;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) { return; }
        THREADENTRY32 te {};
        te.dwSize = sizeof(te);
        const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) { continue; }
            const HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) { continue; }
            Row row { te.th32ThreadID, 0, {} };
            if (SuspendThread(h) != static_cast<DWORD>(-1))
            {
                CONTEXT ctx {};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx))
                {
                    row.rip = ctx.Rip;
                    // The first 256 stack slots that point into the game's code: the call chain, roughly.
                    const auto* sp = reinterpret_cast<const uintptr_t*>(ctx.Rsp);
                    // The first 512 stack slots that point into any loaded module: the call chain, roughly
                    // (return addresses and stale frames alike).
                    for (int i = 0; i < 512 && row.stack.size() < 12; i++)
                    {
                        if (!mgs4e::mem::Readable(sp + i, sizeof(uintptr_t))) { break; }
                        const uintptr_t v = sp[i];
                        if (inModule(v)) { row.stack.push_back(v); }
                    }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            rows.push_back(std::move(row));
        }
        CloseHandle(snap);
        const int64_t now = Ticks();
        spdlog::warn("PW sampler: render thread has waited {:.1f} ms for a frame (frame {}); {} other threads:", spunMs, g_Frame.load(), rows.size());
        std::lock_guard lock(g_WaitTableMutex);
        for (const Row& r : rows)
        {
            std::string wait;
            for (const ThreadWait* w : g_WaitTable)
            {
                if (w->tid == r.tid && w->since.load()) { wait = fmt::format(" | in wait from +{:X} for {:.1f} ms (asked {})", w->caller.load(), TicksToMs(now - w->since.load()), w->ms.load() == INFINITE ? std::string("inf") : std::to_string(w->ms.load())); }
            }
            std::string chain;
            for (uintptr_t a : r.stack) { chain += " " + Where(a); }
            spdlog::warn("PW sampler:   tid {:5}: at {}{}{}", r.tid, Where(r.rip), chain.empty() ? "" : " | stack:" + chain, wait);
        }
    }
    // ---- the vblank wait ----------------------------------------------------------------------
    // The game's vblank handshake: the ticker callback (+78A10) increments a counter (+1084498)
    // and sets a manual-reset event (handle at +1083E90); a waiter (+78250, and a second loop at
    // +78350 on the same event) reads the counter, waits on the event with no timeout through
    // the wrapper +14AD0, resets the event, and re-checks the counter. Two threads reset one
    // event: when one wakes and resets it between the other's counter read and its wait, the
    // other sleeps through the tick it wanted, a whole frame (sampled 2026-09-14: at every hitch
    // every thread was in a wait). A short timeout on that one wait bounds the loss to the
    // timeout; the loop's counter check does the rest.
    constexpr uintptr_t kWaitEventWrapper = 0x14AD0;   // bool WaitEvent(HANDLE* slot, DWORD ms /* 0 = INFINITE */)
    constexpr uintptr_t kVBlankEventSlot = 0x1083E90;
    SafetyHookInline g_WaitEvent {};
    std::atomic<uint64_t> g_VBlankTimeouts { 0 }, g_VBlankWaits2 { 0 };
    // The vblank wait log: per thread, the counter value and time when its last vblank wait
    // returned; at the next wait the work time in between and the ticks passed are known.
    constexpr uintptr_t kVBlankCounter = 0x1084498;
    struct VBlankTrack { uintptr_t lastCaller = 0; uint32_t lastCounter = 0; int64_t lastReturn = 0; uint32_t waits = 0, skipped = 0; double workMax = 0, workSum = 0; int64_t windowStart = 0; double csWindowSum = 0, csWindowMax = 0; };
    thread_local VBlankTrack t_VBlank;
    // Critical sections entered by this thread since its last vblank wait returned: total time
    // blocked, the longest single entry and which section it was (the IAT hook below).
    struct CsTrack { int64_t ticks = 0, maxTicks = 0; uintptr_t maxCs = 0, maxCaller = 0; uint32_t entries = 0; };
    thread_local CsTrack t_Cs;
    struct D3DAcc { int64_t ticks = 0, maxTicks = 0; const char* maxWhat = ""; uint32_t calls = 0; };
    thread_local D3DAcc t_D3D;   // time inside the hooked D3D calls on this thread since its last vblank wait returned
    void AccountD3D(int64_t dt, const char* what) { t_D3D.ticks += dt; t_D3D.calls++; if (dt > t_D3D.maxTicks) { t_D3D.maxTicks = dt; t_D3D.maxWhat = what; } }
    struct WaitAcc { int64_t ticks = 0, maxTicks = 0; uintptr_t maxCaller = 0; uint32_t maxMs = 0, count = 0; };
    thread_local WaitAcc t_WaitAcc;   // Sleep(>=1) and WaitForSingleObject(Ex) on this thread since its last vblank wait returned
    void AccountWait(int64_t dt, uintptr_t caller, DWORD ms) { t_WaitAcc.ticks += dt; t_WaitAcc.count++; if (dt > t_WaitAcc.maxTicks) { t_WaitAcc.maxTicks = dt; t_WaitAcc.maxCaller = caller; t_WaitAcc.maxMs = ms; } }
    using EnterCsFn = void(WINAPI*)(LPCRITICAL_SECTION);
    EnterCsFn g_RealEnterCs = nullptr;
    void WINAPI Hooked_EnterCriticalSection(LPCRITICAL_SECTION cs)
    {
        if (!TryEnterCriticalSection(cs))
        {
            const int64_t t0 = Ticks();
            g_RealEnterCs(cs);
            const int64_t dt = Ticks() - t0;
            t_Cs.ticks += dt; t_Cs.entries++;
            if (dt > t_Cs.maxTicks) { t_Cs.maxTicks = dt; t_Cs.maxCs = reinterpret_cast<uintptr_t>(cs); t_Cs.maxCaller = CallerRva(_ReturnAddress()); }
        }
    }
    std::atomic<int> g_VBlankSkipBudget { 0 };   // per-skip lines, armed by the live command "skips N" (the intro's 30 fps would spend a fixed budget)
    uint8_t WINAPI Hooked_WaitEvent(HANDLE* slot, DWORD ms)
    {
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        DWORD timeout = ms ? ms : INFINITE;
        const bool vblank = reinterpret_cast<uintptr_t>(slot) == base + kVBlankEventSlot;
        if (vblank && ms == 0 && DrawCensus::iVBlankWaitTimeout > 0) { timeout = static_cast<DWORD>(DrawCensus::iVBlankWaitTimeout); g_VBlankWaits2.fetch_add(1); }
        const bool logging = vblank && DrawCensus::bVBlankWaitLog;
        const uintptr_t caller = logging ? CallerRva(_ReturnAddress()) : 0;
        int64_t enter = 0;
        double work = 0;
        if (logging)
        {
            enter = Ticks();
            if (t_VBlank.lastReturn) { work = TicksToMs(enter - t_VBlank.lastReturn); }
        }
        const DWORD r = WaitForSingleObject(*slot, timeout);
        if (vblank && r == WAIT_TIMEOUT) { g_VBlankTimeouts.fetch_add(1); }
        if (logging)
        {
            const int64_t now = Ticks();
            // The value is the vblanks since the game thread last consumed one: 1 on a normal tick,
            // 2 or more when the thread was late for a tick (the game then runs the logic twice).
            const uint32_t counter = *reinterpret_cast<volatile uint32_t*>(base + kVBlankCounter);
            const uint32_t passed = counter;
            if (t_VBlank.lastReturn)
            {
                t_VBlank.waits++; t_VBlank.workSum += work; t_VBlank.workMax = std::max(t_VBlank.workMax, work);
                const double csMs = TicksToMs(t_Cs.ticks);
                t_VBlank.csWindowSum += csMs; t_VBlank.csWindowMax = std::max(t_VBlank.csWindowMax, csMs);
                if (passed >= 2)
                {
                    t_VBlank.skipped++;
                    if (g_VBlankSkipBudget.load() > 0 && g_VBlankSkipBudget.fetch_sub(1) > 0)
                    {
                        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
                        spdlog::warn("PW vblank: tid {} missed {} tick(s): work since the last wait returned {:.2f} ms, of it {:.2f} ms in {} wait(s) (longest {:.2f} ms from +{:X}, asked {}) and {:.2f} ms in critical sections, {:.2f} ms in {} D3D call(s) (longest {:.2f} ms in {}); this vblank wait {:.2f} ms from +{:X} (previous wait from +{:X}), vblanks consumed {}, wait count {} / frame count {}.",
                            GetCurrentThreadId(), passed - 1, work, TicksToMs(t_WaitAcc.ticks), t_WaitAcc.count, TicksToMs(t_WaitAcc.maxTicks), t_WaitAcc.maxCaller, t_WaitAcc.maxMs == INFINITE ? std::string("inf") : std::to_string(t_WaitAcc.maxMs), csMs,
                            TicksToMs(t_D3D.ticks), t_D3D.calls, TicksToMs(t_D3D.maxTicks), t_D3D.maxWhat, TicksToMs(now - enter), caller, t_VBlank.lastCaller, counter,
                            *reinterpret_cast<volatile int32_t*>(base + 0x1084484), *reinterpret_cast<volatile int32_t*>(base + 0x1084490));
                    }
                }
                t_Cs = CsTrack {};
                t_WaitAcc = WaitAcc {};
                t_D3D = D3DAcc {};
            }
            if (!t_VBlank.windowStart) { t_VBlank.windowStart = now; }
            if (TicksToMs(now - t_VBlank.windowStart) >= 5000.0 && t_VBlank.waits)
            {
                spdlog::info("PW vblank: tid {}: {} waits in 5 s, {} missed tick(s); work between waits mean {:.2f} ms, max {:.2f} ms; blocked in critical sections mean {:.2f} ms, max {:.2f} ms.", GetCurrentThreadId(), t_VBlank.waits, t_VBlank.skipped, t_VBlank.workSum / t_VBlank.waits, t_VBlank.workMax, t_VBlank.csWindowSum / t_VBlank.waits, t_VBlank.csWindowMax);
                t_VBlank.waits = 0; t_VBlank.skipped = 0; t_VBlank.workSum = 0; t_VBlank.workMax = 0; t_VBlank.csWindowSum = 0; t_VBlank.csWindowMax = 0; t_VBlank.windowStart = now;
            }
            t_VBlank.lastCounter = counter;
            t_VBlank.lastReturn = now;
            t_VBlank.lastCaller = caller;
        }
        return r == WAIT_OBJECT_0 ? 1 : 0;
    }
    void InstallVBlankWaitHook()
    {
        if (DrawCensus::iVBlankWaitTimeout <= 0 && !DrawCensus::bVBlankWaitLog) { return; }
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(base + kWaitEventWrapper);
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC && p[3] == 0x28 && p[4] == 0x48 && p[5] == 0x8B && p[6] == 0x09 && p[7] == 0x85 && p[8] == 0xD2)
        {
            g_WaitEvent = safetyhook::create_inline(reinterpret_cast<void*>(base + kWaitEventWrapper), reinterpret_cast<void*>(&Hooked_WaitEvent));
            spdlog::info("PW pacing: vblank wait hook ({}): timeout {} ms, log {}.", g_WaitEvent ? "ok" : "FAILED", DrawCensus::iVBlankWaitTimeout, DrawCensus::bVBlankWaitLog ? "on" : "off");
            if (DrawCensus::bVBlankWaitLog)
            {
                g_RealEnterCs = reinterpret_cast<EnterCsFn>(PatchImport("KERNEL32.dll", "EnterCriticalSection", reinterpret_cast<void*>(&Hooked_EnterCriticalSection)));
                spdlog::info("PW pacing: EnterCriticalSection import hook {} (blocked time per tick in the vblank log).", g_RealEnterCs ? "ok" : "FAILED");
            }
        }
        else { spdlog::warn("PW pacing: wait wrapper +{:X} does not look as expected; vblank wait timeout not installed.", kWaitEventWrapper); }
    }
    // ---- the frame handoff ----------------------------------------------------------------------
    // +175B0 hands the game thread's finished frame to the render thread: it sets the flag at
    // display+0x32c0 that the render loop (+17BD0) polls, then Presents and clears. Its first
    // check (+175DF): if the flag is still set, the render thread has not taken the previous
    // frame yet (it is inside a vsync-blocked Present), and the function returns: this frame is
    // dropped and the display repeats the last one. Under composition Present returns at once
    // and the render thread is nearly always free; in direct flip (Borderless covering the
    // monitor, exclusive Fullscreen) it blocks to the vblank, and the drop lands every second
    // or so at 60 Hz. Waiting here for the flag to clear, briefly, keeps the frame.
    constexpr uintptr_t kHandoffCheck = 0x175DF;   // cmp byte ptr [rcx+0x32c0], 0
    SafetyHookMid g_HandoffCheck {};
    std::atomic<uint64_t> g_HandoffWaits { 0 }, g_HandoffWaitTicks { 0 }, g_HandoffDrops { 0 };
    void InstallHandoffWait()
    {
        if (DrawCensus::iFrameHandoffWait <= 0) { return; }
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(base + kHandoffCheck);
        if (!(p[0] == 0x80 && p[1] == 0xB9 && p[2] == 0xC0 && p[3] == 0x32 && p[4] == 0x00 && p[5] == 0x00 && p[6] == 0x00))
        {
            spdlog::warn("PW pacing: handoff site +{:X} does not look as expected; frame handoff wait not installed.", kHandoffCheck);
            return;
        }
        g_HandoffCheck = safetyhook::create_mid(base + kHandoffCheck, [](SafetyHookContext& ctx)
        {
            volatile uint8_t* flag = reinterpret_cast<volatile uint8_t*>(ctx.rcx + 0x32c0);
            if (*flag == 0) { return; }
            const int64_t t0 = Ticks();
            const double limit = static_cast<double>(DrawCensus::iFrameHandoffWait);
            while (*flag != 0 && TicksToMs(Ticks() - t0) < limit) { SwitchToThread(); }
            g_HandoffWaits.fetch_add(1);
            g_HandoffWaitTicks.fetch_add(Ticks() - t0);
            if (*flag != 0) { g_HandoffDrops.fetch_add(1); }
        });
        spdlog::info("PW pacing: frame handoff waits up to {} ms for the render thread instead of dropping the frame ({}).", DrawCensus::iFrameHandoffWait, g_HandoffCheck ? "hooked" : "hook FAILED");
    }
    // ---- the frame-skip governor ------------------------------------------------------------------
    // The game's frame end (+78250..+78480) measures each frame in vblanks by wall clock and,
    // when a frame measures longer than the current vblank wait count (+1084484), raises the
    // count (+78462, capped at 3): the next frame waits two vblanks, a doubled frame; a few
    // frames later it lowers the count again (+7843C). Under direct flip (Borderless covering
    // the monitor, exclusive Fullscreen) the render thread's Present returns at the vblank, so
    // a frame end lands just past it about once a second and measures as 17 ms: the stutter
    // (2026-09-14, vblank wait log: every missed tick was a second wait with 0 ms of work).
    // Skipping that one store leaves the game's explicit count (30 fps movies and menus, set at
    // +78070) and the lowering path intact.
    constexpr uintptr_t kGovernorRaise = 0x78462;   // mov [wait count], edi (6 bytes)
    SafetyHookMid g_GovernorRaise {};
    std::atomic<uint64_t> g_GovernorRaisesSkipped { 0 };
    void InstallGovernor()
    {
        if (DrawCensus::bFrameSkipGovernor) { return; }
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(base + kGovernorRaise);
        if (!(p[0] == 0x89 && p[1] == 0x3D))
        {
            spdlog::warn("PW pacing: governor site +{:X} does not look as expected; the frame-skip governor stays.", kGovernorRaise);
            return;
        }
        g_GovernorRaise = safetyhook::create_mid(base + kGovernorRaise, [](SafetyHookContext& ctx)
        {
            g_GovernorRaisesSkipped.fetch_add(1);
            ctx.rip += 6;   // skip the store: the wait count keeps its value
        });
        spdlog::info("PW pacing: frame-skip governor off: a long-measured frame no longer raises the vblank wait count ({}).", g_GovernorRaise ? "hooked" : "hook FAILED");
    }
    // The render thread's spin: counted in the Sleep hook, the clock read once per 1024 calls.
    thread_local uint32_t t_SpinCalls = 0;
    thread_local int64_t t_SpinStart = 0;
    thread_local bool t_Sampled = false;
    void ResetSpin() { t_SpinCalls = 0; t_SpinStart = 0; t_Sampled = false; }
    void WatchSpin()
    {
        if ((++t_SpinCalls & 1023) != 0) { return; }
        const int64_t now = Ticks();
        if (!t_SpinStart) { t_SpinStart = now; return; }
        if (!t_Sampled && TicksToMs(now - t_SpinStart) > 20.0)
        {
            t_Sampled = true;
            g_SpinHitches.fetch_add(1);
            SampleThreads(TicksToMs(now - t_SpinStart));
        }
    }
    void Account(std::map<uintptr_t, CallerStat>& table, uintptr_t caller, int64_t ticks, DWORD arg)
    {
        std::lock_guard lock(g_PacingMutex);
        CallerStat& c = table[caller];
        c.calls++; c.ticks += ticks; c.maxArg = std::max(c.maxArg, static_cast<uint32_t>(arg));
    }
    void WINAPI Hooked_Sleep(DWORD ms)
    {
        const bool present = OnPresentThread();
        if (ms == 0)
        {
            // The render thread's handoff spin (~170k calls a frame): no clock reads here.
            if (present && DrawCensus::bHitchSampler) { WatchSpin(); }
            if (!present) { g_OtherThreadWaits.fetch_add(1); }
            g_RealSleep(0);
            return;
        }
        const uintptr_t caller = CallerRva(_ReturnAddress());
        ThreadWait& w = MyWait();
        const int64_t t0 = Ticks();
        w.caller.store(caller); w.ms.store(ms); w.since.store(t0);
        g_RealSleep(ms);
        w.since.store(0);
        AccountWait(Ticks() - t0, caller, ms);
        if (!present) { g_OtherThreadWaits.fetch_add(1); return; }
        const int64_t dt = Ticks() - t0;
        g_Work.sleeps.fetch_add(1); g_Work.sleepTicks.fetch_add(dt); g_Work.sleepCaller.store(caller);
        if (DrawCensus::bPacingLog) { Account(g_MainSleepers, caller, dt, ms); }
    }
    DWORD WINAPI Hooked_Wait(HANDLE h, DWORD ms)
    {
        const bool present = OnPresentThread();
        const uintptr_t caller = CallerRva(_ReturnAddress());
        ThreadWait& w = MyWait();
        const int64_t t0 = Ticks();
        w.caller.store(caller); w.ms.store(ms); w.since.store(t0);
        const DWORD r = g_RealWait(h, ms);
        w.since.store(0);
        AccountWait(Ticks() - t0, caller, ms);
        if (!present) { g_OtherThreadWaits.fetch_add(1); return r; }
        const int64_t dt = Ticks() - t0;
        g_Work.waits.fetch_add(1); g_Work.waitTicks.fetch_add(dt); g_Work.waitCaller.store(caller);
        if (DrawCensus::bPacingLog) { Account(g_MainWaiters, caller, dt, ms); }
        return r;
    }
    DWORD WINAPI Hooked_WaitEx(HANDLE h, DWORD ms, BOOL alertable)
    {
        const bool present = OnPresentThread();
        const uintptr_t caller = CallerRva(_ReturnAddress());
        ThreadWait& w = MyWait();
        const int64_t t0 = Ticks();
        w.caller.store(caller); w.ms.store(ms); w.since.store(t0);
        const DWORD r = g_RealWaitEx(h, ms, alertable);
        w.since.store(0);
        AccountWait(Ticks() - t0, caller, ms);
        if (!present) { g_OtherThreadWaits.fetch_add(1); return r; }
        const int64_t dt = Ticks() - t0;
        g_Work.waits.fetch_add(1); g_Work.waitTicks.fetch_add(dt); g_Work.waitCaller.store(caller);
        if (DrawCensus::bPacingLog) { Account(g_MainWaiters, caller, dt, ms); }
        return r;
    }
    std::string CallerTable(std::map<uintptr_t, CallerStat>& table, uint32_t frames)
    {
        std::string out;
        std::lock_guard lock(g_PacingMutex);
        for (const auto& [rva, c] : table)
        {
            out += fmt::format(" +{:X}: {:.1f} calls {:.2f} ms/frame (arg<={});", rva, static_cast<double>(c.calls) / frames, TicksToMs(c.ticks) / frames, c.maxArg);
        }
        table.clear();
        return out.empty() ? " none" : out;
    }
    // Replaces one import in the game's IAT; returns the original pointer or nullptr.
    void* PatchImport(const char* dll, const char* name, void* replacement)
    {
        const auto base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) { return nullptr; }
        for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) { continue; }
            auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
            for (; thunk->u1.AddressOfData; thunk++, iat++)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) { continue; }
                const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
                if (strcmp(byName->Name, name) != 0) { continue; }
                void* original = reinterpret_cast<void*>(iat->u1.Function);
                DWORD old = 0;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) { return nullptr; }
                iat->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                return original;
            }
        }
        return nullptr;
    }
    void InstallPacingHooks()
    {
        g_RealSleep = reinterpret_cast<SleepFn>(PatchImport("KERNEL32.dll", "Sleep", reinterpret_cast<void*>(&Hooked_Sleep)));
        g_RealWait = reinterpret_cast<WaitFn>(PatchImport("KERNEL32.dll", "WaitForSingleObject", reinterpret_cast<void*>(&Hooked_Wait)));
        g_RealWaitEx = reinterpret_cast<WaitExFn>(PatchImport("KERNEL32.dll", "WaitForSingleObjectEx", reinterpret_cast<void*>(&Hooked_WaitEx)));
        spdlog::info("PW pacing: import hooks Sleep {}, WaitForSingleObject {}, WaitForSingleObjectEx {}.", g_RealSleep ? "ok" : "FAILED", g_RealWait ? "ok" : "FAILED", g_RealWaitEx ? "ok" : "FAILED");
    }
    std::atomic<uint64_t> g_DrawsSeen { 0 };
    std::atomic<uint64_t> g_ListsSeen { 0 };
    std::atomic<uint64_t> g_GpuLocalStripped { 0 };
    std::atomic<bool> g_TitleSeen { false };
    std::atomic<uint64_t> g_TextureMaps { 0 };       // Map calls on large textures (any time)
    std::atomic<uint64_t> g_TextureMapFails { 0 };
    std::unordered_set<ID3D11Resource*> g_Stripped;     // textures whose CPU access was stripped (under g_Mutex)
    std::atomic<uint64_t> g_StrippedMaps { 0 };       // Map calls on one of them (any size), each logged

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
        spdlog::info("PW timing: {} frames timed over {:.1f} s at {:.1f} fps: mean GPU work {:.2f} ms per frame, max {:.2f} ms. GPU-local textures {}, maps on them so far {}. Mean cost per pass (sum over the frame / frames; count = occurrences per frame):", g_FrameTotals.size(), wall, fps, sum / g_FrameTotals.size(), mx, g_GpuLocalStripped.load(), g_StrippedMaps.load());
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

    struct Snapshot { float f[64]; size_t count; bool valid; float uiScale[8]; bool hasUiScale; };   // uiScale: rows 132-133 of a 2640-byte UI constant buffer (the layout scale and offset)
    std::unordered_map<void*, uint64_t> g_ShaderHash;             // shader object -> bytecode hash
    std::unordered_map<ID3D11Resource*, bool> g_UiBuffer;          // constant buffer -> its last upload carried the UI ortho (private module)
    std::unordered_map<ID3D11Resource*, float> g_UiShift;          // constant buffer -> the wide-scene move (canvas units) of its last upload
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

    // Lab key Dump Shaders: every shader's bytecode goes to <game root>\logs\shaders\<hash>.<kind>.cso
    // so the passes can be read with D3DDisassemble offline.
    void DumpShader(const void* code, size_t size, uint64_t hash, const char* kind)
    {
        if (!DrawCensus::bDumpShaders) { return; }
        const std::string dir = (mgs4e::game::Root() / "logs" / "shaders").string();
        CreateDirectoryA((mgs4e::game::Root() / "logs").string().c_str(), nullptr);
        CreateDirectoryA(dir.c_str(), nullptr);
                // The 8-hex-digit id used everywhere else is the first 8 digits of the 16-digit hash.
        const std::string byId = std::format("{}/{}.{}.cso", dir, std::format("{:016x}", hash).substr(0, 8), kind);
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
        if (size >= 536 * sizeof(float)) { std::memcpy(s.uiScale, static_cast<const float*>(data) + 528, sizeof(s.uiScale)); s.hasUiScale = true; }
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
            if (slot == 0 && it->second.hasUiScale)
            {
                cb += " |r132";
                for (float v : it->second.uiScale) { cb += std::format(" {:.4g}", v); }
            }
        }
        if (cb.empty()) { cb = "cb -"; }
        const auto vs = g_ShaderHash.find(st.vs);
        const auto ps = g_ShaderHash.find(st.ps);
        bool uiBound = st.uiUpload;
        if (const auto ub = g_UiBuffer.find(static_cast<ID3D11Resource*>(st.vsCb[0])); ub != g_UiBuffer.end()) { uiBound = ub->second; }
        const char* wide = UiBias::WideSceneActive() ? UiBias::Classify(static_cast<ID3D11DeviceContext*>(self), uiBound, ps == g_ShaderHash.end() ? 0 : ps->second) : "";
        spdlog::info("PW census: f{} {}#{} {} n={} start={} base={} topo={} vp=({:.0f},{:.0f} {:.0f}x{:.0f}) sc=({},{})-({},{}) rt={} tex={} smp={} vs={} ps={} {} {} | {}",
            g_Frame.load(), HooksFor(self).name, st.draws, kind, count, start, baseVertex, static_cast<int>(st.topology),
            st.viewport.TopLeftX, st.viewport.TopLeftY, st.viewport.Width, st.viewport.Height,
            st.scissor.left, st.scissor.top, st.scissor.right, st.scissor.bottom, st.target, st.texture, st.sampler,
            vs == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", vs->second).substr(0, 8),
            ps == g_ShaderHash.end() ? std::string("?") : std::format("{:016x}", ps->second).substr(0, 8),
            wide, cb, GameCallers(10));
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
                Hex(st.lastVertexUpload.data(), std::min<size_t>(st.lastVertexUpload.size(), st.lastVertexUpload.size() <= 256 ? 256 : 24)), st.lastVertexUploadCaller);
            st.lastVertexUpload.clear();
        }
    }

    // ---- context hooks (both classes) ------------------------------------------------------------
    void STDMETHODCALLTYPE Hooked_Draw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertex)
    {
        LogDraw(self, "Draw", vertexCount, startVertex, 0);
        bool ui = false;
        uint64_t ps = 0;
        float shift = 0;
        if (UiBias::WideSceneActive())
        {
            std::lock_guard lock(g_Mutex);
            auto* cb = static_cast<ID3D11Resource*>(g_State[self].vsCb[0]);
            const auto ub = g_UiBuffer.find(cb);
            ui = ub != g_UiBuffer.end() ? ub->second : g_State[self].uiUpload;
            const auto us = g_UiShift.find(cb);
            if (us != g_UiShift.end()) { shift = us->second; }
            const auto it = g_ShaderHash.find(g_State[self].ps);
            if (it != g_ShaderHash.end()) { ps = it->second; }
        }
        UiBias::BeforeDraw(self, ui, ps, shift);
        Original<decltype(&Hooked_Draw)>(self, kCtxDraw)(self, vertexCount, startVertex);
        UiBias::AfterDraw(self, ui);
    }

    void STDMETHODCALLTYPE Hooked_DrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndex, INT baseVertex)
    {
        LogDraw(self, "DrawIndexed", indexCount, startIndex, static_cast<UINT>(baseVertex));
        bool ui = false;
        uint64_t ps = 0;
        float shift = 0;
        if (UiBias::WideSceneActive())
        {
            std::lock_guard lock(g_Mutex);
            auto* cb = static_cast<ID3D11Resource*>(g_State[self].vsCb[0]);
            const auto ub = g_UiBuffer.find(cb);
            ui = ub != g_UiBuffer.end() ? ub->second : g_State[self].uiUpload;
            const auto us = g_UiShift.find(cb);
            if (us != g_UiShift.end()) { shift = us->second; }
            const auto it = g_ShaderHash.find(g_State[self].ps);
            if (it != g_ShaderHash.end()) { ps = it->second; }
        }
        UiBias::BeforeDraw(self, ui, ps, shift);
        Original<decltype(&Hooked_DrawIndexed)>(self, kCtxDrawIndexed)(self, indexCount, startIndex, baseVertex);
        UiBias::AfterDraw(self, ui);
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
        D3D11_VIEWPORT copy[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
        if (count > 0 && viewports && count <= std::size(copy))
        {
            std::memcpy(copy, viewports, count * sizeof(D3D11_VIEWPORT));
            if (UiBias::AdjustViewports(count, copy)) { viewports = copy; }
        }
        if (count > 0 && viewports) { std::lock_guard lock(g_Mutex); g_State[self].viewport = viewports[0]; }
        Original<decltype(&Hooked_RSSetViewports)>(self, kCtxRSSetViewports)(self, count, viewports);
    }

    void STDMETHODCALLTYPE Hooked_RSSetScissorRects(ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects)
    {
        D3D11_RECT copy[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
        if (count > 0 && rects && count <= std::size(copy))
        {
            std::memcpy(copy, rects, count * sizeof(D3D11_RECT));
            if (UiBias::AdjustScissors(count, copy)) { rects = copy; }
        }
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
        const int64_t t0 = Ticks();
        const HRESULT r = Original<decltype(&Hooked_Map)>(self, kCtxMap)(self, res, sub, type, flags, mapped);
        const int64_t dt = Ticks() - t0;
        AccountD3D(dt, "Map");
        g_Work.mapTicks.fetch_add(dt);
        g_Work.maps.fetch_add(1);
        if (TicksToMs(dt) > 1.0) { g_Work.mapWaits.fetch_add(1); }
        if (res)
        {
            bool stripped = false;
            { std::lock_guard lock(g_Mutex); stripped = g_Stripped.count(res) != 0; }
            if (stripped)
            {
                // The set holds addresses; a destroyed texture's address can come back as a
                // buffer or another texture. Only a live 2D texture without CPU access is ours.
                ID3D11Texture2D* tex = nullptr;
                D3D11_TEXTURE2D_DESC d {};
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) { tex->GetDesc(&d); tex->Release(); }
                if (!tex || d.CPUAccessFlags != 0 || d.Width < 1024)
                {
                    std::lock_guard lock(g_Mutex);
                    g_Stripped.erase(res);
                    stripped = false;
                }
            }
            if (stripped)
            {
                g_StrippedMaps.fetch_add(1);
                std::lock_guard lock(g_Mutex);
                spdlog::warn("PW census: f{} {} Map on a GPU-local texture ({}) type={} -> {:#x}: the game wanted CPU access here | {}", g_Frame.load(), HooksFor(self).name, DescribeResource(res), static_cast<int>(type), static_cast<uint32_t>(r), GameCallers(6));
            }
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
                if (constant)
                {
                    float shift = 0;
                    const bool ui = UiBias::OnConstantUnmap(it->second.data, size, &shift);
                    g_State[self].uiUpload = ui;
                    g_UiBuffer[res] = ui;   // the flag belongs to the buffer: the draw reads it from VS slot 0
                    g_UiShift[res] = shift;
                    Snapshot16(res, it->second.data, size); Trace(self, std::format("Unmap cb {}B", size));
                }
                if (!constant && size) { UiBias::OnVertexUnmap(self, g_State[self].texture, it->second.data, size); }
                if (!constant && size && g_FramesToLog.load() > 0)
                {
                    Trace(self, std::format("Unmap vb {}B", size));
                    // Every quad of the upload (24-byte vertices, position last): the shared text
                    // buffer positions glyphs here rather than by a translation row.
                    if (size % 24 == 0 && size <= 65536)
                    {
                        const auto* v = static_cast<const uint8_t*>(it->second.data);
                        const size_t verts = size / 24;
                        std::string quads;
                        int shown = 0;
                        for (size_t q = 0; q + 4 <= verts && shown < 80; q += 4, shown++)
                        {
                            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                            for (size_t k = 0; k < 4; k++)
                            {
                                float xy[2];
                                std::memcpy(xy, v + (q + k) * 24 + 12, sizeof(xy));
                                x0 = std::min(x0, xy[0]); x1 = std::max(x1, xy[0]); y0 = std::min(y0, xy[1]); y1 = std::max(y1, xy[1]);
                            }
                            quads += std::format(" ({:.0f},{:.0f})-({:.0f},{:.0f})", x0, y0, x1, y1);
                        }
                        spdlog::info("PW census: f{} {} vb upload {}B tex={} quads{}{}", g_Frame.load(), HooksFor(self).name, size, g_State[self].texture, quads, verts / 4 > 80 ? " ..." : "");
                    }
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
        { const int64_t t0 = Ticks(); Original<decltype(&Hooked_UpdateSubresource)>(self, kCtxUpdateSubresource)(self, res, sub, box, data, rowPitch, depthPitch); AccountD3D(Ticks() - t0, "UpdateSubresource"); }
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
        if (dst && RenderPolicy::ResolveAsCopy(src))
        {
            // A single-sampled source cannot be resolved; copy it instead (same size and format).
            Original<decltype(&Hooked_CopyResource)>(self, kCtxCopyResource)(self, dst, src);
            return;
        }
        if (g_FramesToLog.load() > 0) { std::lock_guard lock(g_Mutex); spdlog::info("PW census: f{} {} ResolveSubresource dst={} src={} | {}", g_Frame.load(), HooksFor(self).name, DescribeResource(dst), DescribeResource(src), GameCallers(6)); }
        Original<decltype(&Hooked_ResolveSubresource)>(self, kCtxResolveSubresource)(self, dst, dstSub, src, srcSub, fmt);
    }

    void STDMETHODCALLTYPE Hooked_Flush(ID3D11DeviceContext* self)
    {
        const int64_t t0 = Ticks();
        Original<decltype(&Hooked_Flush)>(self, kCtxFlush)(self);
        AccountD3D(Ticks() - t0, "Flush");
    }
    HRESULT STDMETHODCALLTYPE Hooked_FinishCommandList(ID3D11DeviceContext* self, BOOL restore, ID3D11CommandList** list)
    {
        const int64_t t0 = Ticks();
        const HRESULT r = Original<decltype(&Hooked_FinishCommandList)>(self, kCtxFinishCommandList)(self, restore, list);
        AccountD3D(Ticks() - t0, "FinishCommandList");
        return r;
    }
    void STDMETHODCALLTYPE Hooked_ExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore)
    {
        g_ListsSeen.fetch_add(1);
        const bool timing = g_TimingState.load() == 2;
        if (timing) { std::lock_guard lock(g_Mutex); StampBefore(self, std::format("list {:#x} begins", reinterpret_cast<uintptr_t>(list) & 0xffffff)); }
        if (g_FramesToLog.load() > 0) { spdlog::info("PW census: f{} ExecuteCommandList {:#x} | {}", g_Frame.load(), reinterpret_cast<uintptr_t>(list) & 0xffffff, GameCallers(4)); }
        { const int64_t t0 = Ticks(); Original<decltype(&Hooked_ExecuteCommandList)>(self, kCtxExecuteCommandList)(self, list, restore); AccountD3D(Ticks() - t0, "ExecuteCommandList"); }
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
            { kCtxPSSetSamplers, reinterpret_cast<void*>(Hooked_PSSetSamplers) },
            { kCtxMap, reinterpret_cast<void*>(Hooked_Map) },
            { kCtxUnmap, reinterpret_cast<void*>(Hooked_Unmap) },
            { kCtxUpdateSubresource, reinterpret_cast<void*>(Hooked_UpdateSubresource) },
            { kCtxIASetPrimitiveTopology, reinterpret_cast<void*>(Hooked_IASetPrimitiveTopology) },
            { kCtxExecuteCommandList, reinterpret_cast<void*>(Hooked_ExecuteCommandList) },
            { kCtxFlush, reinterpret_cast<void*>(Hooked_Flush) },
            { kCtxFinishCommandList, reinterpret_cast<void*>(Hooked_FinishCommandList) },
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
    SafetyHookInline Device_CreateSamplerState_hook {};
    std::set<std::string> g_SamplersSeen;   // under g_Mutex
    std::unordered_map<void*, std::string> g_SamplerName;   // sampler object -> "filter/aniso/address" as the game asked (under g_Mutex)
    std::atomic<uint32_t> g_SamplersAniso { 0 };

    // Every distinct sampler the game creates is logged once (filter, anisotropy, address
    // modes, LOD bias); with iAnisotropy set, linear non-comparison samplers become
    // anisotropic at that level (point samplers are left alone: the PSP-era art is meant to
    // be nearest-filtered where the game says so).
    HRESULT STDMETHODCALLTYPE Hooked_CreateSamplerState(ID3D11Device* self, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
    {
        D3D11_SAMPLER_DESC changed {};
        if (desc)
        {
            const std::string key = std::format("filter={:#x} aniso={} address={}/{}/{} bias={:.2f} lod={:.0f}..{:.0f} cmp={}",
                static_cast<int>(desc->Filter), desc->MaxAnisotropy, static_cast<int>(desc->AddressU), static_cast<int>(desc->AddressV), static_cast<int>(desc->AddressW),
                desc->MipLODBias, desc->MinLOD, desc->MaxLOD, static_cast<int>(desc->ComparisonFunc));
            bool fresh = false;
            { std::lock_guard lock(g_Mutex); fresh = g_SamplersSeen.insert(key).second; }
            if (fresh) { spdlog::info("PW census: sampler {} | {}", key, GameCallers(4)); }
            changed = *desc;
            if (RenderPolicy::SamplerDesc(changed)) { desc = &changed; g_SamplersAniso.fetch_add(1); }
        }
        const HRESULT r = Device_CreateSamplerState_hook.stdcall<HRESULT>(self, desc, out);
        if (SUCCEEDED(r) && out && *out && desc)
        {
            const D3D11_SAMPLER_DESC& game = (desc == &changed) ? *reinterpret_cast<const D3D11_SAMPLER_DESC*>(&changed) : *desc;
            std::lock_guard lock(g_Mutex);
            g_SamplerName[*out] = std::format("{:#x}/a{}/{}{}{}", static_cast<int>(game.Filter), game.MaxAnisotropy, static_cast<int>(game.AddressU), static_cast<int>(game.AddressV), static_cast<int>(game.AddressW));
        }
        return r;
    }

    void STDMETHODCALLTYPE Hooked_PSSetSamplers(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11SamplerState* const* samplers)
    {
        if (start == 0 && count > 0 && samplers)
        {
            std::lock_guard lock(g_Mutex);
            const auto it = g_SamplerName.find(samplers[0]);
            g_State[self].sampler = samplers[0] ? (it != g_SamplerName.end() ? it->second : std::string("?")) : std::string("-");
        }
        Original<decltype(&Hooked_PSSetSamplers)>(self, kCtxPSSetSamplers)(self, start, count, samplers);
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
    {
        D3D11_TEXTURE2D_DESC single {};
        bool strippedHere = false;
        if (desc)
        {
            single = *desc;
            if (RenderPolicy::TextureDesc(single, strippedHere)) { desc = &single; }
            if (strippedHere) { g_GpuLocalStripped.fetch_add(1); }
        }
        const int64_t t0 = Ticks();
        const HRESULT r = Device_CreateTexture2D_hook.stdcall<HRESULT>(self, desc, init, out);
        g_Work.textureTicks.fetch_add(Ticks() - t0);
        g_Work.textures.fetch_add(1);
        if (desc && desc->Width >= 1024) { g_Work.texturesLarge.fetch_add(1); std::lock_guard lock(g_Mutex); g_Work.lastTexture = std::format("{}x{} fmt{} usage={} cpu={:#x}", desc->Width, desc->Height, static_cast<int>(desc->Format), static_cast<int>(desc->Usage), desc->CPUAccessFlags); }
        if (SUCCEEDED(r) && out && *out && strippedHere)
        {
            std::lock_guard lock(g_Mutex);
            g_Stripped.insert(*out);
        }
        if (SUCCEEDED(r) && desc && (((desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)) && desc->Width >= 256) || desc->Width >= 1024))
        {
            g_Work.logLines.fetch_add(1);
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
                spdlog::info("PW census: frame {} ended with {} draw(s) on all contexts; biases applied so far {} vertex, {} translation; GPU-local textures {} (maps on them {}, large-texture maps {} / {} failed).", g_Frame.load(), g_DrawsThisFrame, UiBias::AppliedVertex(), UiBias::AppliedTranslation(), g_GpuLocalStripped.load(), g_StrippedMaps.load(), g_TextureMaps.load(), g_TextureMapFails.load());
                g_FramesToLog.store(left - 1);
                if (left - 1 == 0) { spdlog::info("PW census: done."); }
            }
            g_Frame.fetch_add(1);
            {
                std::lock_guard lock(g_Mutex);
                g_DrawsThisFrame = 0;
                for (auto& [ctx, st] : g_State) { st.draws = 0; }
            }
            g_PresentThread.store(GetCurrentThreadId());
            ResetSpin();
            if (DrawCensus::iPresentSync >= 0 && DrawCensus::iPresentSync <= 4) { sync = static_cast<UINT>(DrawCensus::iPresentSync); }
            if (g_TearingChain.load())
            {
                BOOL fullscreen = FALSE;
                if (SUCCEEDED(self->GetFullscreenState(&fullscreen, nullptr)) && !fullscreen)
                {
                    sync = 0; flags |= kPresentAllowTearing;
                    if (g_TearingPresents.fetch_add(1) == 0) { spdlog::info("PW pacing: presenting with sync 0 and ALLOW_TEARING from frame {}.", g_Frame.load()); }
                }
            }
            if (DrawCensus::bPacingLog && (sync != g_PresentSync || flags != g_PresentFlags))
            {
                if (g_PresentKinds.fetch_add(1) < 6) { spdlog::info("PW pacing: Present(sync {}, flags {:#x}) from frame {} (was sync {}, flags {:#x}).", sync, flags, g_Frame.load(), g_PresentSync, g_PresentFlags); }
                g_PresentSync = sync; g_PresentFlags = flags;
            }
            // The hitch log: this frame's interval is the time since the previous present returned.
            const int64_t now = Ticks();
            if (g_LastPresentEnd)
            {
                const double ms = TicksToMs(now - g_LastPresentEnd);
                if (g_AvgFrameMs <= 0) { g_AvgFrameMs = ms; }
                if (ms > 25.0 && ms > 1.5 * g_AvgFrameMs && g_Hitches < 400)
                {
                    g_Hitches++;
                    std::string last;
                    { std::lock_guard lock(g_Mutex); last = g_Work.lastTexture; }
                    spdlog::warn("PW hitch: frame {} took {:.1f} ms (running average {:.1f}): previous Present {:.1f} ms; textures created {} ({} large, {:.1f} ms in CreateTexture2D{}{}); buffers {} ({:.1f} ms); maps {} ({} waited >1 ms, {:.1f} ms total); census log lines {}; Sleep {} calls {:.1f} ms (last from +{:X}), waits {} calls {:.1f} ms (last from +{:X}).",
                        g_Frame.load(), ms, g_AvgFrameMs, TicksToMs(g_LastPresentTicks), g_Work.textures.load(), g_Work.texturesLarge.load(), TicksToMs(g_Work.textureTicks.load()),
                        last.empty() ? "" : ", last ", last, g_Work.buffers.load(), TicksToMs(g_Work.bufferTicks.load()), g_Work.maps.load(), g_Work.mapWaits.load(), TicksToMs(g_Work.mapTicks.load()), g_Work.logLines.load(),
                        g_Work.sleeps.load(), TicksToMs(g_Work.sleepTicks.load()), g_Work.sleepCaller.load(), g_Work.waits.load(), TicksToMs(g_Work.waitTicks.load()), g_Work.waitCaller.load());
                }
                g_AvgFrameMs = g_AvgFrameMs * 0.95 + ms * 0.05;
                // Pacing window: every 5 s, where the frames' time went.
                if (!g_Pacing.start) { g_Pacing.start = g_LastPresentEnd; }
                g_Pacing.frames++; g_Pacing.sleep += g_Work.sleepTicks.load(); g_Pacing.wait += g_Work.waitTicks.load(); g_Pacing.present += g_LastPresentTicks;
                g_Pacing.maxFrame = std::max(g_Pacing.maxFrame, ms); if (ms > 25.0) { g_Pacing.over25++; }
                const double windowMs = TicksToMs(now - g_Pacing.start);
                if (DrawCensus::bPacingLog && windowMs >= 5000.0)
                {
                    const uint64_t ticks = g_Ticks.exchange(0);
                    spdlog::info("PW pacing: {} frames in {:.1f} s ({:.2f} fps): present thread per frame: Sleep {:.2f} ms, waits {:.2f} ms, Present {:.2f} ms; longest frame {:.1f} ms, {} over 25 ms; other threads' waits {}; game ticks {} ({:.3f} Hz), vblank-locked ticks {}.",
                        g_Pacing.frames, windowMs / 1000.0, g_Pacing.frames * 1000.0 / windowMs, TicksToMs(g_Pacing.sleep) / g_Pacing.frames, TicksToMs(g_Pacing.wait) / g_Pacing.frames,
                        TicksToMs(g_Pacing.present) / g_Pacing.frames, g_Pacing.maxFrame, g_Pacing.over25, g_OtherThreadWaits.exchange(0), ticks, ticks * 1000.0 / windowMs, g_VBlankWaits.exchange(0));
                    if (g_VBlankPerTick.load() > 0)
                    {
                        const uint64_t w = g_VBlankWaits.load();
                        spdlog::info("PW pacing:   vblank ticker: intervals long (>1.5 vblank) {}, short (<0.5) {}; wait per tick mean {:.2f} ms, max {:.2f} ms.", g_VBlankLong.exchange(0), g_VBlankShort.exchange(0), w ? TicksToMs(g_VBlankWaitTicks.exchange(0)) / w : 0.0, TicksToMs(g_VBlankWaitMax.exchange(0)));
                    }
                    spdlog::info("PW pacing:   sleeps:{}", CallerTable(g_MainSleepers, g_Pacing.frames));
                    spdlog::info("PW pacing:   waits:{}", CallerTable(g_MainWaiters, g_Pacing.frames));
                    g_Pacing = PacingWindow {}; g_Pacing.start = now;
                }
            }
            g_Work.Reset();
            { std::lock_guard lock(g_Mutex); g_Work.lastTexture.clear(); }
            const HRESULT pr = SwapChain_Present_hook.stdcall<HRESULT>(self, sync, flags);
            g_LastPresentEnd = Ticks();
            g_LastPresentTicks = g_LastPresentEnd - now;
            return pr;
        }
        return SwapChain_Present_hook.stdcall<HRESULT>(self, sync, flags);
    }

    HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        if (g_TearingChain.load()) { flags |= kSwapChainFlagAllowTearing; }
        return SwapChain_ResizeBuffers_hook.stdcall<HRESULT>(self, count, width, height, format, flags);
    }
    void HookSwapChain(IUnknown* chain)
    {
        static bool hooked = false;
        if (hooked || !chain) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(chain);
        SwapChain_Present_hook = safetyhook::create_inline(vtable[kSwapChainPresent], reinterpret_cast<void*>(Hooked_Present));
        spdlog::info("PW census: swap chain Present hook {}.", SwapChain_Present_hook ? "ok" : "FAILED");
        if (g_TearingChain.load())
        {
            SwapChain_ResizeBuffers_hook = safetyhook::create_inline(vtable[kSwapChainResizeBuffers], reinterpret_cast<void*>(Hooked_ResizeBuffers));
            spdlog::info("PW pacing: swap chain ResizeBuffers hook {} (keeps ALLOW_TEARING on resizes).", SwapChain_ResizeBuffers_hook ? "ok" : "FAILED");
        }
        if (DrawCensus::iFramePacing == 2)
        {
            IDXGISwapChain* forOutput = nullptr;
            if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&forOutput))) && forOutput)
            {
                IDXGIOutput* out = nullptr;
                if (SUCCEEDED(forOutput->GetContainingOutput(&out)) && out)
                {
                    DXGI_OUTPUT_DESC od {};
                    out->GetDesc(&od);
                    g_VBlankOutput = out;   // kept referenced for the ticker thread
                    spdlog::info("PW pacing: vblank source is output {} ({}x{}).", [&] { std::string n; for (const wchar_t* c = od.DeviceName; *c; c++) { n += static_cast<char>(*c); } return n; }(), od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
                }
                else { spdlog::warn("PW pacing: the swap chain has no containing output; the ticker stays the game's."); }
                forOutput->Release();
            }
        }
        // The mode the chain actually runs at: in exclusive fullscreen the refresh rate decides
        // whether a game paced at 60 Hz drops a frame every second (59 Hz) or every 16 s (59.94).
        IDXGISwapChain* sc = nullptr;
        if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc))) && sc)
        {
            DXGI_SWAP_CHAIN_DESC d {};
            BOOL fullscreen = FALSE;
            IDXGIOutput* output = nullptr;
            sc->GetDesc(&d);
            sc->GetFullscreenState(&fullscreen, &output);
            std::string mode = "?";
            if (output)
            {
                DXGI_OUTPUT_DESC od {};
                output->GetDesc(&od);
                DXGI_MODE_DESC want = d.BufferDesc, got {};
                if (SUCCEEDED(output->FindClosestMatchingMode(&want, &got, nullptr)))
                {
                    mode = std::format("{}x{} @ {}/{} ({:.3f} Hz) on {}", got.Width, got.Height, got.RefreshRate.Numerator, got.RefreshRate.Denominator,
                        got.RefreshRate.Denominator ? static_cast<double>(got.RefreshRate.Numerator) / got.RefreshRate.Denominator : 0.0, std::filesystem::path(od.DeviceName).string());
                }
                output->Release();
            }
            spdlog::info("PW census: swap chain runs {}x{} requested {}/{} Hz, fullscreen={}, closest display mode {}.", d.BufferDesc.Width, d.BufferDesc.Height,
                d.BufferDesc.RefreshRate.Numerator, d.BufferDesc.RefreshRate.Denominator, fullscreen ? "yes" : "no", mode);
            sc->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
    {
        if (desc) { RenderPolicy::SwapChainDesc(*desc); }
        if (desc && DrawCensus::bAllowTearing)
        {
            const bool flip = desc->SwapEffect == 3 || desc->SwapEffect == 4;   // FLIP_SEQUENTIAL / FLIP_DISCARD
            if (flip && desc->Windowed)
            {
                desc->Flags |= kSwapChainFlagAllowTearing;
                g_TearingChain.store(true);
                spdlog::info("PW pacing: swap chain created with ALLOW_TEARING (flags {:#x}): windowed Present will not wait for the display.", desc->Flags);
            }
            else { spdlog::warn("PW pacing: Allow Tearing needs a windowed flip-model chain (effect {}, windowed {}); left alone.", static_cast<int>(desc->SwapEffect), desc->Windowed); }
        }
        if (desc && InternalSize::BackBufferWidth() > 0)
        {
            desc->BufferDesc.Width = InternalSize::BackBufferWidth();
            desc->BufferDesc.Height = InternalSize::BackBufferHeight();
            spdlog::info("PW census: swap chain buffers requested at {}x{} (flip model stretches them onto the window).", desc->BufferDesc.Width, desc->BufferDesc.Height);
        }
        const HRESULT r = Factory_CreateSwapChain_hook.stdcall<HRESULT>(self, device, desc, chain);
        if (SUCCEEDED(r) && desc) { RenderPolicy::AfterSwapChain(desc->OutputWindow, *desc); }
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
        Device_CreateSamplerState_hook = safetyhook::create_inline(dv[kDevCreateSamplerState], reinterpret_cast<void*>(Hooked_CreateSamplerState));

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
            // Thirty seconds normally; the whole session while a pacing log is on.
            for (int i = 0; i < 30 || DrawCensus::bVBlankWaitLog || DrawCensus::bPacingLog; i++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const uint64_t d = g_DrawsSeen.load(), l = g_ListsSeen.load(), f = g_Frame.load();
                spdlog::info("PW census: rate: {} draw(s), {} command list(s), {} frame(s) in the last second; large-texture maps so far {} ({} failed), CPU access stripped on {} textures.", d - lastDraws, l - lastLists, f - lastFrames, g_TextureMaps.load(), g_TextureMapFails.load(), g_GpuLocalStripped.load());
                if (g_GovernorRaise)
                {
                    static uint64_t lastSkipped = 0;
                    const uint64_t sk = g_GovernorRaisesSkipped.load();
                    if (sk != lastSkipped) { spdlog::info("PW pacing: governor: {} raise(s) of the vblank wait count prevented this second.", sk - lastSkipped); }
                    lastSkipped = sk;
                }
                if (g_HandoffCheck)
                {
                    static uint64_t lastWaits = 0, lastDrops = 0, lastTicks = 0;
                    const uint64_t w = g_HandoffWaits.load(), dr = g_HandoffDrops.load(), tk = g_HandoffWaitTicks.load();
                    if (w != lastWaits) { spdlog::info("PW pacing: handoff: {} frame(s) waited for the render thread this second ({:.2f} ms each), {} still dropped.", w - lastWaits, w > lastWaits ? TicksToMs(tk - lastTicks) / (w - lastWaits) : 0.0, dr - lastDrops); }
                    lastWaits = w; lastDrops = dr; lastTicks = tk;
                }
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
    // Live commands: <game root>\logs\MGSPWEnabler_live.txt, polled every 250 ms and truncated
    // once read. (The harness's live.txt is the same file through a junction or a copy.)
    std::string LiveFile() { return (mgs4e::game::Root() / "logs" / "MGSPWEnabler_live.txt").string(); }

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
        else if (cmd == "skips")
        {
            int n = 20;
            in >> n;
            g_VBlankSkipBudget.store(std::max(1, n));
            spdlog::info("PW vblank: the next {} skipped tick(s) will be logged with their work and lock times (command).", std::max(1, n));
        }
        else if (cmd == "sample")
        {
            int n = 6;
            in >> n;
            g_SampleBudget.store(std::max(1, n));
            spdlog::info("PW sampler: the next {} hitch(es) will be sampled (command).", std::max(1, n));
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
                RunCommand(std::format("census {}", std::max(1, DrawCensus::iCensusFrames)));
            }
            f11Down = down;
            // F10: the next twenty skipped ticks with their work and lock times; F9: the thread
            // sampler for the next six hitches.
            static bool f10Down = false, f9Down = false;
            const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0, f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (f10 && !f10Down) { spdlog::info("PW live: F10 pressed (frame {}).", g_Frame.load()); RunCommand("skips 20"); }
            if (f9 && !f9Down) { spdlog::info("PW live: F9 pressed (frame {}).", g_Frame.load()); RunCommand("sample 6"); }
            f10Down = f10; f9Down = f9;
            static int tick = 0;
            if (++tick % 5) { continue; }   // the command file every 250 ms
            HANDLE file = CreateFileA(LiveFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE) { continue; }
            char buf[512] {};
            DWORD read = 0;
            ReadFile(file, buf, sizeof(buf) - 1, &read, nullptr);
            CloseHandle(file);
            if (read == 0) { continue; }
            std::string text(buf, read);
            HANDLE clear = CreateFileA(LiveFile().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
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
        if (DrawCensus::bPacingLog || DrawCensus::bHitchSampler) { InstallPacingHooks(); }
        InstallVBlankWaitHook();
        InstallHandoffWait();
        InstallGovernor();
        if (DrawCensus::bHitchSampler) { spdlog::info("PW sampler: armed by the live command \"sample N\": the next N times the render thread waits 20 ms for a frame, the other threads are sampled."); }
        if (DrawCensus::bPacingLog || DrawCensus::iFramePacing != 0) { InstallTickerHooks(); }
        else { spdlog::info("PW pacing: off (no import hooks, no ticker hooks; the game's own pacing)."); }
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
            spdlog::info("PW live: polling {}.", LiveFile());
            std::thread(PollThread).detach();
        }
    }
}

#endif
