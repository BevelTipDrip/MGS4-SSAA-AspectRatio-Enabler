# Display mode: DirectX version and window mode

**Settings:** `DirectX Version`, `Window Mode`
**Implements:** `asi/src/features/render_pipeline.cpp` (`InstallDisplayModeOverride`),
`asi/src/core/config.cpp` (`ReadDisplayMode`)

## What it does

Overrides two of the game's own options at every start: which renderer it uses (DirectX 11 or
12) and how its window is presented (exclusive fullscreen, borderless, windowed). The game
keeps both outside its config files - the API in `mgs4.savedsettings` under the Steam-synced
save folder, the window mode in a settings block the options screen edits - and this feature
never writes either file. It changes what the engine reads.

| `DirectX Version` | `Window Mode` | Effect |
| --- | --- | --- |
| `Use Game Setting` | `Use Game Setting` | nothing is hooked |
| `DirectX 11` / `DirectX 12` | | the config store's `render.api` is rewritten to `dx11` / `dx12` after the saved settings load |
| | `Fullscreen` | engine mode 0, `full_exclusive`: the game's own default look (a frameless window the size of the display; under DirectX 12 it also switches the display mode) |
| | `Borderless` | engine mode 1, `full_borderless`: a frameless window the size of the display, no mode switch |
| | `Windowed` | engine mode 2: a captioned window at the in-game resolution (or the Window Resolution override) |

## How the game decides (measured 2026-09-07, lab string scan and disassembly)

Strings in the executable: `render.api` (default `dx12` seeded at `+3FD52`), `dx11`, `dx12`,
`render.fullscreen`, and the mode names `full_exclusive`, `full_borderless`, `windowed` at
`+40640` (a name-from-mode helper: 0, 1, else).

**The config store** (`+2286510`) holds variant entries: value at +0, type tag at +0x20
(1 bool, 2/3 integer, 4 double, 5 `std::string` with data or pointer at +0, size +0x10,
capacity +0x18). `+13DF50(key)` looks an entry up by name; `+13B400(entry)` is the string of a
tag-5 entry; `+13B210(entry)` reads a bool (tag 1: the byte at +0; tag 5: compares with
"true"); `+13E1A0(key, std::string*)` sets a string.

**The saved-settings loader** `+65D880` (prologue `48 89 5C 24 18 55 48 8D 6C 24 A9 48 81 EC
B0 00 00 00 48 8D 1D`) runs at boot: if `mgs4.savedsettings` exists it loads it into a
temporary store, reads its `api`, validates against `dx11`/`dx12` (a global flag at `+3BD1152`
forces `dx12`) and writes the winner into the global store's `render.api` (`+65D9EF`). It then
rebuilds the current-settings block: the window mode from `+4AF970`, which maps the
saved-settings block's value at `+3B7A42C` (0 borderless, 1 exclusive, 2 windowed) to the
engine's numbering (1, 0, 2); the API word from `render.api` (0 dx11, 1 dx12); the fullscreen
bool from `render.fullscreen`; then copies the block to globals, the mode word first
(`+65DD34` -> `+3BD1150`), and validates the windowed size against the display modes.

**Who reads what afterwards.** The renderer start (`+660100`) takes `render.api` (0 for dx12,
3 for dx11, and a `vulkan` branch) and `render.fullscreen` straight from the store. The
options screen shows the store's `render.api` (`+C6380`). The mode word has a getter
`+65D7B0` (`movzx eax,word ptr [+3BD1150]; ret`) that the boot code asks four times while
creating the window (`+3CC81..+3CCC6`) - *before* the loader returns - and the helpers
`+40640` (name) and `+40690` (is windowed: mode == 2) go through it too. Direct readers of the
word: `+65C9CB`, `+65E22C`, `+65E2E8`, `+65F062`; the options-apply path writes it
(`+65F08B`, `+65F09A`).

A first cut hooked a different function that reads both keys, `+65C140`, at the point where
its API and mode words are final. It installed but never fired: that function is not on the
boot path (it is the options screen's re-read). The lab's `Find References` key was added
for this: every rip-relative operand or near call/jmp to a given RVA.

## The override

`InstallDisplayModeOverride` finds the loader by its prologue, the `render.api` read inside it
(`lea rcx,[render.api]; call +13DF50; mov rcx,rax; call +13B400`, which also yields the two
accessors), and the mode-word copy (`movzx eax,word ptr [rsp+40h]; mov [word],ax`). It then:

1. hooks the mode getter `+65D7B0` (found as `0F B7 05 ?? ?? ?? ?? C3` padded, whose target is
   that word) to return the chosen mode from the first call - this is what makes the window
   come up right, since the window is created before the loader returns;
2. wraps the loader: after it returns, rewrites the store's `render.api` string in place
   (both values are four characters, so size and capacity are untouched), sets the store's
   `render.fullscreen` bool to whether the mode is exclusive fullscreen, and writes the mode
   word.

Log lines: `Display Mode: override installed on the saved-settings loader at +65D880 (mode
word at +3BD1150; DirectX 11, window mode windowed)`, `window-mode getter at +65D7B0
hooked`, then per boot `the game chose dx12; running dx11 instead` and `the game chose
fullscreen; running windowed instead`.

**Verified** (lab, 1600x1200 display, saved setting dx12 fullscreen): DirectX 11 + Windowed
gave a captioned 1616x939 window with a 1600x900 client (the Window Resolution override,
capped to the display) and a D3D11 device; DirectX 12 + Borderless a frameless 1600x1200
window (style `0x17000000`, no caption, no popup bit, not topmost) and a D3D12 device;
DirectX 12 + Fullscreen a topmost popup window (style `0x96000000`, `WS_POPUP` and
`WS_EX_TOPMOST`), the game's own exclusive presentation, with the chain resize of AR-014.
The three are told apart by those style bits; on a display the size of the window the
picture looks the same in all of them.

## Side effect to know

The game writes `mgs4.savedsettings` itself on every exit, from the store. With
`DirectX Version` set, the file therefore comes to say the overridden API (seen: `api=dx11`
after a DirectX 11 run), and clearing the override later leaves the game on whatever ran
last. That is the game's own save, not a write by this mod, and the options screen agrees
with it; it is mentioned in the tool's help text. The window mode is not persisted this way
(the saved-settings block's value is untouched).

---

## Active bugs

None recorded.

## Retracted / unverified

- Whether the engine's `full_exclusive` mode is a true DXGI exclusive fullscreen on DirectX 11
  was not checked; on DirectX 12 the display-mode switch observed in AR-014 is that mode's.
