#include "games/slus_20909_crash_twinsanity/hle/menu/menu_text_table.h"

#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/menu/menu_layout.h"

namespace title {

uint32_t MenuTextTable::Index(uint32_t base, MenuStrings::Id id) {
    return base + static_cast<uint32_t>(id);
}

MenuTextTable::Result MenuTextTable::Extend(const MenuGuest& guest, CpuContext& ctx) {
    Result r;
    const uint32_t file = MenuLayout::kCodeTextsFile;
    const uint32_t languages = guest.read32(MenuLayout::kLanguageCount);
    const uint32_t names = guest.read32(MenuLayout::kLanguageNames);
    const uint32_t tables = guest.read32(MenuLayout::kLanguageTexts + file * 4);   // const char**[languages]
    const uint32_t current = guest.read32(MenuLayout::kTexts + file * 4);
    const uint32_t count = guest.read32(MenuLayout::kTextLineCounts + file * 4);
    r.languages = languages;
    r.original_count = count;
    if (languages == 0 || languages > 16 || !MenuLayout::IsRam(tables)) {
        r.refusal = "languages not initialised";
        return r;
    }
    if (count == 0 || count > 4000 || count + MenuStrings::kCount > MenuLayout::kMaxTitleText) {
        r.refusal = "text line count out of range";
        return r;
    }
    // Every loaded bank gets the same appended indices: the base is the common
    // line count (the title reads one count for the file; the banks share it).
    const uint32_t base = count;
    const uint32_t new_count = count + MenuStrings::kCount;
    for (uint32_t language = 0; language < languages; ++language) {
        const uint32_t lines = guest.read32(tables + language * 4);
        if (lines == 0) continue;   // this bank is not loaded (TR LoadTexts reads the current language only)
        if (!MenuLayout::IsRam(lines)) {
            r.refusal = "a language's line table is outside RAM";
            return r;
        }
        char name[32] = {};
        const uint32_t name_ptr = MenuLayout::IsRam(names) ? guest.read32(names + language * 4) : 0;
        if (MenuLayout::IsRam(name_ptr)) guest.read_string(name_ptr, name, sizeof(name) - 1);
        const std::string language_name(name);
        // The longer table, allocated as the title allocates its own (MemoryAllocate2).
        uint32_t size = new_count * 4;
        const uint32_t table = guest.call(ctx, MenuLayout::kMemoryAllocate2, &size, 1);
        if (!MenuLayout::IsRam(table)) {
            r.refusal = "the title's allocator refused the text table";
            return r;
        }
        for (uint32_t i = 0; i < count; ++i) guest.write32(table + i * 4, guest.read32(lines + i * 4));
        for (uint32_t i = 0; i < MenuStrings::kCount; ++i) {
            const char* text = MenuStrings::Text(static_cast<MenuStrings::Id>(i), language_name);
            uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1;
            const uint32_t copy = guest.call(ctx, MenuLayout::kMemoryAllocate2, &length, 1);
            if (!MenuLayout::IsRam(copy)) {
                r.refusal = "the title's allocator refused a string";
                return r;
            }
            guest.write_bytes(copy, reinterpret_cast<const uint8_t*>(text), length);
            guest.write32(table + (count + i) * 4, copy);
        }
        guest.write32(tables + language * 4, table);
        if (current == lines) guest.write32(MenuLayout::kTexts + file * 4, table);
        ++r.banks;
        r.names.push_back(language_name);
    }
    if (r.banks == 0) {
        r.refusal = "no language bank is loaded";
        return r;
    }
    guest.write32(MenuLayout::kTextLineCounts + file * 4, new_count);
    r.base = base;
    r.ok = true;
    return r;
}

}  // namespace title
