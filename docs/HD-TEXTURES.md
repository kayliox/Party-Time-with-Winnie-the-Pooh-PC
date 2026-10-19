# HD textures (advanced)

The fix can replace the game's small textures with sharper versions from a **texture pack**. A pack is made from the
textures of your own copy, so it cannot be downloaded here: the textures belong to the game. You make it yourself in
three steps. This needs a little more computer knowledge than the rest of the fix.

## 1. Collect the textures while you play

1. In the game folder, create an empty folder named `textures`.
2. Start the game, press **F1**, open the **Textures** tab and tick **Collect textures the pack does not have yet**.
3. Play normally: menus, the boards, the mini-games, Multiplayer. Every texture the game shows for the first time is
   saved to `textures\dump` (as .tga files). The more of the game you see, the more complete the pack.

## 2. Upscale them

You need [Python 3](https://www.python.org/downloads/) with Pillow (`pip install pillow`) and this repository
(download it as a ZIP from the green **Code** button and unpack it).

The included tool uses the free Real-ESRGAN upscaler (BSD licence; needs a graphics card with Vulkan, which almost all
cards from the last 10 years have). Install it once with:

```
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchains.ps1 -Only realesrgan
```

Then build the pack (replace the path with your game folder):

```
python scripts\build-texture-pack.py --dump "%USERPROFILE%\Games\Party Time with Winnie the Pooh\textures\dump" --pack "%USERPROFILE%\Games\Party Time with Winnie the Pooh\textures" --model realesr-animevideov3 --scale 4 --size-scale 128x128=8
```

`--size-scale 128x128=8` upscales the game's font sheets 8x, which makes text very crisp.

You can use another upscaler instead (for example Topaz Gigapixel): `--prepare` writes the input images, you upscale
them with your tool, and `--from-upscaled <folder>` builds the pack from the results. Run the script with `--help`.

## 3. Play with it

Start the game. The pack loads automatically; **F1 > Textures > HD textures** switches it on and off, so you can
compare. Texture packs are for your own use only: please do not share them, because they are made from the game's
artwork.
