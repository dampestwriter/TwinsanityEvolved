#pragma once
// MenuWidget -- the widget API of the owned menu extension :
// what an option owner (the Display page, texture packs,
// controls) declares to put ONE option on one of the added pages.
// The extension builds the widget with the title's OWN item constructors
// (ToggleItem / ChoiceItem / ValueItem -- MenuLayout) so the title's menu code
// navigates it, steps it and draws it with the title's font; the host side is
// two callbacks: `read` (the current value of the setting) and `apply` (the
// user changed it in the menu: apply live and persist). Every label is a
// MenuStrings id, so the page shows the string of the language bank in play.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/menu/menu_strings.h"

namespace title {

struct MenuWidget {
    enum class Kind : uint8_t {
        // off/on (a ToggleItem: select flips it, left/right step it). Value 0/1.
        Toggle,
        // one of N named choices (a ChoiceItem: left/right step, wraps). Value = choice index.
        Enum,
        // an integer in [minimum, maximum] (a ValueItem drawn as "%d"). Value = the number.
        Slider,
        // a key/button binding: an Enum whose choices are the bindable names
        // (`choices`); the owner maps the index to its binding. Drawn and stepped
        // like an Enum; the Kind documents the intent for the Controls page.
        KeyBinding,
    };
    Kind kind = Kind::Toggle;
    // The label (an added text of every language bank) and the settings key
    // the value persists under (documentation + the census row name).
    MenuStrings::Id label = MenuStrings::Id::Display;
    std::string key;
    // Enum/KeyBinding: the choice labels, in value order.
    std::vector<MenuStrings::Id> choices;
    // Slider: the range; Enum: derived from `choices`; Toggle: 0..1.
    int32_t minimum = 0;
    int32_t maximum = 1;
    // Whether stepping past an end wraps round (the title's `wraps`).
    bool wraps = true;
    // The host side: the value now, and the user's new value from the menu.
    std::function<int32_t()> read;
    std::function<void(int32_t)> apply;
};

// One added page: its title string and its widgets, in order; a back item to
// the Options page is appended by the extension.
struct MenuPageSpec {
    MenuStrings::Id title = MenuStrings::Id::Display;
    std::vector<MenuWidget> widgets;
};

}  // namespace title
