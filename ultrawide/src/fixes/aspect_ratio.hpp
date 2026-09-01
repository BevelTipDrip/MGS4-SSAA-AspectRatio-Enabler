#pragma once

// Aspect ratio correction for the game's UI.
//
// The game lays out its entire interface in a logical 1280x720 space and maps that space onto
// the full output, whatever the output's aspect - which stretches the HUD at anything other
// than 16:9. The mechanism, measurements and design live in docs/features/aspect-ratio.md.
// The research diagnostics that established them live in resolution_scaling_fixes.cpp.
namespace AspectRatio
{
    // Hooks the UI layout converter so every layout object maps an aspect-corrected logical
    // range instead of a stretched one. Widgets keep 16:9 proportions at any output aspect;
    // at wider-than-16:9 the interface sits centered, at narrower it sits vertically centered.
    // Applied only when the setting asks for it.
    void ApplyFixes();

    // Experimental, off by default and absent from the Config Tool while the second layer
    // (anchoring corner widgets to the real corners) is still being built.
    inline bool bFixUiAspect = false;

    // Layer 2: spread widget anchors so corner widgets sit at the margins they have on 16:9
    // instead of huddling in the centered area. Separate switch so the layers can be tested
    // independently; does nothing without bFixUiAspect.
    inline bool bFixUiAspectCorners = false;
}
