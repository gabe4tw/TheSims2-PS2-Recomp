#pragma once

// Local change (the remaster's quick menu): the game's own menu sounds - moving, choosing, going back - played while the quick menu
// is open (the game is held then and makes no sound itself). The clips are taken from the game's sound memory once
// (C:/PS2Native/re/extract_ui_sounds.py) and kept as 48 kHz stereo 16-bit WAV files in <game folder>/menu/sounds: move.wav,
// select.wav, back.wav. Without them the menu is silent. PS2_MENU_SOUNDS=0 turns them off.

#include <cstddef>
#include <cstdint>
#include <string>

namespace menusound {

enum class Clip { move, select, back, count_ };

// reads the clips of a folder (UTF-8 path); false when none was found
bool load(const std::string &folder);
// from any thread: starts a clip (a clip that still plays is cut off)
void play(Clip clip);
// the sound device's thread: adds the playing clip to `frames` stereo frames of 48 kHz 16-bit sound
void mix(std::int16_t *stereo, std::size_t frames);

}  // namespace menusound
