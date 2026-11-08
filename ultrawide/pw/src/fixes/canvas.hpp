#pragma once

// The wide (or tall) canvas, the engine's own way: Peace Walker's camera/UI structure carries
// the canvas size (uint16 pair), the UI offset (int16 pair) and the inverse aspect; telling it
// the canvas is W x H units (650x272 at 21:9, 480x360 at 4:3, 480x300 at 16:10) with the UI
// offset by ((W - 480) / 2, (H - 272) / 2) makes the engine centre the 480x272 UI and fill the
// larger scene itself. On top of that: the per-effect fixups for elements anchored to the
// canvas edges or size (colour filter, damage overlay, glare, grenade arc, hit markers, pickup
// markers, loading and movie viewports, the scissor), the two hard-sized 480x272 quads of the
// post chain (rewritten at upload), and the in-mission HUD register (each group moved by half
// the canvas growth toward its side, at upload of its constants).
//
// This is the hook set of Afevis's MGSPW-Resolution-Unlocked, both branches, on the integer
// render scale, which keeps the post effects intact. The sites are RVAs verified against the
// clean dump (afevis_sites.py, a lab script not in this repository); each hook checks its bytes first. Method and register:
// pw/docs/ui-elements.md and engine-notes.md.
//
// Compiled in both builds; the public tree calls it through pw/src/features/canvas.hpp.
namespace Canvas
{
    // unitsW x unitsH: the canvas in PSP units (480x272 = off); scale: the integer render scale.
    void Configure(int unitsW, int unitsH, int scale);
    bool Active();
    int Units();         // canvas width in units (480 when off)
    int HeightUnits();   // canvas height in units (272 when off)

    // A constant buffer of `size` bytes just uploaded (still writable): when it carries the UI
    // ortho and its translation row is a registered HUD group, the group's move is added.
    // Returns true when the upload was a UI one (the 480x272 ortho in its first row).
    bool OnConstantUnmap(void* data, size_t size);

    // A vertex buffer of `size` bytes just uploaded (still writable): the post chain's
    // hard-sized 480x272 quads are resized to the canvas.
    void OnVertexUnmap(void* data, size_t size);
}
