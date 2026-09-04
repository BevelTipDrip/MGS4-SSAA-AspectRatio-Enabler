#pragma once

// Test-loop automation built on the engine's own fast-load path.
//
// The crosshair investigation is bottlenecked on the human loop, not the analysis: every
// hypothesis costs a manual launch, a save load, a walk to somewhere with an enemy, an aim,
// and a quit. The retail build still contains its developer stage loader - only the config
// parsing that reached it was removed - so a stage can be requested directly from code.
//
// Established by cipherxof/MGS4-Debug, which reaches the same machinery by signature.
namespace StageAutomation
{
    void ApplyFixes();

    // Master switch. Everything here is diagnostic scaffolding, so it stays off unless asked
    // for, and resolving the signatures is skipped entirely when it is off.
    inline bool bEnabled = false;

    // Virtual key that requests a stage load. Default F7 - F9/F10 are already taken by the
    // crosshair probes and PIX capture respectively.
    inline int iLoadStageKey = 0x76; // VK_F7

    // Stages to load, as a comma-separated list of registry names (e.g. "s01a20l,s04a10l").
    // Each press advances to the next entry and wraps, so a whole survey runs in one launch
    // instead of one relaunch per candidate.
    //
    // Empty cycles the registry itself, skipping the "_D" entries - those are the cutscene
    // variants, which never hand control to the player and so can never hold a weapon.
    inline std::string sStage = "";

    // Log every stage in the registry once it populates. The registry is a red-black map the
    // engine builds during boot, so the list is only available after the game reaches a menu.
    inline bool bLogStageRegistry = true;

    // Drive the rest of the loop from inside the process: dismiss the confirmation prompt,
    // wait for the transition, then hold aim.
    //
    // Off by default, and it should usually stay off. It presses Enter without being able to
    // see what is on screen, and the confirmation prompt does not appear for several seconds
    // after the request - so the presses land in whatever menu is still up and navigate it.
    // The orchestrating script (mgspf_tools\run_test.ps1) can take screenshots, so it can
    // confirm the prompt is actually there before pressing anything. Prefer that.
    //
    // The transition edges are logged either way, by ReportTransitionEdges, so the script has
    // an unambiguous "the stage is loading" signal without needing this.
    inline bool bAutoSequence = false;

    // How long to hold aim once the stage settles. The diagnostics sample while it is held.
    inline int iAimHoldSeconds = 6;

    // Request the first playlist stage on its own once the registry populates, so a run needs
    // no keypress at all. Only useful once the boot-to-title path is automated too.
    inline bool bAutoStartOnBoot = false;

    // Quit the process when the sequence finishes, so a script can loop builds unattended.
    inline bool bExitAfterRun = false;
}
