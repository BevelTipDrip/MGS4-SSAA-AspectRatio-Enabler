MGS Peace Walker SSAA and Aspect Ratio Enabler - preview package

For METAL GEAR SOLID: Peace Walker - Master Collection Version (Steam).

What is in the zip
  mgspw\winmm.dll                 the ASI loader (Ultimate ASI Loader, see its licence file)
  mgspw\scripts\MGSPWEnabler.asi  the plugin
  MGSPWEnabler.settings           the settings, with comments
  MGS4Enabler.exe                 the Config Tool, for changing the settings without a text editor

Install
  Copy the contents of the zip into the game's install folder, the one that contains the
  "mgspw" folder (Steam: steamapps\common\MGS_PW). The settings file sits in that folder,
  next to "mgspw"; the plugin and the loader go inside "mgspw" as laid out in the zip.
  Start the game normally. A log is written to logs\MGSPWEnabler_Game.log.

Changing settings
  Run MGS4Enabler.exe from that same folder. It works out which game it is sitting in, writes
  the settings file for you, and each setting explains itself when you select it. You can still
  edit MGSPWEnabler.settings by hand if you prefer; both write the same file.

Remove
  Delete mgspw\winmm.dll and mgspw\scripts\MGSPWEnabler.asi. The game is untouched otherwise.

Notes
  - "Use Display" fits the picture to the shape of your primary display at start-up. With a
    16:9 display everything is the game's own layout, only the render resolution changes.
  - Fullscreen is the game's exclusive mode. On a monitor whose native resolution is 16:9,
    Windows offers that native mode even if the desktop is set to a 21:9 resolution; use
    Borderless there.
  - The 21:9 and 4:3 layouts move the in-mission HUD to the display edges. Known cosmetic:
    the weapon menu's white column highlight starts its slide further right than at 16:9.

What is new in this preview, and what to watch for
  - Works with the game update of 2026-09-18 (Ver. 1.3.2) and with the version before it. The
    plugin now finds its places in the game by signature instead of by fixed address, so a game
    update that only moves code no longer breaks it. The first lines of the log name the game
    build it found ("PW sites: executable linked ..."); a line saying a site was "not found in
    this build" means that one feature stayed off, please report it.
  - "Busy Wait Fix" is the reason the game no longer stutters at 60 Hz. The game spins instead
    of waiting, which costs about a whole processor core. Please report your frame time graph,
    and your processor usage before and after, if you are able.
  - Its top setting, "Everything", also lets the game's window sleep between messages. That is
    the only part that can affect other programs, so if an overlay (Steam, RivaTuner, Discord)
    misbehaves, step it down to "Render thread and ticker" and say so.
  - "NVIDIA Fast Sync" is for NVIDIA cards with G-SYNC (or any variable refresh display) at 60 Hz.
    If you still see one hitch a second with "Busy Wait Fix" on, and it goes away when you switch
    G-SYNC off, tick this. With G-SYNC and V-Sync together the NVIDIA driver holds the game to 59
    frames a second, and the game cannot run slower than 60. The option sets Vertical sync to
    "Fast" for this game only, in the driver's own per-game profile, and unticking it puts back
    what was there. It does nothing on AMD or Intel. Please report whether it cleared the hitch.
  - MOUSE, new in this build (Config Tool, Performance tab, "Mouse"). Three are on by default:
    "Mouse Late Sample" (about 15 ms less mouse latency), "Mouse Prompt Pump" (much less jitter,
    most of all with a 4000 or 8000 Hz mouse) and "Mouse Fraction Carry" (slow aiming no longer
    loses part of the movement). "Mouse Uniform Rail" is off by default: it makes looking up and
    down in the normal third-person camera as even as looking sideways, and changes the feel.
    Please try each on and off and report: latency, jitter, anything that drifts, jumps or
    double-counts, and whether Uniform Rail feels better or worse to you after a while.
  - "State Object Cache" saves roughly one frame's worth of processor work in twelve. It should
    change nothing you can see; report anything that looks wrong while it is on.
  - "Audio Prefetch" is EXPERIMENTAL and off by default. It targets the single-frame hitch at
    the start of a Codec call or voice line: the game reads each line from a large archive on
    its main thread and waits for the disk. "Voice archives at start" reads those archives
    through in the background right after launch (about 1.6 GB, which Windows gives back
    whenever anything else needs the memory). If you see that hitch, switch it on and report
    whether it goes away, and whether launch felt slower or memory got tight.
  - The first two are on by default; turning either off returns the game to how it shipped.

The aspect-ratio work in this plugin is a private component with a credit-required licence:
free to use; credit BevelTipDrip directly and visibly if you redistribute or build on it.
