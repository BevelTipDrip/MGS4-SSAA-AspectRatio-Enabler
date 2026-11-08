#include "pch.hpp"
#include "stall_sampler.hpp"

#if MGS4E_LAB_BUILD

#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace StallSampler
{
    namespace
    {
        constexpr int kMaxFrames = 24;
        constexpr int kSignatureFrames = 12;
        constexpr size_t kRingSize = size_t { 1 } << 15;   // ~7 s at the sampling rates below
        constexpr int kColdEvery = 20;                      // other threads: every 20th tick

        int64_t g_Frequency = 1;
        int64_t Ticks()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return t.QuadPart;
        }
        double Ms(int64_t ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_Frequency); }

        // ---- unwind tables, indexed by the sampler itself ------------------------------------
        // RtlLookupFunctionEntry takes the loader's function-table lock; a suspended thread
        // that holds it would hang the sampler (and with it the suspended thread). Each
        // module's .pdata is looked up here instead, from a list built while nothing is
        // suspended.
        struct Module
        {
            uintptr_t base = 0, end = 0;
            const RUNTIME_FUNCTION* pdata = nullptr;
            size_t count = 0;
            std::string name;
        };
        std::vector<Module> g_Modules;   // sampler thread only

        void RefreshModules()
        {
            std::vector<Module> modules;
            HMODULE handles[1024];
            DWORD needed = 0;
            if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) { return; }
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++)
            {
                MODULEINFO info {};
                if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) { continue; }
                Module m;
                m.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
                m.end = m.base + info.SizeOfImage;
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (dir.VirtualAddress && dir.Size)
                {
                    m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + dir.VirtualAddress);
                    m.count = dir.Size / sizeof(RUNTIME_FUNCTION);
                }
                char path[MAX_PATH] {};
                K32GetModuleBaseNameA(GetCurrentProcess(), handles[i], path, MAX_PATH);
                m.name = path;
                modules.push_back(std::move(m));
            }
            std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
            g_Modules = std::move(modules);
        }

        const Module* FindModule(uint64_t pc)
        {
            for (const Module& m : g_Modules)
            {
                if (pc >= m.base && pc < m.end) { return &m; }
            }
            return nullptr;
        }

        const RUNTIME_FUNCTION* Lookup(uint64_t pc, uint64_t& imageBase)
        {
            const Module* m = FindModule(pc);
            if (!m || !m->pdata) { return nullptr; }
            imageBase = m->base;
            const auto rva = static_cast<DWORD>(pc - m->base);
            size_t lo = 0, hi = m->count;
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                const RUNTIME_FUNCTION& f = m->pdata[mid];
                if (rva < f.BeginAddress) { hi = mid; }
                else if (rva >= f.EndAddress) { lo = mid + 1; }
                else { return &f; }
            }
            return nullptr;
        }

        // ---- one sample ---------------------------------------------------------------------
        struct Sample
        {
            int64_t t = 0;
            DWORD tid = 0;
            uint8_t frames = 0;
            bool blocked = false;   // the thread sits right after a syscall: waiting, not running
            uint64_t pc[kMaxFrames] {};
        };
        Sample g_Ring[kRingSize];   // written and read by the sampler thread only
        size_t g_Head = 0;

        // No C++ objects here: __try needs a frame without unwinding.
        int Walk(CONTEXT* c, uint64_t* out, bool* blocked)
        {
            int n = 0;
            __try
            {
                const auto* leaf = reinterpret_cast<const uint8_t*>(c->Rip);
                *blocked = leaf && leaf[-2] == 0x0F && leaf[-1] == 0x05;
                while (n < kMaxFrames)
                {
                    const uint64_t pc = c->Rip;
                    if (pc < 0x10000) { break; }
                    out[n++] = pc;
                    uint64_t imageBase = 0;
                    const RUNTIME_FUNCTION* f = Lookup(n == 1 ? pc : pc - 1, imageBase);
                    if (!f)
                    {
                        if (n != 1) { break; }
                        // A leaf function has no unwind entry: its return address is on top.
                        c->Rip = *reinterpret_cast<const uint64_t*>(c->Rsp);
                        c->Rsp += 8;
                        continue;
                    }
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, const_cast<PRUNTIME_FUNCTION>(f),
                        c, &handlerData, &establisher, nullptr);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return n;
        }

        bool SampleThread(HANDLE thread, DWORD tid, int64_t now)
        {
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) { return false; }
            CONTEXT c {};
            c.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(thread, &c) != FALSE;
            if (ok)
            {
                Sample& s = g_Ring[g_Head % kRingSize];
                s.t = now;
                s.tid = tid;
                s.frames = static_cast<uint8_t>(Walk(&c, s.pc, &s.blocked));
                g_Head++;
            }
            ResumeThread(thread);
            return ok;
        }

        // ---- the threads --------------------------------------------------------------------
        struct Target
        {
            DWORD tid = 0;
            HANDLE handle = nullptr;
            std::string label;
        };
        std::vector<Target> g_Targets;   // sampler thread only
        DWORD g_MainTid = 0;
        DWORD g_SelfTid = 0;
        std::atomic<DWORD> g_PresentTid { 0 };

        void RefreshThreads()
        {
            // Drop the threads that have exited.
            for (Target& t : g_Targets)
            {
                if (t.handle && WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0)
                {
                    CloseHandle(t.handle);
                    t.handle = nullptr;
                }
            }
            std::erase_if(g_Targets, [](const Target& t) { return t.handle == nullptr; });

            const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) { return; }
            THREADENTRY32 te {};
            te.dwSize = sizeof(te);
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_SelfTid) { continue; }
                if (std::any_of(g_Targets.begin(), g_Targets.end(), [&](const Target& t) { return t.tid == te.th32ThreadID; })) { continue; }
                const HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                    FALSE, te.th32ThreadID);
                if (!h) { continue; }
                Target t { te.th32ThreadID, h, {} };
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(h, &description)) && description)
                {
                    for (const wchar_t* c = description; *c; c++) { t.label += static_cast<char>(*c < 128 ? *c : '?'); }
                    LocalFree(description);
                }
                if (t.label == "MGS4 MAIN") { g_MainTid = t.tid; }
                g_Targets.push_back(std::move(t));
            }
            CloseHandle(snap);
        }

        // ---- stalls -------------------------------------------------------------------------
        struct Stall { int64_t start, end; uint64_t present; };
        std::mutex g_StallMutex;
        std::vector<Stall> g_Pending;   // under g_StallMutex
        std::atomic<int64_t> g_LastPresent { 0 };
        std::atomic<uint64_t> g_Presents { 0 };
        std::atomic<int> g_Queued { 0 };
        int64_t g_Start = 0;
        int g_Analysed = 0;
        std::map<std::string, uint32_t> g_MainTotals;   // MAIN's chains summed over every stall
        uint32_t g_MainTotalSamples = 0;

        std::string Where(uint64_t pc, bool leaf)
        {
            const Module* m = FindModule(pc);
            if (!m) { return std::format("{:#x}", pc); }
            uint64_t imageBase = 0;
            const RUNTIME_FUNCTION* f = Lookup(leaf ? pc : pc - 1, imageBase);
            if (!f) { return std::format("{}+{:X}?", m->name, pc - m->base); }
            return std::format("{}!{:X}", m->name, f->BeginAddress);
        }

        std::string Signature(const Sample& s)
        {
            std::string out;
            for (int i = 0; i < s.frames && i < kSignatureFrames; i++)
            {
                if (i) { out += " < "; }
                out += Where(s.pc[i], i == 0);
            }
            return out;
        }

        std::string Label(DWORD tid)
        {
            std::string kind = tid == g_MainTid ? "MAIN" : tid == g_PresentTid.load() ? "PRESENT" : "";
            for (const Target& t : g_Targets)
            {
                if (t.tid == tid && !t.label.empty() && t.label != "MGS4 MAIN")
                {
                    kind += (kind.empty() ? "" : " ") + std::format("\"{}\"", t.label);
                }
            }
            return std::format("tid {}{}{}", tid, kind.empty() ? "" : " ", kind);
        }

        void Analyse(const Stall& stall)
        {
            struct PerThread
            {
                uint32_t samples = 0, blocked = 0;
                std::map<std::string, uint32_t> chains;
                std::map<std::string, uint64_t> leafExample;
            };
            std::map<DWORD, PerThread> threads;
            const size_t available = (std::min)(g_Head, kRingSize);
            for (size_t i = 1; i <= available; i++)
            {
                const Sample& s = g_Ring[(g_Head - i) % kRingSize];
                if (s.t < stall.start) { break; }
                if (s.t > stall.end) { continue; }
                PerThread& p = threads[s.tid];
                p.samples++;
                if (s.blocked) { p.blocked++; }
                const std::string sig = (s.blocked ? "[wait] " : "") + Signature(s);
                p.chains[sig]++;
                if (s.frames) { p.leafExample.try_emplace(sig, s.pc[0]); }
            }

            g_Analysed++;
            spdlog::warn("Stall {}: {:.1f} ms between presents, ending {:.2f} s into the session (present {}).",
                g_Analysed, Ms(stall.end - stall.start), Ms(stall.end - g_Start) / 1000.0, stall.present);

            auto top = [](const PerThread& p, size_t n, bool runningOnly)
            {
                std::vector<std::pair<std::string, uint32_t>> v;
                for (const auto& [sig, count] : p.chains)
                {
                    if (runningOnly && sig.starts_with("[wait]")) { continue; }
                    v.emplace_back(sig, count);
                }
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                if (v.size() > n) { v.resize(n); }
                return v;
            };

            for (const DWORD hot : { g_MainTid, g_PresentTid.load() })
            {
                const auto it = threads.find(hot);
                if (!hot || it == threads.end()) { continue; }
                const PerThread& p = it->second;
                spdlog::warn("  {}: {} samples, {} blocked in a system call.", Label(hot), p.samples, p.blocked);
                for (const auto& [sig, count] : top(p, 6, false))
                {
                    const auto leaf = p.leafExample.find(sig);
                    const Module* m = leaf != p.leafExample.end() ? FindModule(leaf->second) : nullptr;
                    spdlog::warn("    {:3}x {}{}", count, sig,
                        m ? std::format("   (leaf at {}+{:X})", m->name, leaf->second - m->base) : "");
                    if (hot == g_MainTid) { g_MainTotals[sig] += count; }
                }
                if (hot == g_MainTid) { g_MainTotalSamples += p.samples; }
            }

            for (const auto& [tid, p] : threads)
            {
                if (tid == g_MainTid || tid == g_PresentTid.load() || p.blocked == p.samples) { continue; }
                for (const auto& [sig, count] : top(p, 1, true))
                {
                    spdlog::warn("  {}: running in {} of {} samples: {}", Label(tid), p.samples - p.blocked, p.samples, sig);
                }
            }

            if (g_Analysed % 10 == 0)
            {
                PerThread all;
                all.chains = g_MainTotals;
                spdlog::warn("Stall summary after {} stalls: MAIN's commonest chains over {} samples:", g_Analysed, g_MainTotalSamples);
                for (const auto& [sig, count] : top(all, 10, false))
                {
                    spdlog::warn("    {:4}x {}", count, sig);
                }
            }
        }

        void ProcessStalls()
        {
            if (g_Queued.load(std::memory_order_relaxed) == 0) { return; }
            std::vector<Stall> ready;
            {
                std::lock_guard lock(g_StallMutex);
                ready.swap(g_Pending);
                g_Queued.store(0);
            }
            for (const Stall& s : ready) { Analyse(s); }
        }

        DWORD WINAPI SamplerThread(void*)
        {
            g_SelfTid = GetCurrentThreadId();
            SetThreadDescription(GetCurrentThread(), L"MGS4Enabler stall sampler");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            spdlog::info("Stall sampler: running (threshold {} ms, budget {} stalls, {} timer).", iThresholdMs, iBudget,
                timer ? "high-resolution" : "Sleep(1)");

            int64_t lastModules = 0, lastThreads = 0;
            bool warnedNoPresent = false, reportedMain = false;
            for (uint64_t tick = 0;; tick++)
            {
                if (timer)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -10000;   // 1 ms
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, INFINITE);
                }
                else
                {
                    Sleep(1);
                }

                const int64_t now = Ticks();
                if (Ms(now - lastModules) > 5000.0) { RefreshModules(); lastModules = now; }
                if (Ms(now - lastThreads) > 1000.0)
                {
                    RefreshThreads();
                    lastThreads = now;
                    if (g_MainTid && !reportedMain)
                    {
                        spdlog::info("Stall sampler: game thread \"MGS4 MAIN\" is tid {}; {} threads in the process.", g_MainTid, g_Targets.size());
                        reportedMain = true;
                    }
                    if (!warnedNoPresent && g_Presents.load() == 0 && Ms(now - g_Start) > 60000.0)
                    {
                        spdlog::warn("Stall sampler: no Present seen in 60 s (the Present hooks belong to the aspect fix; is it off?).");
                        warnedNoPresent = true;
                    }
                }

                const DWORD presentTid = g_PresentTid.load(std::memory_order_relaxed);
                const bool cold = tick % kColdEvery == 0;
                for (const Target& t : g_Targets)
                {
                    const bool hot = t.tid == g_MainTid || t.tid == presentTid;
                    if (hot || cold) { SampleThread(t.handle, t.tid, Ticks()); }
                }

                ProcessStalls();
            }
        }
    }

    void Start()
    {
        if (!bEnabled) { return; }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_Frequency = f.QuadPart;
        g_Start = Ticks();
        if (const HANDLE h = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr)) { CloseHandle(h); }
        else { spdlog::warn("Stall sampler: could not start its thread."); }
    }

    void OnPresent()
    {
        if (!bEnabled) { return; }
        g_PresentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        const uint64_t present = g_Presents.fetch_add(1) + 1;
        const int64_t now = Ticks();
        const int64_t previous = g_LastPresent.exchange(now);
        if (!previous || Ms(now - previous) < static_cast<double>(iThresholdMs)) { return; }
        static std::atomic<int> queued { 0 };
        if (queued.fetch_add(1) >= iBudget) { return; }
        std::lock_guard lock(g_StallMutex);
        g_Pending.push_back({ previous, now, present });
        g_Queued.store(static_cast<int>(g_Pending.size()));
    }
}

#endif
