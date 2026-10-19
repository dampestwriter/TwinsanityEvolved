#pragma once
// MenuLayout -- the model of the title's menu page/item structures and text
// table that the owned menu extension (MenuExtension) builds its pages with.
// Every constant is DERIVED from the decompilation (twinsanity-reversed, PAL)
// and VERIFIED against the NTSC generated code (SLUS-20909); the PAL->NTSC
// function mapping is games/<t>/symbols/pal_ntsc_map.tsv (every row below is a
// `high` confidence row), the data words are gp-relative in the generated
// bodies (gp = 0x3114F0, crt0 FUN_00100000 Label_0002).
//
// Spec: TR include/game/menus.h (MenuItem, LinkItem, BackItem, ValueItem,
// ChoiceItem, ToggleItem, MenuPage, MenuDrawer), TR src/game/menus.cpp
// (Step, Enter, Add, the constructors), TR include/game/language.h +
// src/game/language.cpp (the text table: one `const char*` line table per
// language per text file; a text index is a NON-BLANK line's ordinal),
// TR src/game/olegpages.cpp OptionsPage::Construct (the page the extension
// registers with -- the title's own three option subpages are constructed
// and Add()ed there) and TR src/game/oleg.cpp OLEG::StartUp (the page table
// pages[18]; PageOptions = OptionsPage::Construct(Allocate<OptionsPage>())).
//
// Nothing here is copied from the decompilation: the numbers are the title's
// binary layout, read back from the NTSC generated functions named per row.
#include <cstdint>

namespace title {

class MenuLayout {
public:
    // --- functions (NTSC starts; the generated FUN_ names) -------------------
    // OptionsPage::Construct(page, parent) -- PAL FUN_00164418 -> NTSC FUN_001646c0.
    // The extension's registration point: the title constructs the Options page
    // and its Graphics/Sound/Game subpages here and Add()s their links.
    static constexpr uint32_t kOptionsPageConstruct = 0x001646c0u;
    // MenuPage::ConstructTitled(page, title_text, players) -- PAL FUN_0025bde0 -> 0x25b588.
    static constexpr uint32_t kMenuPageConstructTitled = 0x0025b588u;
    // MenuPage::Add(page, item) -- PAL FUN_0025bf68 -> 0x25b710 (PointerArray::Append).
    static constexpr uint32_t kMenuPageAdd = 0x0025b710u;
    // LinkItem::Construct(item, text, id, target, players) -- PAL FUN_0025c948 -> 0x25c0f0.
    static constexpr uint32_t kLinkItemConstruct = 0x0025c0f0u;
    // BackItem::Construct(item, text, id, target, players) -- PAL FUN_0025ba50 -> 0x25b1f8.
    static constexpr uint32_t kBackItemConstruct = 0x0025b1f8u;
    // ToggleItem::Construct(item, text, id, offText, onText, wraps, value, players)
    // -- PAL FUN_0025ca68 -> 0x25c210 (a0-a3, t0-t3).
    static constexpr uint32_t kToggleItemConstruct = 0x0025c210u;
    // ChoiceItem::Construct(item, text, id, choices, wraps, value, players) -- PAL FUN_0025c6b0 -> 0x25be58.
    static constexpr uint32_t kChoiceItemConstruct = 0x0025be58u;
    // ChoiceItem::SetChoiceText(item, choice, text) -- PAL FUN_0025c838 -> 0x25bfe0.
    static constexpr uint32_t kChoiceItemSetText = 0x0025bfe0u;
    // ValueItem::Construct(item, text, id, minimum, maximum, wraps, value, players)
    // -- PAL FUN_0025bb38 -> 0x25b2e0 (a0-a3, t0-t3).
    static constexpr uint32_t kValueItemConstruct = 0x0025b2e0u;
    // The title's allocators: MemoryAllocate(size) FUN_00181560 (the page/item
    // allocations of OptionsPage::Construct) and MemoryAllocate2(size)
    // FUN_001815c0 (the text line tables, ReadTextFile).
    static constexpr uint32_t kMemoryAllocate = 0x00181560u;
    static constexpr uint32_t kMemoryAllocate2 = 0x001815c0u;

