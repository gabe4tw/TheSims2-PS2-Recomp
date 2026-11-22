# The Sims 2 PS2 Recomp

A native PC remaster of **The Sims 2 for PlayStation 2** (PAL, SLES-53718), built from **your own disc** with a static
recompiler. The game's code is translated to C++ and compiled into a normal Windows program, so it runs without an
emulator, at higher resolutions and frame rates, with a launcher, mods and HD textures.

**Not affiliated with or endorsed by Electronic Arts.** This repository contains **no game data and no game code**. You need
your own copy of the game.

> Early stage. Things change a lot, and some features below have not been tested much yet. Bug reports are very welcome.

## What you get

- **Smooth frame rates:** 50/60 fps instead of the original 25/30, and higher rates on high-refresh screens.
- **Widescreen:** 4:3, 16:9 or 21:9, with the menus and HUD kept at their proper shape instead of stretched. (21:9 remains untested)
- **A sharper picture:** the game is drawn at up to 4x the PS2 resolution, with anti-aliasing (Off / Fast / Extra) and
  picture filters.
- **HD textures:** SomberTwilight's HD texture pack, downloaded at install and switchable in Settings > Textures.
- **A launcher** that installs the game from your disc image, keeps it up to date, and holds the settings, memory cards
  and mods.
- **An in-game quick menu** (controller or keyboard) with settings, save states with named slots, cheats and the game's
  own debug settings.
- **More saves:** the game allows one story save and one freeplay save per memory card. Here you get a second card slot
  and a card library with as many named cards as you like (swappable while you play).
- **Mods:** texture packs (PCSX2 style; pictures are matched automatically), patches and settings mods, each switchable
  with a load order on the launcher's Mods page.
- **Sound fixes:** no more restarting music loops, and no build-mode stutter.
- **EyeToy with your webcam** (experimental).

## Requirements

- Windows 10 or 11, 64-bit
- A graphics card with Direct3D 12 or Vulkan
- About 3 GB free disk space
- **A disc image (.iso) of your own The Sims 2, PAL version SLES-53718.** The US version (SLUS-21265) is not supported
  yet.

## Install

1. Download `Sims2Launcher.exe` and `SDL2.dll` from the [latest release](https://github.com/gabe4tw/TheSims2-PS2-Recomp/releases/latest)
   and put them in an empty folder (the .iso can go in the same folder).
2. Start `Sims2Launcher.exe` and choose your disc image.
3. The launcher downloads its build tools and the HD textures, checks your disc, and builds the game **on your PC**
   from your disc. This takes a few minutes.
4. Press **Play**.

New versions show up as an "Update" tile in the launcher. Your saves, cards, settings and mods are kept across updates.

## Controls

The game is made for a controller, and any controller SDL recognises works. Keyboard keys can be changed in the quick
menu (keyboard F1 shows them). The quick menu opens from the controller or with its keyboard key.

## Mods

A mod is a folder (or a .zip) in the `mods` folder with a `mod.ini` (name, author, version, description) and any of:
`textures\` (a PCSX2-style texture pack), `patches.txt`, `settings.ini`. Install a .zip from the launcher's Mods page, then
switch mods on or off and set their order there. Mods must contain only their own files, never files from the game.

## Roadmap

Roughly in this order:

1. Mouse and keyboard controls in the PC Sims style (point and click, edge scrolling, clickable menus and pie menus)
2. Split screen improvements: one player on keyboard and mouse, one monitor per player, top/bottom split
3. Mod support part 2: new CAS items, hair and objects, music and sounds, lots, script mods, plus tools for modders, and
   cut/beta content (switchable in the launcher, taken from your own discs)
4. Performance: big CPU and GPU optimising, more fps on strong PCs, 60 fps on weak laptops, FSR 1 (later FSR 2/3)
5. Split screen in freeplay, maybe 4 players
6. EyeToy photos in Create-a-Sim

## Credits

- [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp) by exoteric33, built on
  [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) by Samir Nuri: the static recompiler this project is built on (MIT)
- **SomberTwilight**: the HD texture pack (downloaded from its official link)
- [PCSX2 patches](https://github.com/PCSX2/pcsx2_patches): the 50 FPS and Widescreen 16:9 patches (downloaded at install)
- [Varela Round](https://fonts.google.com/specimen/Varela+Round): the menu font (SIL Open Font License)
- [SDL2](https://www.libsdl.org/)

The Sims is a trademark of Electronic Arts Inc. All game content belongs to its owners.

## License

See [LICENSE](LICENSE). The recompiler's MIT license and copyright notices stay with its code.
