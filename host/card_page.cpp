#include "card_page.hpp"

#include "card_library.hpp"
#include "ps2/os.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace cardpage {

namespace {
std::string lower(std::string t) {
    for (char &c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}
std::string stem_of(const std::string &file) { return ps2::os::utf8(ps2::os::path(file).stem()); }
}  // namespace

void fill(quickmenu::Values &v, const std::string &dir, const Slots &slots) {
    v.card_library = !dir.empty();
    if (!v.card_library) return;
    for (int slot = 0; slot < 2; ++slot) v.slot_cards[slot] = slots.in_slot(slot);
    for (const std::string &name : cardlib::list(dir)) {
        quickmenu::Values::Card c;
        c.name = name;
        c.when = cardlib::when(dir, name);
        for (int slot = 0; slot < 2; ++slot)
            if (!v.slot_cards[slot].empty() && lower(v.slot_cards[slot]) == lower(name)) c.slot = slot + 1;
        v.cards.push_back(c);
    }
}

std::string act(const quickmenu::Action &a, const std::string &typed, const std::string &dir, const Slots &slots) {
    using quickmenu::Command;
    quickmenu::Values v;
    fill(v, dir, slots);
    std::string why;
    const quickmenu::Values::Card *card = a.value >= 0 && a.value < static_cast<int>(v.cards.size()) ? &v.cards[a.value] : nullptr;
    std::vector<std::string> names;
    for (const auto &c : v.cards) names.push_back(c.name);
    switch (a.cmd) {
    case Command::card_new: {
        const std::string name = cardlib::random_name(names);
        if (!cardlib::create(dir, name, why)) return "No new card: " + why;
        std::fprintf(stderr, "[cards] new card \"%s\"\n", name.c_str());
        return "New card \"" + name + "\" made. Put it into a slot with its Slot 1 or Slot 2 button.";
    }
    case Command::card_insert: {
        if (!card || a.step < 1 || a.step > 2) return {};
        if (card->slot && card->slot != a.step) return "\"" + card->name + "\" is in slot " + std::to_string(card->slot) + ". Take it out there first.";
        if (!slots.put(a.step - 1, cardlib::file_of(dir, card->name), &why)) return "Not now: " + why + ". Try again in a moment.";
        slots.remember();
        std::fprintf(stderr, "[cards] \"%s\" into slot %d\n", card->name.c_str(), a.step);
        return "\"" + card->name + "\" is in slot " + std::to_string(a.step) + " now.";
    }
    case Command::card_eject: {
        if (a.value < 1 || a.value > 2) return {};
        const std::string name = slots.in_slot(a.value - 1);
        if (!slots.put(a.value - 1, "", &why)) return "Not now: " + why + ". Try again in a moment.";
        slots.remember();
        std::fprintf(stderr, "[cards] slot %d emptied\n", a.value);
        return "\"" + name + "\" is out of slot " + std::to_string(a.value) + ".";
    }
    case Command::card_rename: {
        if (!card) return {};
        const std::string name = cardlib::clean_name(typed);
        if (name.empty()) return "That name cannot be used for a card. Nothing was changed.";
        if (name == card->name) return {};
        if (lower(name) != lower(card->name) && cardlib::exists(dir, name)) return "There is a card called \"" + name + "\" already.";
        bool done = false;
        if (card->slot) {
            done = slots.rename_in_slot(card->slot - 1, cardlib::file_of(dir, name), &why);
            if (done) slots.remember();
        } else {
            done = cardlib::rename(dir, card->name, name, why);
        }
        if (!done) return "Not renamed: " + why;
        std::fprintf(stderr, "[cards] \"%s\" renamed to \"%s\"\n", card->name.c_str(), name.c_str());
        return "The card is now called \"" + name + "\".";
    }
    case Command::card_copy: {
        if (!card) return {};
        if (card->slot && slots.flush) slots.flush();  // (the file must hold what the game has)
        const std::string name = cardlib::unique_name(card->name, names);
        if (!cardlib::copy(dir, card->name, name, why)) return "No copy: " + why;
        std::fprintf(stderr, "[cards] \"%s\" copied as \"%s\"\n", card->name.c_str(), name.c_str());
        return "Copied as \"" + name + "\".";
    }
    case Command::card_delete: {
        if (!card) return {};
        if (card->slot) return "Take the card out of its slot first.";
        if (!cardlib::remove(dir, card->name, why)) return "Not deleted: " + why;
        std::fprintf(stderr, "[cards] \"%s\" deleted\n", card->name.c_str());
        return "\"" + card->name + "\" is deleted.";
    }
    default: return {};
    }
}

Slots file_slots(std::string (&names)[2], std::function<void()> remember) {
    Slots s;
    s.in_slot = [&names](int slot) { return slot >= 0 && slot < 2 ? names[slot] : std::string(); };
    s.put = [&names](int slot, const std::string &file, std::string *why) {
        if (slot < 0 || slot > 1) return false;
        const std::string name = file.empty() ? std::string() : stem_of(file);
        if (!name.empty() && lower(names[1 - slot]) == lower(name)) {
            if (why) *why = "that card is in slot " + std::to_string(2 - slot);
            return false;
        }
        names[slot] = name;
        return true;
    };
    s.rename_in_slot = [&names](int slot, const std::string &to_file, std::string *why) {
        if (slot < 0 || slot > 1 || names[slot].empty()) return false;
        const std::string dir = ps2::os::utf8(ps2::os::path(to_file).parent_path()), to = stem_of(to_file);
        std::string reason;
        if (!cardlib::rename(dir, names[slot], to, reason)) {
            if (why) *why = reason;
            return false;
        }
        names[slot] = to;
        return true;
    };
    s.flush = [] {};
    s.remember = std::move(remember);
    return s;
}

}  // namespace cardpage
