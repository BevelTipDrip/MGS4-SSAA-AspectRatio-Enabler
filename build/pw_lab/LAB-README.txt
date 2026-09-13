MGS Peace Walker SSAA and Aspect Ratio Enabler - LAB build (for testers who send logs)

This is the research build of the plugin. It does everything the normal build does (the
settings are the same file, with the same keys) and adds the instrumentation that found the
plugin's performance and layout fixes. It costs a little frame rate (a lock per draw call) and
the census stalls the game for a moment, so use it to diagnose, not to play through.

What is in the zip
  mgspw\winmm.dll                 the ASI loader (Ultimate ASI Loader, see its licence file)
  mgspw\scripts\MGSPWEnabler.asi  the plugin, Lab build
  MGSPWEnabler.settings           the settings, with comments; the [Lab] section is what is new
  tools\                          Python 3 scripts that turn a census log into tables (optional)

Install
  Copy the contents into the game's install folder, the one that contains "mgspw" (Steam:
  steamapps\common\MGS_PW). Start the game normally. The log is logs\MGSPWEnabler_Game.log;
  the plugin truncates it on every start, so copy it out before launching again.

What to capture, and how
  Turn on Debug Logging in the settings first: every setting then prints what it resolved to.

  1. A census (which draws happen in a frame, with what):
     press F11 in the situation you want captured. The plugin logs the next N frames (Draw
     Census Frames, 2 by default; 12 spans a menu animation) with every draw's target,
     shaders, textures, constants, callers and vertex uploads, then "census: done". A census
     frame is several megabytes of log.
  2. GPU timing of a frame (which pass costs what):
     Time Passes is on by default; every F11 census is followed by a timing run of Time Frames
     frames (60) and a "PW timing:" table in the log: milliseconds per pass, the frame rate
     over the window, and where the time went (copies, resolves, post passes, the present).
     Press F11 during gameplay, not on a menu: menus run at 30 fps and tell nothing.
  3. Stutter (a frame that takes far longer than its neighbours):
     always on. Every such frame writes a "PW hitch:" line with how long it took, the running
     average, how many textures and buffers were created inside it, how many Map calls waited
     on the GPU, and what the previous Present cost. Play the stretch where you feel it, then
     send the log.
  4. Textures and samplers:
     always on. Every large texture the game creates is logged with its size, format, usage
     and CPU flags; every distinct sampler the game asks for is logged once.
  5. Live commands (advanced):
     with Live Commands on, the plugin reads logs\MGSPWEnabler_live.txt four times a second,
     runs each line and empties the file. "census 12" logs twelve frames. The tbias/bias
     commands move HUD elements live while a layout is being worked out (see the tools' notes).

What to send
  logs\MGSPWEnabler_Game.log from the run, and one line on what you did and when (which
  menu, which mission, what you saw). Zip it if it is large. The tools\ scripts are what we
  use to read a census; you do not need to run them.

The aspect-ratio work in this plugin is a private component with a credit-required licence:
free to use; credit BevelTipDrip directly and visibly if you redistribute or build on it.
