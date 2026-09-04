#pragma once

// Config-driven patches to the game's own graphics settings.
//
// Everything here rewrites values the engine parses out of its scalability config, or the
// instructions that consume them. None of it touches D3D12 - these are patches to mgs4.exe.
// The UI coordinate investigation and its diagnostics live in render_pipeline.cpp.
namespace GraphicsSettings
{
    // Shadow map resolution and filter taps, FXAA and its quality level. Each is an independent
    // hook on the engine's config parsing, applied only when the corresponding setting asks
    // for it.
    void ApplyShadowAndAntiAliasing();

    // Widens the 16-bit conversions that lose the aiming reticle above a 4095-wide render
    // buffer. Independent of the resolution override, and worth applying at any scale.
    void PatchReticleTruncation();
}
