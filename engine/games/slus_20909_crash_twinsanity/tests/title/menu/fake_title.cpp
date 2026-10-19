#include "games/slus_20909_crash_twinsanity/tests/title/menu/fake_title.h"

namespace menu_test {

uint8_t* FakeTitle::At(uint32_t a) {
    if (a < kRamBase || a + 4 > kRamBase + kRamSize) {
        std::fprintf(stderr, "guest access outside fake RAM: %x\n", a);
        std::abort();
    }
    return ram.data() + (a - kRamBase);
}

uint32_t FakeTitle::R(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, At(a), 4);
    return v;
}

uint32_t FakeTitle::Alloc(uint32_t size) {
    if (allocator_fails) return 0;
    const uint32_t p = bump;
    bump += (size + 15) & ~15u;
    return p;
}

uint32_t FakeTitle::String(const char* s) {
    const uint32_t p = Alloc(static_cast<uint32_t>(std::strlen(s)) + 1);
    std::memcpy(At(p), s, std::strlen(s) + 1);
    return p;
}

uint32_t FakeTitle::Call(uint32_t fn, const uint32_t* a, size_t n) {
    ++calls[fn];
    (void)n;
    switch (fn) {
        case L::kMemoryAllocate: case L::kMemoryAllocate2: return Alloc(a[0]);
        case L::kMenuPageConstructTitled: {
            const uint32_t page = a[0];
            W(page + L::kPageVTable, kBasePageVTable);
            W(page + L::kPageName, 0);
            W(page + L::kPageSelections, Alloc(a[2]));
            W(page + L::kPageFlags, (a[1] & 0x3ff) | ((a[2] & 0x1f) << 13));
            W(page + L::kPageParent, 0);
            W(page + L::kPageItemsData, Alloc(4 * 4));
            W(page + L::kPageItemsCount, 0);
            W(page + L::kPageItemsCapacity, 4);
            W(page + L::kPageItemsCapacity + 4, 4);
            return page;
        }
        case L::kMenuPageAdd: {
            const uint32_t page = a[0];
            uint32_t count = R(page + L::kPageItemsCount), cap = R(page + L::kPageItemsCapacity);
            uint32_t data = R(page + L::kPageItemsData);
            if (count == cap) {
                const uint32_t larger = Alloc((cap + 4) * 4);
                for (uint32_t i = 0; i < count; ++i) W(larger + i * 4, R(data + i * 4));
                data = larger;
                W(page + L::kPageItemsData, data);
                W(page + L::kPageItemsCapacity, cap + 4);
            }
            W(data + count * 4, a[1]);
            W(page + L::kPageItemsCount, count + 1);
            return 1;
        }
        case L::kLinkItemConstruct: case L::kBackItemConstruct:
            Item(a[0], a[1], a[2], a[4], fn == L::kLinkItemConstruct ? kLinkVTable : kBackVTable);
            W(a[0] + L::kItemTarget, a[3]);
            return a[0];
        case L::kToggleItemConstruct:
            Item(a[0], a[1], a[2], a[7], kToggleVTable);
            W8(a[0] + L::kValueItemCycles, 1);
            Range(a[0], a[7], 0, 1, a[5], static_cast<int32_t>(a[6]));
            Choices(a[0], 2);
            W(R(a[0] + L::kChoiceItemTexts) + 0, a[3]);
            W(R(a[0] + L::kChoiceItemTexts) + 4, a[4]);
            return a[0];
        case L::kChoiceItemConstruct:
            Item(a[0], a[1], a[2], a[6], kChoiceVTable);
            W8(a[0] + L::kValueItemCycles, 0);
            Range(a[0], a[6], 0, static_cast<int32_t>(a[3]) - 1, a[4], static_cast<int32_t>(a[5]));
            Choices(a[0], a[3]);
            return a[0];
        case L::kChoiceItemSetText:
            W(R(a[0] + L::kChoiceItemTexts) + a[1] * 4, a[2]);
            return 0;
        case L::kValueItemConstruct:
            Item(a[0], a[1], a[2], a[7], kValueVTable);
            W8(a[0] + L::kValueItemCycles, 0);
            Range(a[0], a[7], static_cast<int32_t>(a[3]), static_cast<int32_t>(a[4]), a[5], static_cast<int32_t>(a[6]));
            return a[0];
    }
    std::fprintf(stderr, "unexpected title call %x\n", fn);
    std::abort();
}

