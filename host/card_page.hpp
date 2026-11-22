#pragma once

// Local change (the remaster): the Memory cards page's actions (host/quickmenu: Page::cards), shared by the game window and the
// launcher. What "a slot" is comes from the caller: in the game a card is swapped live (ps2::swap_memory_card); in the launcher no game
// runs and a slot is only the card name kept in remaster.ini for the next start (file_slots).

#include "quickmenu.hpp"

#include <functional>
#include <string>

namespace cardpage {

struct Slots {
    std::function<std::string(int slot)> in_slot;                                        // slot 0 / 1 -> card name, "" = empty
    std::function<bool(int slot, const std::string &file, std::string *why)> put;          // file "" = take the card out
    std::function<bool(int slot, const std::string &to_file, std::string *why)> rename_in_slot;
    std::function<void()> flush;                                                          // before copying a card that is in a slot
    std::function<void()> remember;                                                       // the slots changed: write remaster.ini
};

void fill(quickmenu::Values &v, const std::string &dir, const Slots &slots);
// what the page asked for; the answer is the line for the page's description box ("" = nothing to say)
std::string act(const quickmenu::Action &a, const std::string &typed, const std::string &dir, const Slots &slots);
// The launcher's slots: the names live in `names` (the caller loads them from remaster.ini; `remember` writes them back).
Slots file_slots(std::string (&names)[2], std::function<void()> remember);

}  // namespace cardpage
