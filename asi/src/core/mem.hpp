#pragma once

#include <cstdint>
#include <vector>

// Process-memory helpers: signature scanning, probing and small writes.
namespace mgs4e::mem
{
    // Finds the first occurrence of an IDA-style byte signature ("48 8B ?? 10", where "?" or
    // "??" is a wildcard) inside a loaded module's image. Logs the hit (under verbose
    // logging) or the miss, tagged with `tag` so the log names the feature that wanted it,
    // and warns when the signature is not unique. Returns nullptr on a miss; never throws.
    uint8_t* FindPattern(HMODULE module, const char* signature, const char* tag);

    // Same scan, no logging. For probes that expect to miss.
    uint8_t* FindPatternQuiet(HMODULE module, const char* signature);

    // Every occurrence of the signature in the module's image.
    std::vector<uint8_t*> FindAll(HMODULE module, const char* signature);

    // Whether [ptr, ptr + size) lies inside one committed, readable region.
    bool Readable(const void* ptr, size_t size);

    // As Readable, and the region is writable.
    bool Writable(const void* ptr, size_t size);

    // Resolves a rip-relative disp32 at `address` (the displacement's own address, not the
    // instruction's) to the absolute address it refers to.
    uintptr_t RipTarget(uintptr_t address) noexcept;

    // The destination of a near call (E8 rel32) at `callInsn`.
    uint8_t* CallTarget(uint8_t* callInsn) noexcept;

    // Writes a value through page protection.
    template<typename T>
    void Poke(uintptr_t address, T value)
    {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<T*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), old, &old);
    }
}
