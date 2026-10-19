#include "games/slus_20909_crash_twinsanity/hle/menu/menu_layout.h"

namespace title {

bool MenuLayout::IsRam(uint32_t address) {
    return address >= 0x00010000u && address < 0x02000000u && (address & 3u) == 0;
}

uint32_t MenuLayout::ItemBits(uint32_t id, uint32_t text, uint32_t players) {
    return (id & 0xfffu) | ((text & 0xfffu) << 12) | ((players & 0xfu) << 24);
}

uint32_t MenuLayout::ItemId(uint32_t bits) { return bits & 0xfffu; }

uint32_t MenuLayout::ItemTextIndex(uint32_t bits) { return (bits >> 12) & 0xfffu; }

}  // namespace title
