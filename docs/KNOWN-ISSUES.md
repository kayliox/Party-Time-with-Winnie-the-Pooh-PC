# Known issues

| Issue | Details |
|---|---|
| Edges of some boards on very wide screens | The boards were built for a 4:3 view, so on 21:9 and wider screens there is sometimes nothing to draw at the far left and right. The fix fills those areas with a soft blur of the picture (F1 > Graphics > "Fill the gaps at the screen edges"); with the fill off they are black. |
| Movies | The game's movies are 320 x 240 videos at 25 to 30 frames per second, scaled up to your screen by Windows' video player. They cannot look sharper than that. |
| Exclusive fullscreen | Tested less than borderless fullscreen. If your monitor refuses the chosen mode, the game falls back to borderless. |
| Some settings need a restart | Window mode, resolution, refresh rate, aspect, MSAA and supersampling: the game lays out its menus once at its drawing size. The overlay offers a **Restart now** button. |
| MSAA with supersampling | MSAA only applies when supersampling and the side fill are both off (Direct3D 8 cannot multisample the picture the fix draws into). Supersampling smooths edges anyway. A Direct3D 9 mode that lifts this limit is in development. |
| Hidden cheat untested | The title-screen cheat was found in the game's code and needs a gamepad; it has not been confirmed on a real pad yet. |
| Other editions | Only the UK English PC CD has been tested. Other editions may work in part; please report your results. |

Report new problems as an issue, with `spartacompat.log`, `spartacompat.prev.log` and `fix-install.log` from the game
folder.
