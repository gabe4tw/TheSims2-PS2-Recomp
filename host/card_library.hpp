#pragma once

// Local change (the remaster): the memory card library. Every card is a file "<folder>/<name>.ps2" in one folder (the launcher's
// --card-library, games/sims2/cards); the file name is the card's name. The quick menu's "Memory cards" page makes, names, copies and
// deletes cards and puts one into slot 1 or 2 (the swap itself is the runtime's: ps2::swap_memory_card). Which card is in which slot is
// kept in remaster.ini as PS2_CARD1 / PS2_CARD2 (a card's name, or "none" for an empty slot). No game data is needed for any of this.

#include <string>
#include <vector>

namespace cardlib {

constexpr int kNameMax = 28;  // (the same as the save state slots' names)

// The card names in the folder, sorted without regard to case. Files the runtime keeps beside a card (.fresh, .crash, .damaged-...,
// .tmp, .lock, the backups folders) are not cards.
std::vector<std::string> list(const std::string &dir);
std::string file_of(const std::string &dir, const std::string &name);
// A name a file can have: characters Windows does not allow in a file name are dropped, spaces at the ends removed, at most kNameMax
// characters; "" when nothing is left or the name would look like one of the runtime's own files.
std::string clean_name(const std::string &name);
// A random town or neighbourhood of The Sims 1 to 4 that no card has yet ("Pleasantview", or "Pleasantview 2" once all are taken).
std::string random_name(const std::vector<std::string> &taken);
// `base`, or "base 2", "base 3"... whichever no card has
std::string unique_name(const std::string &base, const std::vector<std::string> &taken);
bool exists(const std::string &dir, const std::string &name);

// A new, empty card (a 0-byte file: the runtime makes it a blank card the game offers to format).
bool create(const std::string &dir, const std::string &name, std::string &why);
bool copy(const std::string &dir, const std::string &from, const std::string &to, std::string &why);
// Deletes the card file for good, with its backups folder and the runtime's files beside it. Never for a card in a slot.
bool remove(const std::string &dir, const std::string &name, std::string &why);
// Renames a card that is in no slot (the file, its backups folder and the copies in it). A card in a slot: ps2::rename_memory_card.
bool rename(const std::string &dir, const std::string &from, const std::string &to, std::string &why);
// When the card file was last written ("17 Nov, 02:10"), "" when it has never been written (a new card)
std::string when(const std::string &dir, const std::string &name);

// At the start of a run. Makes the folder if it is missing. A folder without cards gets its first ones: `seed_file` (the launcher's
// --memcard card, if it holds anything) copied in under a random town name, which goes into slot 1, and a new empty card for slot 2.
// `want1` / `want2`: the names from remaster.ini ("" = not set: the first-start choice above; "none" = the slot stays empty). A name
// that is not a card (deleted by hand) leaves that slot empty. The result is each slot's card name ("" = empty).
struct Start {
    std::string slot[2];
    std::string note;  // what was done, for the log ("" when nothing)
};
Start prepare(const std::string &dir, const std::string &seed_file, const std::string &want1, const std::string &want2);

}  // namespace cardlib
