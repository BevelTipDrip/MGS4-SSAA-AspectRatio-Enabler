MGS Peace Walker SSAA and Aspect Ratio Enabler - preview package (plugin and settings only)

For METAL GEAR SOLID: Peace Walker - Master Collection Version (Steam).

What is in the zip
  mgspw\winmm.dll                 the ASI loader (Ultimate ASI Loader, see its licence file)
  mgspw\scripts\MGSPWEnabler.asi  the plugin
  MGSPWEnabler.settings           the settings, with comments; edit by hand

Install
  Copy the contents of the zip into the game's install folder, the one that contains the
  "mgspw" folder (Steam: steamapps\common\MGS_PW). The settings file sits in that folder,
  next to "mgspw"; the plugin and the loader go inside "mgspw" as laid out in the zip.
  Start the game normally. A log is written to logs\MGSPWEnabler_Game.log.

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

The aspect-ratio work in this plugin is a private component with a credit-required licence:
free to use; credit BevelTipDrip directly and visibly if you redistribute or build on it.
