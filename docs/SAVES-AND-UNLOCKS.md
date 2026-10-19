# Saves, unlocks and the hidden cheat

## Saving

- The game saves from **Adventure > Pause > Save game**. The fix makes **Yes** the default answer of the save prompt.
- The fix's **autosave** saves your profile a moment after your progress changes (a new profile, every board round)
  and when the game window closes (also with Alt+F4). It never writes over another profile. Switch it off in
  **F1 > Game** if you prefer the original behaviour.
- The save file is `DPTWP.sav` in the game folder (8 KB, up to 4 player profiles). Copy it to back it up.

## What can be unlocked

From the game's manual and its code:

| Content | At the start | How it unlocks |
|---|---|---|
| Quick Play mini-games (5) | 2 of 5 | Play Adventure Mode |
| Playground levels 1 to 5 of each mini-game | Playgrounds 1 and 2 of two mini-games | Progress in Adventure Mode (a human player must win the board) |
| Game boards 2 and 3 in Multiplayer | Board 1 | Reach them in Adventure Mode |
| Adventure boards 2 and 3 | Board 1 | Win the previous board |
| Movie clips (Movie Selection) | None | Do well in Adventure Mode (each board's ending unlocks clips) |
| Characters, difficulty levels, hazards, credits | All available | Nothing to unlock |

## The hidden cheat

The game contains a cheat that no guide documents. On the **title screen**, press **Up, Down, Left, Right, Cross** in
that order (Cross is the gamepad's bottom face button). The game then unlocks all mini-games and Playgrounds for every
**new** profile you create in that session, and Multiplayer boards 2 and 3. Existing profiles are not changed and the
cheat is not saved. It reads the gamepad only, so use a gamepad for it.

This was found in the game's code and has not yet been confirmed on a real gamepad. If you try it, please report
whether it worked. (Without a gamepad, the save editor below gives the same result.)

## Save editor (unlock everything in an existing save)

`scripts/pc_save_edit.py` (Python 3) shows the profiles in a save and can unlock everything for one of them. Close the
game first; the editor makes a backup copy before it changes anything.

```
python scripts\pc_save_edit.py show  --serial PC-DPTWP-UK --save "%USERPROFILE%\Games\Party Time with Winnie the Pooh\DPTWP.sav"
python scripts\pc_save_edit.py apply --serial PC-DPTWP-UK --save "%USERPROFILE%\Games\Party Time with Winnie the Pooh\DPTWP.sav" --preset unlock-all --name YOURNAME
```

Presets: `unlock-all` (mini-games, Playgrounds, boards, movies) and `unlock-all-boards`. The save layout is described
in `games/PC-DPTWP-UK/save-format.json`.

Sources: the game's own manual and code; public cheat sites list no unlock codes for this game (GameFAQs, PC and PS1
pages, checked October 2026).
