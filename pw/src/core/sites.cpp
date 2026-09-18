#include "pch.hpp"
#include "sites.hpp"

#include "game.hpp"
#include "log.hpp"

#include <ctime>
#include <vector>

namespace
{
    struct Text { uintptr_t rva = 0; size_t size = 0; uint32_t timestamp = 0; };

    const Text& TextSection()
    {
        static const Text text = []
        {
            Text t {};
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            t.timestamp = nt->FileHeader.TimeDateStamp;
            const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
            {
                if (std::memcmp(sec[i].Name, ".text", 5) == 0) { t.rva = sec[i].VirtualAddress; t.size = sec[i].Misc.VirtualSize; break; }
            }
            return t;
        }();
        return text;
    }

    struct Pattern { std::vector<uint8_t> bytes; std::vector<uint8_t> mask; };   // mask 1 = literal

    Pattern Parse(const char* s)
    {
        Pattern p;
        for (const char* c = s; *c;)
        {
            while (*c == ' ') { c++; }
            if (!*c) { break; }
            if (c[0] == '?') { p.bytes.push_back(0); p.mask.push_back(0); c += (c[1] == '?') ? 2 : 1; continue; }
            auto nibble = [](char h) { return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10; };
            p.bytes.push_back(static_cast<uint8_t>((nibble(c[0]) << 4) | nibble(c[1]))); p.mask.push_back(1); c += 2;
        }
        return p;
    }

    // Hits of the pattern in [rva, rva + size), at most `cap` of them.
    std::vector<uintptr_t> Find(const Pattern& p, uintptr_t rva, size_t size, size_t cap)
    {
        std::vector<uintptr_t> hits;
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const auto* hay = reinterpret_cast<const uint8_t*>(base + rva);
        const size_t n = p.bytes.size();
        if (size < n) { return hits; }
        // the first literal byte drives the outer scan
        size_t lead = 0; while (lead < n && !p.mask[lead]) { lead++; }
        for (size_t i = 0; i + n <= size; i++)
        {
            if (lead < n && hay[i + lead] != p.bytes[lead]) { continue; }
            size_t k = 0;
            for (; k < n; k++) { if (p.mask[k] && hay[i + k] != p.bytes[k]) { break; } }
            if (k == n) { hits.push_back(rva + i); if (hits.size() >= cap) { break; } }
        }
        return hits;
    }
}

namespace mgspwe::sites
{
    uintptr_t Resolve(const Signature& s)
    {
        const Text& text = TextSection();
        if (!text.size) { spdlog::warn("PW sites: no .text section found; {} unresolved.", s.name); return 0; }
        const Pattern p = Parse(s.pattern);
        if (p.bytes.empty()) { spdlog::warn("PW sites: {} has an empty pattern.", s.name); return 0; }
        const uintptr_t textEnd = text.rva + text.size;
        // around the hint first (the common case: unchanged or shifted a little)
        constexpr uintptr_t kWindow = 0x8000;
        const uintptr_t lo = (s.hint > text.rva + kWindow) ? s.hint - kWindow : text.rva;
        const uintptr_t hi = (s.hint + kWindow < textEnd) ? s.hint + kWindow : textEnd;
        std::vector<uintptr_t> hits = (lo < hi) ? Find(p, lo, hi - lo, 2) : std::vector<uintptr_t> {};
        if (hits.size() != 1) { hits = Find(p, text.rva, text.size, 2); }
        if (hits.empty()) { spdlog::warn("PW sites: {} not found in this build (hint +{:X}); that feature stays off.", s.name, s.hint); return 0; }
        if (hits.size() > 1) { spdlog::warn("PW sites: {} is ambiguous in this build (+{:X} and +{:X}); that feature stays off.", s.name, hits[0], hits[1]); return 0; }
        const uintptr_t rva = hits[0] + s.offset;
        if (rva != s.hint) { spdlog::info("PW sites: {} at +{:X} (hint +{:X}, moved {}{:#x}).", s.name, rva, s.hint, rva > s.hint ? "+" : "-", rva > s.hint ? rva - s.hint : s.hint - rva); }
        else if (mgs4e::log::Verbose()) { spdlog::info("PW sites: {} at +{:X}.", s.name, rva); }
        return rva;
    }

    void LogBuild()
    {
        const Text& text = TextSection();
        const std::time_t t = static_cast<std::time_t>(text.timestamp);
        std::tm tm {};
        char when[32] = "unknown";
        if (gmtime_s(&tm, &t) == 0) { std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm); }
        spdlog::info("PW sites: executable linked {} UTC, .text {:#x} bytes at +{:X}.", when, text.size, text.rva);
    }
}
