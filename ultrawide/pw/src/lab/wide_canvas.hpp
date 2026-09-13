#pragma once

// Lab: the wide (or tall) canvas the engine's own way. The game's camera/UI structure carries
// the canvas size (uint16 pair), the UI offset (int16 pair) and the inverse aspect; telling it
// the canvas is W x H units (650x272 at 21:9, 480x360 at 4:3, 480x300 at 16:10) with the UI
// offset by ((W - 480) / 2, (H - 272) / 2) makes the engine centre the 480x272 UI and fill the
// larger scene itself. The per-effect fixups are the elements anchored to the canvas edges or
// size (colour filter, damage overlay, glare, grenade arc, hit markers, pickup markers, loading
// and movie viewports, the scissor). This mirrors the hook set of Afevis's
// MGSPW-Resolution-Unlocked, both of its branches, on our integer render scale, which keeps the
// post effects intact. Sites are RVAs verified against the clean dump (pw/tools/afevis_sites.py);
// each hook checks its bytes first.
namespace WideCanvas
{
    // unitsW x unitsH: the canvas in PSP units (480x272 = off); scale: the integer render scale.
    void Install(int unitsW, int unitsH, int scale);
    bool Active();
    int Units();         // canvas width in units (480 when off)
    int HeightUnits();   // canvas height in units (272 when off)
}
