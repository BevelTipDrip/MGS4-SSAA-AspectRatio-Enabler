#pragma once

// Lab: the wide canvas the engine's own way. The game's camera/UI structure carries the canvas
// size (uint16 pair), the UI offset (int16 pair) and the inverse aspect; telling it the canvas
// is `units` wide (650 at 21:9) with the UI offset by (units - 480) / 2 makes the engine
// centre the 480-unit UI and fill the wider scene itself. The per-effect fixups are the
// elements anchored to the canvas edges or size (colour filter, damage overlay, glare,
// grenade arc, hit markers, pickup markers, loading and movie viewports, the scissor).
// This mirrors the hook set of Afevis's MGSPW-Resolution-Unlocked on our integer render
// scale, which keeps the post effects intact. Sites are RVAs verified against the clean dump
// (pw/tools/afevis_sites.py); each hook checks its bytes first.
namespace WideCanvas
{
    // units: canvas width in PSP units (480 = off); scale: the integer render scale in use.
    void Install(int units, int scale);
    bool Active();
    int Units();   // canvas width in units (480 when off)
}
