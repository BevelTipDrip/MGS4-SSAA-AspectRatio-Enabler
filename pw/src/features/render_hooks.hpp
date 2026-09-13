#pragma once

// The Release build's Direct3D hooks: the least the rendering settings need. Device creation
// (texture and sampler descriptions, deferred contexts), DXGI factory creation (the swap chain
// description and the window), and on every context the Map/Unmap pair (the private canvas
// module's uploads) and the resolve (MSAA off). Everything they decide comes from
// render_policy.cpp and canvas.hpp, which the Lab census shares. A Lab build does not compile
// this file: its census owns the hooks.
namespace RenderHooks
{
    void Install();
}
