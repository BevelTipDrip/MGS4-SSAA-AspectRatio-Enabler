#pragma once

// Aspect ratio correction for the game's UI.
//
// The game lays out its entire interface in a logical 1280x720 space and maps that space onto
// the full output, whatever the output's aspect - which stretches the HUD at anything other
// than 16:9. The mechanism, measurements and design live in docs/features/aspect-ratio.md.
// The research diagnostics that established them live in render_pipeline.cpp.
namespace AspectRatio
{
    // Hooks the UI layout converter so every layout object maps an aspect-corrected logical
    // range instead of a stretched one. Widgets keep 16:9 proportions at any output aspect;
    // at wider-than-16:9 the interface sits centered, at narrower it sits vertically centered.
    // Applied only when the setting asks for it.
    void ApplyFixes();

    // Layer 1. In a normal launch the settings tool's "Ultrawide HUD" choice sets this alone
    // (Centered HUD) or together with bAspectPoolSpread (Expanded HUD); the lab settings
    // file drives them separately.
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

    // World-tracked labels (pickup names, Solid Eye stat blocks, targeting icons): translate
    // each group back onto the object it marks, undoing layer 1's horizontal compression for
    // widgets that are projected from 3D positions rather than laid out on the 16:9 canvas.
    // The Config Tool's "Solid Eye Overlay Fix"; shares the pool hook with bAspectPoolSpread
    // but is switchable on its own, so a centered HUD can keep its labels on target. Does
    // nothing without bFixUiAspect.
    inline bool bAspectWorldTracking = false;

    // Diagnostic: bias every distinct la2's objects by index x 12 logical units, so one run
    // maps every visible widget to its host object by measured shift.
    inline bool bAspectTestLadder = false;

    // The 16:9 frame on a non-16:9 output (AR-010): black bands over the region outside the
    // centred 16:9 rect - side pillars on a wide output, top and bottom bands on a tall one.
    // Three switches, the settings tool's "Menu Masking", "Cutscene Masking" and "Cutscene FOV
    // Compensation"; the lab settings' "Aspect Letterbox" turns all three on.
    //
    // Menu masking: the bands are up over the boot sequence up to the continue press on a
    // loaded save (the boot ladder), and over any tabled screen element (kScreenIdentities:
    // title, main menu, pause menu, codec). Needs bFixUiAspect - the menus have to be drawn
    // in the 16:9 frame before it makes sense to mask outside it.
    inline bool bMenuMasking = false;

    // Cutscene masking: the bands are up while a cinematic camera is being built (structural
    // - no state flag, no stage name), except over the post-load grace. Does not need the
    // HUD fix: the ladder still runs for the grace when the pool hook is installed, and
    // degrades to the camera-alone fallbacks without it.
    inline bool bCutsceneMasking = false;

    // Cutscene FOV compensation: divide the cinematic camera's projection scale by the
    // horizontal spread factor - a uniform zoom-out that puts the authored 16:9 frame back
    // inside a wide window (the engine's cutscene camera is Vert- otherwise). With the
    // cutscene masking that frame is what the bands surround; without it the cutscene is
    // simply shown wider than authored. A no-op on outputs that are not wider than 16:9.
    inline bool bCutsceneFovCompensation = false;

    // Gameplay FOV: the gameplay camera's view width relative to stock, as the settings tool's
    // percentage / 100. 1.0 is stock; the projection scale on the gameplay route alone is
    // divided by it. Cutscenes are not touched by this.
    inline float fFovAdjustment = 1.0f;
}
