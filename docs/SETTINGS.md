# Settings file reference

Everything you can change in the **F1** overlay is stored in `spartacompat.ini` in the game folder, and you can also
edit that file with Notepad while the game is closed. Each setting is explained by a comment in the file itself. The
most useful ones:

| Section | Setting | Values | Meaning |
|---|---|---|---|
| Display | `Mode` | `borderless`, `fullscreen`, `windowed` | Borderless fullscreen (default), exclusive fullscreen, or a window |
| Display | `Width`, `Height` | `0` = your monitor | Resolution the game draws at |
| Display | `Refresh` | `0` = default | Refresh rate for exclusive fullscreen |
| Display | `Aspect` | `hor+`, `4:3`, `stretch` | Widescreen, original picture with black bars, or stretched |
| Display | `FpsLimit` | `120`, `60`, `144`, ..., `-1` = monitor, `0` = unlimited | Frame rate limit (120 is the game's own logic rate) |
| Display | `Supersample` | `2` or `1` | Draw at twice the resolution for smooth edges (needs a restart) |
| Display | `MSAA` | `0`, `2`, `4`, `8` | Multisampling; only used when `Supersample=1` and `SideFill=black` |
| Display | `Anisotropy` | `0`, `1`, `2` ... `16` | Texture filtering (0 = the game's own) |
| Display | `SideFill` | `ambient`, `black` | What fills the screen edges where a board has nothing to draw |
| Display | `HudSpread` | `1`, `0` | Move the score panels towards the corners on wide screens |
| Display | `RunInBackground` | `0`, `1` | Keep running when another window is in front |
| Textures | `Enable` | `1`, `0` | Use the HD texture pack if there is one |
| Textures | `SoftEdges` | `1`, `0` | Smooth text edges with an HD pack |
| Textures | `Dump` | `0`, `1` | Collect textures the pack does not have (needs a `textures` folder) |
| Autosave | `Enable` | `1`, `0` | Autosave on or off |
| Overlay | `Enable` | `1`, `0` | The F1 overlay |
| Overlay | `FpsCounter` | `0`, `1` | Small frame counter in the corner |

The sections `[Patches]`, `[Hooks]`, `[Game]`, `[Watch]` and the address keys of `[Autosave]` describe how the fix
adapts this exact version of the game. Do not change them unless you know what they do; every patch checks the
original bytes it expects before it changes anything.

If the file gets broken, run the setup again on the same folder (**Update fix**): it writes a fresh file and keeps the
settings listed in the table above.
