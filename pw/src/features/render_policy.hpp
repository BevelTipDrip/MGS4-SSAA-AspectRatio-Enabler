#pragma once

// What the rendering settings do at the Direct3D level, in one place for both builds: the
// Lab census calls these from its hooks, the Release build's lean hooks (render_hooks.cpp)
// call the same functions, so the two cannot drift. The knobs are InternalSize's.
namespace RenderPolicy
{
    // MSAA off: the scene targets single-sampled. GPU-local: CPU write access stripped from
    // the game's large default textures (its per-frame copies into them then stay in video
    // memory). Returns true when `desc` was changed; `stripped` says the CPU flag was dropped.
    bool TextureDesc(D3D11_TEXTURE2D_DESC& desc, bool& stripped);

    // Anisotropic filtering: every linear, non-comparison sampler at the set level. Returns
    // true when `desc` was changed.
    bool SamplerDesc(D3D11_SAMPLER_DESC& desc);

    // Exclusive chains ask for the display's current refresh rate instead of the game's 60/1.
    void SwapChainDesc(DXGI_SWAP_CHAIN_DESC& desc);

    // After a windowed chain exists: Windowed mode sizes the window's client to the selected
    // screen resolution and centres it (the game's WM_SIZE handling resizes the chain).
    void AfterSwapChain(HWND window, const DXGI_SWAP_CHAIN_DESC& desc);

    // MSAA off: a resolve of a single-sampled source must be a copy instead.
    bool ResolveAsCopy(ID3D11Resource* src);
}
