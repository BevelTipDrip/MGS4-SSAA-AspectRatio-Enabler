#pragma once

// The game asks the Direct3D device to create blend, depth-stencil, rasterizer and sampler state
// objects about 350 times a frame, and in a whole session it only ever asks for 42 distinct ones.
// Those objects are immutable; creating them per frame is a straightforward defect in the port.
// Every call also takes the device-wide lock, which is what made this look like the cause of the
// 60 Hz stutter. It was not: the stutter was the busy waits (busy_wait.hpp). This remains worth
// having on its own merits, removing the redundant calls and about 8% of the game thread's
// executed CPU, which matters most on slower processors.
//
// Shared by both builds so the two cannot drift: the Lab census calls these from its hooks and the
// Release build's lean hooks call the same functions. See docs/pw/frame-pacing.md section 5.1.
namespace StateCache
{
    // Bit 1: blend, depth-stencil and rasterizer. Bit 2: samplers. The Config Tool exposes this as
    // one checkbox, all four or none; the split survives so a Lab session can isolate a kind.
    // On by default. These objects are immutable, so handing back the one the game asked for last
    // time is what the render loop should have done in the first place; the risk is not in caching
    // them but in the port creating them per frame.
    inline int iLevel = 3;

    constexpr int kObjects = 1;
    constexpr int kSamplers = 2;

    inline bool Wants(int bit) { return (iLevel & bit) != 0; }

    // True when the object the game is asking for is already held; `out` then carries a reference
    // of its own and the game's create call must be skipped.
    bool Serve(const void* device, const D3D11_SAMPLER_DESC& d, IUnknown** out);
    bool Serve(const void* device, const D3D11_DEPTH_STENCIL_DESC& d, IUnknown** out);
    bool Serve(const void* device, const D3D11_BLEND_DESC& d, IUnknown** out);
    bool Serve(const void* device, const D3D11_RASTERIZER_DESC& d, IUnknown** out);

    // Takes a reference of its own. Pass the description the GAME asked for, not one our render
    // policy rewrote, so the next identical request is recognised.
    void Keep(const void* device, const D3D11_SAMPLER_DESC& d, IUnknown* made);
    void Keep(const void* device, const D3D11_DEPTH_STENCIL_DESC& d, IUnknown* made);
    void Keep(const void* device, const D3D11_BLEND_DESC& d, IUnknown* made);
    void Keep(const void* device, const D3D11_RASTERIZER_DESC& d, IUnknown* made);

    uint64_t Hits();
    size_t Held();
}
