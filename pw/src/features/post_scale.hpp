#pragma once

#include "lab.hpp"

// Lab-only: the game's end-of-frame resolve, scene target -> post-chain target, replaced by
// a GPU downscale draw when the two differ in size. With the post targets (the "internal
// size" sites) set smaller than the scene (the render scale), every post pass then runs at
// the smaller size and the final blit is closer to 1:1. The draw is a full-screen triangle
// with a box filter made of bilinear taps, recorded on whichever context issued the resolve,
// with that context's state saved and restored around it.
namespace PostScale
{
#if MGS4E_LAB_BUILD
    // Returns true when the resolve was replaced by a downscale draw (sizes differ, single-
    // sampled source, resources ready); false to let the original resolve run.
    bool Downscale(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src);
    void SetDevice(ID3D11Device* device);
#else
    inline bool Downscale(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*) { return false; }
    inline void SetDevice(ID3D11Device*) {}
#endif
}
