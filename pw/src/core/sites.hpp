#pragma once
#include <cstddef>
#include <cstdint>

// Code sites found by signature rather than by fixed address, so a game update that only shifts
// code (the 2026-09-18 build moved everything past +2A000 by 0x150..0x180 bytes) costs nothing.
// A pattern is IDA style ("48 8B 0D ?? ?? ?? ?? 48 B8", ?? = any byte), starts at an instruction
// boundary, and masks rel32 branch targets and RIP-relative displacements. The site is the match
// plus `offset` (an immediate inside an instruction, say). `hint` is the site's RVA in the build the
// pattern was made on: the scan looks around it first, then over the whole .text, and accepts only
// exactly one hit. The generator and the proof that every pattern is unique in the builds seen so
// far live in the harness (C:\mgspf_tools\pw\siggen.py, signatures.txt).
namespace mgspwe::sites
{
    struct Signature
    {
        const char* name;
        uintptr_t hint;
        const char* pattern;
        size_t offset;
    };

    // The site's RVA, or 0 when the pattern is missing or ambiguous (both logged). Sites that
    // moved from their hint are logged with the distance.
    uintptr_t Resolve(const Signature& s);

    // Logs the executable's link timestamp and .text extent once, so a report names the build. Also
    // takes the pristine copy of .text that every Resolve scans; call it before any hook is installed.
    void LogBuild();

    // Releases the copy of .text once every site has been resolved.
    void Forget();
}
