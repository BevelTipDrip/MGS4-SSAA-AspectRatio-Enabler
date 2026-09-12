#include "pch.hpp"
#include "probe.hpp"

#include "game.hpp"
#include "log.hpp"

#if MGS4E_LAB_BUILD

namespace
{
    // Module and command-line text is ASCII in practice; anything else is shown as '?'.
    std::string Narrow(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) { s.push_back(c < 0x80 ? static_cast<char>(c) : '?'); }
        return s;
    }

    // Shannon entropy of a buffer, bits per byte: ~8.0 for encrypted or compressed data,
    // ~6 or lower for x64 code.
    double Entropy(const uint8_t* data, size_t size)
    {
        std::array<uint32_t, 256> counts {};
        for (size_t i = 0; i < size; i++) { counts[data[i]]++; }
        double h = 0.0;
        for (uint32_t c : counts)
        {
            if (c == 0) { continue; }
            const double p = static_cast<double>(c) / static_cast<double>(size);
            h -= p * std::log2(p);
        }
        return h;
    }

    void LogModules(const char* when)
    {
        static const wchar_t* const interesting[] = {
            L"d3d11.dll", L"d3d12.dll", L"dxgi.dll", L"D3D12Core.dll", L"d3dcompiler_47.dll",
            L"winmm.dll", L"steam_api64.dll", L"MGSPWEnabler.asi", L"MGSPWResolutionUnlocked.asi", L"MGSPatriotFix.asi",
        };
        std::string seen;
        for (const wchar_t* name : interesting)
        {
            const HMODULE h = GetModuleHandleW(name);
            if (!h) { continue; }
            std::wstring w(name);
            if (!seen.empty()) { seen += ", "; }
            seen += Narrow(w);
        }
        spdlog::info("PW probe: modules {}: {}.", when, seen.empty() ? "(none of the watched ones)" : seen);
    }

    void LogCommandLine()
    {
        const wchar_t* cmd = GetCommandLineW();
        const std::wstring w(cmd ? cmd : L"");
        spdlog::info("PW probe: command line: {}", Narrow(w));
    }

    void LogDecryptTiming()
    {
        const auto* base = reinterpret_cast<const uint8_t*>(mgs4e::game::Module());
        const uint8_t* text = base + 0x1000;
        constexpr size_t kSample = 4096;
        const auto start = std::chrono::steady_clock::now();
        const double first = Entropy(text, kSample);
        spdlog::info("PW probe: .text entropy at init {:.2f} bits/byte (8.0 = still encrypted).", first);
        if (first < 7.0)
        {
            spdlog::info("PW probe: .text is readable at init; pattern scans can run immediately.");
            return;
        }
        std::thread([text, start]
        {
            for (int i = 0; i < 6000; i++)   // up to 30 s
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const double h = Entropy(text, kSample);
                if (h < 7.0)
                {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    spdlog::info("PW probe: .text became readable after {} ms (entropy {:.2f}).", ms, h);
                    return;
                }
            }
            spdlog::warn("PW probe: .text still looks encrypted after 30 s.");
        }).detach();
    }
}

namespace Probe
{
    void Run()
    {
        if (bLogCommandLine) { LogCommandLine(); }
        if (bLogLoadedModules)
        {
            LogModules("at init");
            std::thread([]
            {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                LogModules("20 s after init");
            }).detach();
        }
        if (bLogDecryptTiming) { LogDecryptTiming(); }
    }
}

#endif
