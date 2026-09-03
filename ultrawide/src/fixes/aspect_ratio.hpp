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

    // Layer 1. In a normal launch the Config Tool's "Ultrawide HUD Fix" sets this together
    // with bAspectPoolSpread; the lab settings file drives them separately.
    inline bool bFixUiAspect = false;

    // Layer 2: spread widget anchors so corner widgets sit at the margins they have on 16:9
    // instead of huddling in the centered area. Separate switch so the layers can be tested
    // independently; does nothing without bFixUiAspect.
    inline bool bFixUiAspectCorners = false;

    // Diagnostic: logs each UI channel's descriptor-table entry as the payload assembler
    // emits it, labeling the channels for the producer-based positioning plan. F6 re-arms.
    inline bool bLogUiChannels = false;

    // Diagnostic: logs every .la2 UI layout file the game reads, with each file's parsed
    // layer table (strcodes, status, object counts) - the identity map for the
    // producer-based positioning.
    inline bool bLogLa2Loads = false;

    // Diagnostic: with bLogLa2Loads, also write each la2's bytes to C:\mgspf_tools\la2_dump
    // so the files can be parsed and previewed outside the game.
    inline bool bDumpLa2 = false;

    // Diagnostic: bias the logical range of the one layout object whose la2 strcode matches,
    // so a screenshot shows exactly which screen elements that file governs. 0 disables.
    inline uint32_t uAspectTestStrcode = 0;

    // Runtime re-apply of the bottom-corner anchors. Off by default: it renders the HUD
    // corners but drags the same content's menu and selector copies with it, and has
    // crashed in play. A lab/experiment switch until context discrimination is solved.
    inline bool bAspectRuntimeAnchoring = false;

    // Draw-time corner anchoring: shift widget anchors in the per-frame instance pool at
    // upload, with wheel-state awareness read from the pool itself (AR-008) - an open
    // selector's whole cross moves with its box so the wheel stays rigid. Also restores
    // the world-tracked decompression (Solid Eye labels). Experimental; intended to be
    // run with Fix UI Aspect Corners=0 so the conversion-time table does not double-shift.
    // This is the shipping layer 2 (see bFixUiAspect).
    inline bool bAspectPoolSpread = false;

    // Diagnostic: bias every distinct la2's objects by index x 12 logical units, so one run
    // maps every visible widget to its host object by measured shift.
    inline bool bAspectTestLadder = false;
}
