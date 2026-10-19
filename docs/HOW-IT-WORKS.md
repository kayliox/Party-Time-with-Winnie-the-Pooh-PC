# How the fix works

## What it is (and what it is not)

The fix is a **compatibility layer**. Your original game program (`WTP.exe`, from your CD) runs on your computer
exactly as it did in 2002. The fix is one extra file, `d3d8.dll`, placed next to the game. Windows loads it instead of
its own Direct3D 8 file, so the fix sits between the game and Windows and can change how the game talks to the
graphics card, the keyboard and gamepads, the CD drive and the registry. A few dozen bytes of the game are adjusted in
memory while it runs (never on disk), and each change first checks that the game's original bytes are exactly the
expected ones.

It is **not** a recompilation, a decompilation, a remake or an emulator. No game code is rewritten or included. (The
game was studied with a disassembler to find the places to adjust; that work is described below.)

## The parts

| Part | What it does |
|---|---|
| Virtual CD | The game looks for its CD in a drive. The setup copies the CD's `Datas` folder into `cd\Datas` in the game folder, and the fix answers the game's drive checks with that folder, so no disc is needed. |
| Settings without the registry | The old launcher stored options in the Windows registry. The fix serves them from `registry.ini` in the game folder instead, so nothing is written to your registry and the game folder is portable. |
| Window and resolution | The game only knows 640 x 480 to 1024 x 768. The fix opens a borderless window (or exclusive fullscreen) at your monitor's resolution and tells the game to draw at that size. |
| Widescreen (hor+) | The game draws its 3D in a 4:3 picture. The fix widens the picture: the 3D fills the full width, the game's 3D culling and view frustum are widened so objects at the sides are drawn, 2D screens stay centred, full-screen fades are stretched, and the score panels move towards the corners. |
| Supersampling | The game draws into a picture twice as wide and high as your screen, which is then averaged down: smooth edges on everything, including cut-out trees and sprites. |
| Side fill | Some boards were built only for a 4:3 view, so at the very edge of a wide screen there is nothing to draw. The fix fills those gaps with a soft blur of the picture instead of black. |
| Frame rate | The game measures time in whole milliseconds and its game logic runs in 1/120 s steps. The fix paces frames precisely and feeds the game exact time steps, so it runs at 120 fps at the right speed (higher rates work too). |
| Input | Gamepads are detected in the background (the original detection could stall the start for a minute). |
| Autosave | The fix watches the game's in-memory profile. When your progress changes, it calls the game's own save routine (the same one the pause menu uses), and again when the game window closes. It checks the save file first and never writes over another profile. |
| Settings overlay (F1) | Drawn with Dear ImGui over the finished picture. Changes are written to `spartacompat.ini` at once. |
| HD textures (optional) | When a texture pack folder exists, every texture the game uploads is identified by a fingerprint of its pixels and replaced by an HD version from the pack. See [HD-TEXTURES.md](HD-TEXTURES.md). |
| Movies | The game plays its movies with Windows' DirectShow; the fix centres and scales the video window for wide screens. |

## Where the game-specific knowledge lives

All addresses, byte patterns and options for this game are data in `games/PC-DPTWP-UK/`:

- `spartacompat.ini`: the fix's settings plus the game-specific patches, each with the original bytes it expects;
- `registry.ini`: the old launcher's default options (keys, number of players);
- `profile.json`: what the setup copies from the disc, the exact version (SHA-256 of `WTP.exe`), and test scenarios;
- `save-format.json`: the save file layout used by the save editor.

The layer's code in `native/pc-compat` is general; a new game would get its own folder under `games/`.

## Checks that guard it

- Every byte patch and the autosave check the game's original bytes before doing anything; a different version of the
  game simply runs without them.
- An automated test run (`scripts/pc_regression.py`) plays scripted sessions in a hidden window and checks the logs,
  saves and the game's own frame dumps: menus and a race, a board walk, saving and loading, unlocks, the overlay and
  autosave.
