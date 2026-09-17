#pragma once

// The game reads its voice and data archives synchronously, on the game thread, and the first touch
// of one costs about ten milliseconds. Measured 2026-09-16: a single 20 KB read from a .PDT archive
// took 9.72 ms while ordinary reads in the same session cost 0.04 ms, and the tick it landed in
// blocked for 13.53 ms. That is the frame time spike players see whenever a new voice line starts.
//
// It is not the scanner: excluding the whole game folder from Windows Defender changed nothing, the
// same file still costing 9.82 ms. It is not a cold file cache either, since the same archive cost
// the same ten milliseconds twice, half an hour apart. What fits is the drive waking from a low
// power state, which we cannot prevent.
//
// What we can do is pay it somewhere harmless. When the game opens an archive we read through it on
// a background thread at low priority, so the device is awake and the pages are resident by the time
// the game asks for its twenty kilobytes. Nothing is cached by us; the operating system's own file
// cache does the work, and it gives those pages back whenever anything else wants the memory.
namespace FilePrefetch
{
    // 0 off; 1 warms each archive the moment the game opens it, which covers every line after the
    // first in a conversation; 2 also walks every archive in the background from start-up, which
    // covers the first line too but pulls several gigabytes through the file cache, so it is not the
    // default and is a poor trade on a handheld.
    // Off by default: measured on 2026-09-16 and it made no difference to the spikes, which is
    // consistent with the cost being the drive waking rather than the pages being cold. Kept as a
    // Lab option so the idea can be retested on other hardware rather than re-argued.
    inline int iMode = 0;

    void Install();
    void Shutdown();

    // For the Lab census line.
    uint64_t FilesWarmed();
    uint64_t BytesWarmed();
}
