# Terranigma Recompiled release notes

## v0.1.0 (first preview) - Germany, Rev 1 (PAL)

Target ROM: `Terranigma (Germany) (Rev 1).sfc` (CRC32 `6fb8a4df`). You need your own copy; no game data is included.

- **16:9 widescreen** (398x224, presentation only): the world, map edges, rooms and menus are widened from the game's own map data, with an automatic clamp fallback for scenes that do not fit.
- **No black strips at the old 4:3 edge:** the game's dark BG3 border columns are hidden, including while text boxes open and close.
- **Characters and objects outside the old 4:3 area** now appear in the side margins (object-list and OAM culling widened). Disabled inside rooms, so neighbouring rooms never show.
- **PAL region fix:** the game starts without the "not designed for your Super Famicom" screen. Timing is NTSC 60 Hz, so it runs about 20 % faster than a real PAL console.
- Mod feature `widescreen` can be switched off in the launcher; option `edge_policy` selects the margin mode (default: world-aware).

Not yet tested: world map, intro/title, Mode 7 scenes, fades, HUD in widescreen, enemy spawn distance, dialogs at the margins. Save states are not affected by the mod.
