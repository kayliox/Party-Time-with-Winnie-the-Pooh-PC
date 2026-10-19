# Building from source

Everything builds on Windows 10/11 without Visual Studio, with the free llvm-mingw compiler (Clang). Nothing needs
admin rights.

## Build the fix and the setup

```
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchains.ps1 -Only llvm-mingw
powershell -ExecutionPolicy Bypass -File scripts\build-pc-release.ps1 -Version 1.0.0
```

The first command downloads the pinned llvm-mingw release into `%USERPROFILE%\Tools` and checks its SHA-256
(`toolchains/toolchains.json`). The second builds:

- `build\pc-compat\d3d8.dll`: the compatibility layer (32-bit, statically linked). Its build script fetches the pinned
  Dear ImGui release (checked by commit) into `build\pc-compat-deps`.
- `build\pc-installer\PartyTimePooh-PCFix-Setup.exe`: the setup, with the layer, `spartacompat.ini`,
  `registry.ini`, the licences and the player's notes embedded. The game facts it uses (which files to copy, the
  supported `WTP.exe` SHA-256, names) are generated from `games/PC-DPTWP-UK/profile.json`.

## Layout

| Folder | Contents |
|---|---|
| `native/pc-compat` | The compatibility layer (proxy `d3d8.dll`): display, widescreen, supersampling, textures, input, virtual CD and registry, autosave, the F1 overlay |
| `native/pc-installer` | The setup program: disc and image reading (ISO 9660 / Joliet, .iso and raw .bin), version check, copying, update mode, shortcuts |
| `games/PC-DPTWP-UK` | Everything specific to this game: settings and patches, launcher defaults, save layout, setup texts, test scenarios |
| `scripts` | Build scripts, the save editor, the texture-pack builder, the automated test runner |
| `docs` | Player and developer documentation |

## Unattended setup (for testing)

```
PartyTimePooh-PCFix-Setup.exe /source <CD drive, folder or disc image> /target <folder> /quiet [/noshortcuts]
```

Exit code 0 means installed or updated; details are in `<folder>\fix-install.log`. With an existing prepared copy as
the target, the setup only updates the fix (no source needed).

## Tests

```
python scripts\test_pc_save_edit.py
python scripts\test_pc_regression.py
```

The automated play test needs a **separate test copy** of the game (it deletes saves): make one with the unattended
setup, then

```
python scripts\pc_regression.py --serial PC-DPTWP-UK --game <test copy>
```

The setup itself is checked against your own disc image (installs from .iso, a raw .bin made from it and optionally a
folder, each compared file by file with a reference copy; updates; wrong sources):

```
python scripts\pc_installer_check.py --setup build\pc-installer\PartyTimePooh-PCFix-Setup.exe --iso <your disc image> --reference <a prepared copy> --work <scratch folder>
```

The regression runner copies the freshly built layer and the profile's settings into the test copy, plays the scenarios from
`games/PC-DPTWP-UK/profile.json` in a hidden, muted window and checks the logs, saves and the game's own frame dumps.
Results (logs and frames from your copy of the game) are written next to the test copy, never into the repository.
Requires Python 3 with Pillow.

## Notes on the code

References such as `(O-121)` or names like `pc.textures.stale-entry-overread` in comments point to entries of the
author's development log and lesson notes, which are not part of this repository; the comment next to them says what
was learned.

## Rules for contributions

- Never add game files or anything made from them (executables, data, movies, music, textures, texture packs, frame
  dumps, logs that contain them). `scripts/audit_public.py` checks the repository for that before every release.
- Game-specific knowledge belongs in `games/<ID>/` as data, not in the layer's code.
- Every byte patch states the original bytes it expects, so a different game version is never changed blindly.
