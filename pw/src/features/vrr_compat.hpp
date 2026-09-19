#pragma once

struct ID3D11Device;

// NVIDIA variable refresh (G-SYNC) and Peace Walker's fixed clock (2026-09-19).
//
// The simulation runs at a fixed 60.000 Hz and cannot slow down. On an NVIDIA system with variable
// refresh active on a 60 Hz mode and V-Sync on (the game's own, or forced in the control panel),
// the driver reports "low latency mode on" for the game's device and sleeps 16,948 us inside
// Present: an evenly spaced 59.00 frames a second, the value of its ceiling cap
// (refresh - refresh^2 / 3600). One frame a second has nowhere to go: a dropped frame and one
// refresh held open to ~20 ms, once a second. With Vertical sync = Fast the same driver sleeps
// 16,666 us (60.00 a second) with variable refresh still active, and the game is flat.
// NvAPI_D3D_SetSleepMode(low latency off) from the plugin was tried and is ignored by the driver,
// before and after it arms; no profile setting switches the cap off. docs/pw/frame-pacing.md has
// the measurements.
//
// Everything here goes through nvapi64.dll, loaded at run time and only when a key asks for it
// (nv_profile.hpp), so it does nothing on AMD and Intel systems and nothing at all when both
// keys are off and Fast Sync was never switched on from here.
namespace VrrCompat
{
    // Vertical sync = Fast in the driver's profile for this game only. Switching it off puts back
    // what it replaced; a Fast value that did not come from here is never touched.
    inline bool bFastSync = false;

    // Diagnostic: log what the driver says about the game's device (low latency mode, variable
    // refresh, the control panel's V-Sync override, its latest sleep interval) at device creation
    // and 46, 106 and 180 s in. With bFastSync on, the first two are logged without this key, so
    // a log always shows whether the driver took the setting for that start.
    inline bool bReport = false;

    // Called once at start-up, before the game creates its Direct3D device: the driver reads the
    // profile when it loads into the process.
    void ApplyProfile();

    // Called once, from the device hook, with the game's Direct3D 11 device.
    void OnDevice(ID3D11Device* device);
}
