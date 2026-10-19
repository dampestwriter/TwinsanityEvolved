// tests/title/menu/fake_title.h -- FakeTitle: a fake RAM, a bump allocator and
// the title's menu constructors' layout effects (TR menus.h / menus.cpp as
// MenuLayout documents them), so MenuExtension is pinned without a game.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/menu/menu_guest.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_layout.h"

namespace menu_test {

using L = title::MenuLayout;

constexpr uint32_t kRamBase = 0x00100000u, kRamSize = 0x00300000u;
constexpr uint32_t kBasePageVTable = 0x00302f30u, kLinkVTable = 0x00302b90u, kBackVTable = 0x00302b18u,
                   kValueVTable = 0x00302aa0u, kChoiceVTable = 0x00302910u, kToggleVTable = 0x00302898u;
constexpr uint32_t kOriginalLines = 228;

// The fake title: RAM, a bump allocator and the constructors' layout effects.
struct FakeTitle {
    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize, 0);
    uint32_t bump = kRamBase + 0x200000;
    std::map<uint32_t, uint32_t> calls;   // fn -> count
    bool allocator_fails = false;

    uint8_t* At(uint32_t a);
    uint32_t R(uint32_t a);
    void W(uint32_t a, uint32_t v);
    void W8(uint32_t a, uint8_t v);
    uint32_t Alloc(uint32_t size);
    uint32_t String(const char* s);
    // MenuItem::Construct + the subclasses (TR menus.cpp 822, 905, 1037, 1056, 1208, 1253).
    void Item(uint32_t item, uint32_t text, uint32_t id, uint32_t players, uint32_t vtable);
    void Range(uint32_t item, uint32_t players, int32_t mn, int32_t mx, uint32_t wraps, int32_t value);
    void Choices(uint32_t item, uint32_t choices);
    uint32_t Call(uint32_t fn, const uint32_t* a, size_t n);
    title::MenuGuest Guest();
    std::string Str(uint32_t a);

    // The title's language globals as GameController::LoadTexts leaves them for
    // a one-bank disc (American), and an Options page as OptionsPage::Construct
    // leaves it: three links and a back item.
    uint32_t lines = 0, options = 0, title_back = 0;
    void SeedTitle(bool load_texts = true);
};

}  // namespace menu_test