    // --- data words (gp = 0x3114F0) ------------------------------------------
    static constexpr uint32_t kGp = 0x003114f0u;
    static constexpr uint32_t kLanguageCount = kGp - 0x7da0;      // 0x309750 InitLanguages (FUN_001812f0)
    static constexpr uint32_t kCurrentLanguage = kGp - 0x7d9c;    // 0x309754 UseLanguageTexts (FUN_00181398)
    static constexpr uint32_t kLanguageNames = kGp - 0x7d98;      // 0x309758 const char** (the folder/file names)
    static constexpr uint32_t kTextLineCounts = kGp - 0x7d90;     // 0x309760 u32[2] per text file (ReadTextFile FUN_0017d978)
    static constexpr uint32_t kLanguageTexts = kGp - 0x7d88;      // 0x309768 const char***[2]: [file][language] -> lines
    static constexpr uint32_t kTexts = kGp - 0x7d80;              // 0x309770 const char**[2]: the current language's lines
    static constexpr uint32_t kCodeTextsFile = 0;                 // Language\Code\<language>.txt (GameText reads it)
    // The Options page's vtable (NTSC D_002F39E0; PAL D_002F3D00 g_OptionsPageVTable):
    // the word the registration verifies at page+kPageVTable after the title's
    // constructor returns.
    static constexpr uint32_t kOptionsPageVTable = 0x002f39e0u;

    // --- MenuPage (0x50 bytes; TR CHECK_SIZE(MenuPage, 0x50)) -----------------
    static constexpr uint32_t kPageSize = 0x50;
    static constexpr uint32_t kPageName = 0x00;
    static constexpr uint32_t kPageSelections = 0x0c;             // u8 per player
    static constexpr uint32_t kPageFlags = 0x14;                  // MenuPageFlags: title:10 .. firstItem:6 at bit 18
    static constexpr uint32_t kPageParent = 0x18;
    static constexpr uint32_t kPageItemsData = 0x3c;              // PointerArray<MenuItem>: data, count, capacity, growth
    static constexpr uint32_t kPageItemsCount = 0x40;
    static constexpr uint32_t kPageItemsCapacity = 0x44;
    static constexpr uint32_t kPageVTable = 0x4c;

    // --- MenuItem family -----------------------------------------------------
    static constexpr uint32_t kItemText = 0x00;                   // const char* (0 = the text table's)
    static constexpr uint32_t kItemBits = 0x04;                   // MenuItemBits: id:12 text:12 players:4 stepsOnPress:1
    static constexpr uint32_t kItemShown = 0x08;                  // u8 bit per player
    static constexpr uint32_t kItemEnabled = 0x09;
    static constexpr uint32_t kItemTarget = 0x0c;
    static constexpr uint32_t kItemVTable = 0x10;
    static constexpr uint32_t kLinkItemSize = 0x14;               // LinkItem / BackItem
    static constexpr uint32_t kValueItemValues = 0x14;            // s32 values[8] (player 0 first)
    static constexpr uint32_t kValueItemMinimum = 0x34;
    static constexpr uint32_t kValueItemMaximum = 0x38;
    static constexpr uint32_t kValueItemWraps = 0x3c;
    static constexpr uint32_t kValueItemCycles = 0x3d;
    static constexpr uint32_t kValueItemSize = 0x40;
    static constexpr uint32_t kChoiceItemStrings = 0x40;          // const char** (own names)
    static constexpr uint32_t kChoiceItemUsesStrings = 0x44;
    static constexpr uint32_t kChoiceItemTexts = 0x48;            // s32* text indices per choice
    static constexpr uint32_t kChoiceItemChoices = 0x4c;
    static constexpr uint32_t kChoiceItemSize = 0x50;             // ChoiceItem / ToggleItem

    // --- the title's constants the extension reuses -------------------------
    static constexpr uint32_t kMenuPlayers = 1;
    static constexpr uint32_t kBackText = 4;                      // "back" (American.txt line 4)
    static constexpr uint32_t kOffText = 0;                       // "off"
    static constexpr uint32_t kOnText = 1;                        // "on"
    static constexpr uint32_t kSubPageBackItem = 0x1e;            // the id the title's option subpages give their back item
    // The extension's item ids: above every id the title uses (MenuItemBits.id is 12 bits).
    static constexpr uint32_t kFirstExtensionItemId = 0x800;
    static constexpr uint32_t kMaxTitleText = 0x3ff;              // MenuPageFlags.title is 10 bits
    static constexpr uint32_t kMaxItemText = 0xfff;               // MenuItemBits.text is 12 bits

    // A guest address inside main RAM above the kernel area.
    static bool IsRam(uint32_t address);
    // MenuItemBits for an item (id, text, players).
    static uint32_t ItemBits(uint32_t id, uint32_t text, uint32_t players);
    static uint32_t ItemId(uint32_t bits);
    static uint32_t ItemTextIndex(uint32_t bits);
};

}  // namespace title
