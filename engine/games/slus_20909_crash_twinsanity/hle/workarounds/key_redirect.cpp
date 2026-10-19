#include "games/slus_20909_crash_twinsanity/hle/workarounds/key_redirect.h"

#include <cstring>

#include "engine/ee/mem/main_memory.h"

namespace {
constexpr uint32_t kPhysMask = 0x1FFFFFFFu;
constexpr uint32_t kLowestData = 0x10000u;
constexpr char kBeachKey[] = "Levels\\Earth\\Hub\\Beach";
constexpr char kLevelsPrefix[] = "levels\\";
// The beach-string probe's end-of-RAM guard covers 24 bytes (the 23-byte key
// rounded to the original's bound), the levels-prefix probe sizeof("levels\\") = 8.
constexpr uint32_t kBeachTailBound = 24;

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
}  // namespace

KeyRedirect::KeyRedirect(MainMemory& ram) : ram_(ram) {}

bool KeyRedirect::ReadWord(uint32_t addr, uint32_t* out) const {
    if (addr + 4 > ram_.Size()) return false;
    std::memcpy(out, ram_.Data() + addr, 4);
    return true;
}

bool KeyRedirect::IsBeachAt(uint32_t addr) const {
    addr &= kPhysMask;
    if (addr < kLowestData || addr + kBeachTailBound >= ram_.Size()) return false;
    for (size_t i = 0; kBeachKey[i]; i++)
        if (Lower(static_cast<char>(ram_.Data()[addr + i])) != Lower(kBeachKey[i])) return false;
    return true;
}

bool KeyRedirect::IsLevelsPathAt(uint32_t addr) const {
    addr &= kPhysMask;
    if (addr < kLowestData || addr + sizeof(kLevelsPrefix) >= ram_.Size()) return false;
    for (size_t i = 0; kLevelsPrefix[i]; i++)
        if (Lower(static_cast<char>(ram_.Data()[addr + i])) != kLevelsPrefix[i]) return false;
    return true;
}

void KeyRedirect::NoteWrite() {
    if ((stats_.writes++ % kCadence) == 0) Scan();
}

bool KeyRedirect::Scan() {
    ++stats_.scans;
    uint32_t global_key = 0, object = 0;
    if (!ReadWord(kGlobalBeachPtr, &global_key) || !ReadWord(kLevelObjectPtr, &object)) return false;
    const uint32_t global_phys = global_key & kPhysMask;
    const uint32_t object_phys = object & kPhysMask;
    if (object_phys < kLowestData || object_phys + kKeyOffset + 4 > ram_.Size()) return false;
    if (global_phys == 0 || !IsBeachAt(global_phys)) return false;
    uint32_t key = 0;
    ReadWord(object_phys + kKeyOffset, &key);
    const uint32_t key_phys = key & kPhysMask;
    if (IsBeachAt(key_phys)) stats_.seen_beach = true;
    if (!stats_.seen_beach || key_phys == global_phys || IsLevelsPathAt(key_phys)) return false;
    std::memcpy(ram_.Data() + object_phys + kKeyOffset, &global_key, 4);  // rewrite the POINTER only
    ++stats_.repairs;
    return true;
}
