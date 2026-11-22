#pragma once

// Local change (the remaster's quick menu: cheats and the game's debug settings). Reading and writing the game's main memory from the
// window thread.
//  - read_game_memory / write_game_memory: direct. Writing is meant for while the quick menu holds the game (the emulation thread then
//    waits at the end of its frame and runs no game code), which is when the menu calls it.
//  - set_forced_pokes: values written again on every refresh (runtime/src/system.cpp), for settings the player chose that the game
//    would otherwise set back (its settings are filled in at start, and some are kept per lot).

#include <cstdint>
#include <vector>

namespace ps2 {

struct GamePoke {
    std::uint32_t addr = 0;
    std::uint8_t size = 4;    // 1, 2 or 4 bytes
    std::uint32_t value = 0;
    std::uint32_t mask = 0xFFFFFFFFu;  // only these bits are written (one cheat bit in a word of them)
};

bool read_game_memory(std::uint32_t addr, void *out, std::uint32_t size);
bool write_game_memory(std::uint32_t addr, const void *in, std::uint32_t size);
void set_forced_pokes(const std::vector<GamePoke> &pokes);
// calls the game function at address with a0 and a1 once, the next time every game thread waits (system.cpp idle); for one-shot
// cheats such as the horn (0x142428 with code 10)
void request_game_call(std::uint32_t address, std::uint32_t a0, std::uint32_t a1);

// a function the emulation thread calls once per refresh (vertical blank), after the forced pokes: it may read and write game memory
// directly, the game is between frames there (host/mouse_pc.cpp: the PC mouse). nullptr removes it.
void set_frame_hook(void (*hook)());
// a full-screen game panel is up (the interface squeeze's panel mode; false while the squeeze is off)
bool ui_panel_up();

}  // namespace ps2
