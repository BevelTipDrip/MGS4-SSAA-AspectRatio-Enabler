#pragma once
#include <cstddef>
#include <cstdint>

// Code sites found by signature rather than by fixed address. The 2026-10-27 Steam build
// (BGFX 0.0.8) recompiled the game: .text shrank from 0x165D000 to 0x10A2200 bytes and every
// function moved, while the code of each function stayed the same apart from its call targets
// and RIP-relative operands. A pattern is IDA style ("48 8B 0D ?? ?? ?? ?? 48 B8", ?? = any
// byte), starts at an instruction boundary and masks rel32 branch targets and RIP-relative
// displacements. The site is the match plus `offset`. `hint` is the site's RVA in the build the
// pattern was checked on: the scan looks around it first, then over the whole .text, and accepts
// only exactly one hit, so a build where a site is gone or ambiguous turns that feature off
// instead of patching the wrong code. Every pattern here is unique in both the BGFX 0.0.3 and
// 0.0.8 builds; the generator and that proof are in the harness (C:\mgspf_tools\mgs4_008\tools,
// gen_sigs.py, signatures.json).
namespace mgs4e::sites
{
    struct Signature
    {
        const char* name;
        uintptr_t hint;
        const char* pattern;
        size_t offset;
    };

    // The site's RVA, or 0 when the pattern is missing or ambiguous (both logged). A site found
    // away from its hint is logged with the distance.
    uintptr_t Resolve(const Signature& s);

    // The site's address in the running module, or nullptr (see Resolve).
    uint8_t* Address(const Signature& s);

    // Logs the executable's link timestamp, .text extent and the game's version.id line once, so
    // a report names the build. Also takes the pristine copy of .text that Resolve scans; call it
    // before any hook is installed.
    void LogBuild();

    // Releases the copy of .text. A later Resolve scans the live code instead (where another
    // hook may already sit on the pattern).
    void Forget();
}