void FakeTitle::W(uint32_t a, uint32_t v) {
    std::memcpy(At(a), &v, 4);
}

void FakeTitle::W8(uint32_t a, uint8_t v) {
    *At(a) = v;
}

void FakeTitle::Item(uint32_t item, uint32_t text, uint32_t id, uint32_t players, uint32_t vtable) {
    W(item + L::kItemText, 0);
    W(item + L::kItemBits, L::ItemBits(id, text, players));
    W8(item + L::kItemShown, 0xff);
    W8(item + L::kItemEnabled, 0xff);
    W(item + L::kItemTarget, 0);
    W(item + L::kItemVTable, vtable);
}

void FakeTitle::Range(uint32_t item, uint32_t players, int32_t mn, int32_t mx, uint32_t wraps, int32_t value) {
    W(item + L::kValueItemMinimum, static_cast<uint32_t>(mn));
    W(item + L::kValueItemMaximum, static_cast<uint32_t>(mx));
    W8(item + L::kValueItemWraps, static_cast<uint8_t>(wraps));
    for (uint32_t p = 0; p < players; ++p) W(item + L::kValueItemValues + p * 4, static_cast<uint32_t>(value));
}

void FakeTitle::Choices(uint32_t item, uint32_t choices) {
    W(item + L::kChoiceItemUsesStrings, 0);
    W(item + L::kChoiceItemStrings, 0);
    W(item + L::kChoiceItemChoices, choices);
    const uint32_t texts = Alloc(choices * 4);
    for (uint32_t c = 0; c < choices; ++c) W(texts + c * 4, 2);   // NoText
    W(item + L::kChoiceItemTexts, texts);
}

title::MenuGuest FakeTitle::Guest() {
    title::MenuGuest g;
    g.read32 = [this](uint32_t a) { return R(a); };
    g.write32 = [this](uint32_t a, uint32_t v) { W(a, v); };
    g.write_bytes = [this](uint32_t a, const uint8_t* b, size_t n) { std::memcpy(At(a), b, n); };
    g.read_string = [this](uint32_t a, char* out, size_t max) {
        size_t n = 0;
        while (n < max && *At(a + static_cast<uint32_t>(n))) { out[n] = static_cast<char>(*At(a + static_cast<uint32_t>(n))); ++n; }
        out[n] = 0;
        return static_cast<uint32_t>(n);
    };
    g.call = [this](CpuContext&, uint32_t fn, const uint32_t* a, size_t n) { return Call(fn, a, n); };
    return g;
}

std::string FakeTitle::Str(uint32_t a) {
    return std::string(reinterpret_cast<const char*>(At(a)));
}

void FakeTitle::SeedTitle(bool load_texts) {
    W(L::kLanguageCount, 1);
    W(L::kCurrentLanguage, 0);
    const uint32_t names = Alloc(4);
    W(names, String("American"));
    W(L::kLanguageNames, names);
    const uint32_t tables = Alloc(4);
    W(L::kLanguageTexts, tables);
    W(L::kLanguageTexts + 4, Alloc(4));
    lines = 0;
    if (load_texts) {
        lines = Alloc(kOriginalLines * 4);
        for (uint32_t i = 0; i < kOriginalLines; ++i) W(lines + i * 4, String(("line" + std::to_string(i)).c_str()));
    }
    W(tables, lines);
    W(L::kTexts, lines);
    W(L::kTextLineCounts, kOriginalLines);
    options = Alloc(0x144);
    uint32_t a[3] = {options, 0xb, 1};
    Call(L::kMenuPageConstructTitled, a, 3);
    W(options + L::kPageVTable, L::kOptionsPageVTable);
    for (uint32_t text : {0xdu, 0xeu, 0xfu}) {
        const uint32_t link = Alloc(L::kLinkItemSize);
        uint32_t la[5] = {link, text, text + 0x10, 0, 1};
        Call(L::kLinkItemConstruct, la, 5);
        uint32_t ad[2] = {options, link};
        Call(L::kMenuPageAdd, ad, 2);
    }
    title_back = Alloc(L::kLinkItemSize);
    uint32_t ba[5] = {title_back, 4, 0x1d, 0, 1};
    Call(L::kBackItemConstruct, ba, 5);
    uint32_t ad[2] = {options, title_back};
    Call(L::kMenuPageAdd, ad, 2);
}


}  // namespace menu_test
