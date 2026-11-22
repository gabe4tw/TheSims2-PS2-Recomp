The Sims 2 (PAL, SLES-53718) remaster: the files a fresh install needs that may be published.

scripts/sims2_install.sh copies this folder into games/sims2 (never over files already there) before it builds the game from the
player's own disc image.

  entries.txt      code addresses of the game's program that the static translation must start from (numbers only)
  remaster.pnach   the remaster's own patch section [Aspect from memory] (the picture's shape is read from memory, so the launcher
                   and the quick menu can switch 4:3 / 16:9 / 21:9)
  menu/menu.ttf    the quick menu's and the launcher's font: Varela Round, SIL Open Font License (menu/OFL-VarelaRound.txt)

Never in this folder or anywhere in the repository (they are made on the player's PC from their disc, or downloaded at install):
  - the game's program, translated code, recorded vector unit programs (vu1/, vu0/), any file from the disc
  - the menu sounds (scripts/sims2_sounds.py takes them from the player's disc)
  - PCSX2's patch file with [50 FPS] and [Widescreen 16:9] (scripts/pnach.py fetch downloads it from github.com/PCSX2/pcsx2_patches)
  - HD texture packs (the launcher downloads them from their authors' links)
