#include "pch.hpp"
#include "mem.hpp"

#include "game.hpp"
#include "log.hpp"

namespace pfc::mem
{
    namespace
    {
        // "48 8B ?? 10" -> {0x48, 0x8B, -1, 0x10}
        std::vector<int> ParseSignature(const char* signature)
        {
            std::vector<int> bytes;
            for (const char* p = signature; *p; )
            {
                if (*p == ' ')
                {
                    ++p;
                    continue;
                }
                if (*p == '?')
                {
                    bytes.push_back(-1);
                    ++p;
                    if (*p == '?')
                    {
                        ++p;
                    }
                    continue;
                }
                bytes.push_back(static_cast<int>(std::strtoul(p, nullptr, 16) & 0xFF));
                ++p;
                if (*p && *p != ' ')
                {
                    ++p;
                }
            }
            return bytes;
        }

        struct Image
        {
            uint8_t* base = nullptr;
            size_t size = 0;
        };

        Image ImageOf(HMODULE module)
        {
            auto* const base = reinterpret_cast<uint8_t*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return { base, nt->OptionalHeader.SizeOfImage };
        }

        // Scans [start, end) for the parsed signature; returns nullptr on a miss.
        uint8_t* Scan(uint8_t* start, uint8_t* end, const std::vector<int>& bytes)
        {
            if (bytes.empty() || end - start < static_cast<ptrdiff_t>(bytes.size()))
            {
                return nullptr;
            }
            uint8_t* const last = end - bytes.size();
            for (uint8_t* p = start; p <= last; ++p)
            {
                bool hit = true;
                for (size_t i = 0; i < bytes.size(); ++i)
                {
                    if (bytes[i] != -1 && p[i] != static_cast<uint8_t>(bytes[i]))
                    {
                        hit = false;
                        break;
                    }
                }
                if (hit)
                {
                    return p;
                }
            }
            return nullptr;
        }
    }

    uint8_t* FindPatternQuiet(HMODULE module, const char* signature)
    {
        const Image image = ImageOf(module);
        return Scan(image.base, image.base + image.size, ParseSignature(signature));
    }

    std::vector<uint8_t*> FindAll(HMODULE module, const char* signature)
    {
        std::vector<uint8_t*> hits;
        const Image image = ImageOf(module);
        const std::vector<int> bytes = ParseSignature(signature);
        uint8_t* cursor = image.base;
        uint8_t* const end = image.base + image.size;
        while (uint8_t* hit = Scan(cursor, end, bytes))
        {
            hits.push_back(hit);
            cursor = hit + 1;
        }
        return hits;
    }

    uint8_t* FindPattern(HMODULE module, const char* signature, const char* tag)
    {
        uint8_t* const hit = FindPatternQuiet(module, signature);
        if (!hit)
        {
            spdlog::error("{}: pattern not found.", tag);
            return nullptr;
        }
        if (pfc::log::Verbose())
        {
            const auto rva = reinterpret_cast<uintptr_t>(hit) - reinterpret_cast<uintptr_t>(module);
            spdlog::info("{}: pattern found at {}+{:X}.", tag, pfc::game::ExePath().filename().string(), rva);
            if (FindAll(module, signature).size() > 1)
            {
                spdlog::warn("{}: pattern is not unique; using the first match.", tag);
            }
        }
        return hit;
    }

    bool Readable(const void* ptr, size_t size)
    {
        if (!ptr || size == 0)
        {
            return false;
        }
        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(ptr, &info, sizeof(info)))
        {
            return false;
        }
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            return false;
        }
        const auto begin = reinterpret_cast<uintptr_t>(ptr);
        const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return begin + size <= regionEnd;
    }

    bool Writable(const void* ptr, size_t size)
    {
        if (!Readable(ptr, size))
        {
            return false;
        }
        MEMORY_BASIC_INFORMATION info {};
        VirtualQuery(ptr, &info, sizeof(info));
        return (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    uintptr_t RipTarget(uintptr_t address) noexcept
    {
        return address + 4 + *reinterpret_cast<const int32_t*>(address);
    }

    uint8_t* CallTarget(uint8_t* callInsn) noexcept
    {
        return callInsn + 5 + *reinterpret_cast<const int32_t*>(callInsn + 1);
    }
}
