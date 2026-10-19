#pragma once
// MenuTextTable -- adds the extension's strings (MenuStrings) to the title's
// text table, for every language bank that is LOADED (TR language.cpp: a
// language's line table exists only once GameController::LoadTexts read its
// Language\Code\<name>.txt; the US disc loads `American`).
//
// The title's table (MenuLayout::kLanguageTexts[file][language]) is a
// `const char*` array of `kTextLineCounts[file]` entries allocated with
// MemoryAllocate2 in ReadTextFile; GameText(i) = kTexts[file][i]. The extension
// allocates a LONGER array with the title's allocator, copies the title's
// entries, appends one guest-resident copy of each added string, and re-points
// the language's table (and kTexts[file] when that language is current) at it.
// The added strings therefore have the indices old_count .. old_count+N-1 in
// EVERY bank (the same index names the same string in each language), which
// is what the pages' MenuItemBits.text / MenuPageFlags.title carry.
#include <cstdint>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

#include "games/slus_20909_crash_twinsanity/hle/menu/menu_guest.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_strings.h"

namespace title {

class MenuTextTable {
public:
    struct Result {
        bool ok = false;
        uint32_t base = 0;            // the text index of MenuStrings::Id 0 in every extended bank
        uint32_t banks = 0;           // language banks extended (loaded ones)
        uint32_t languages = 0;       // the title's language count
        uint32_t original_count = 0;  // the line count before the extension (the current language's)
        std::string refusal;          // why nothing was done ("" when ok)
        std::vector<std::string> names;   // the names of the banks extended
    };

    // Appends every MenuStrings string to each loaded bank; `ctx` is the EE
    // context the title's allocator runs on. Refuses (no change) when no bank
    // is loaded, a word is outside RAM, or the title's allocator returns 0.
    static Result Extend(const MenuGuest& guest, CpuContext& ctx);

    // The text index of `id` given the base the extension received.
    static uint32_t Index(uint32_t base, MenuStrings::Id id);
};

}  // namespace title
