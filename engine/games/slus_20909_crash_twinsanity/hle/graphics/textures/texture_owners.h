#pragma once

#include <array>
#include <cstdint>

namespace title {

// Retail startup asset identities. Selection still requires the matching
// complete consumer service, owned generations and checkpoint preflight.
// Generated/checked by tests/title/textures/audit_texture_owners.py.
class TextureOwners {
public:
    enum class Family { CrashFont, ArialFont, Hud };
    struct Entry { uint32_t texture, material; Family family; };
    static constexpr const char* kArchiveHeaderSha256 = "27fe74b6f1253954dc9c2bede794a8528df0b035852ce4e8213d8d08ad42f6a7";
    static constexpr std::array<Entry, 44> kStartup{{
        {0x046a8347, 0x65195469, Family::Hud},
        {0x08a2db3f, 0x6a15933e, Family::Hud},
        {0x0e3a647b, 0xc9d1cf08, Family::Hud},
        {0x0ff9c7ef, 0x691cbc72, Family::Hud},
        {0x130bd9c4, 0x7f9c1fce, Family::Hud},
        {0x19235bb6, 0x958b90e9, Family::ArialFont},
        {0x1fc1648f, 0x721c6350, Family::Hud},
        {0x2b50a937, 0x761fcb59, Family::Hud},
        {0x321abbf3, 0x1b6dd3d8, Family::Hud},
        {0x36abd2e5, 0xb8c52d40, Family::Hud},
        {0x422ecdf5, 0xfb6e0238, Family::Hud},
        {0x4736c1d9, 0x16f9b561, Family::Hud},
        {0x5931ecf5, 0x31b9d150, Family::Hud},
        {0x5afc100c, 0xa1c6be13, Family::Hud},
        {0x6141645a, 0x60f94c0e, Family::Hud},
        {0x6f270dea, 0xd790fd72, Family::Hud},
        {0x78da5eff, 0xecaaff6e, Family::Hud},
        {0x79e21f4c, 0xa1deb3bd, Family::Hud},
        {0x7cfa1c72, 0x022136ac, Family::Hud},
        {0x7de66039, 0x745c5ee6, Family::Hud},
        {0x80c34f55, 0xc67f9aa4, Family::Hud},
        {0x87fcc096, 0xfc9ad83a, Family::Hud},
        {0x8f881cd1, 0xffe7d5fe, Family::Hud},
        {0x922ed81e, 0xb5cd358d, Family::CrashFont},
        {0x96432c5c, 0xc1b8c44b, Family::CrashFont},
        {0x9a57809a, 0xcda45309, Family::CrashFont},
        {0xa98a5c40, 0x455d66c8, Family::Hud},
        {0xb025198b, 0x119c8e56, Family::Hud},
        {0xb25af790, 0x11211ead, Family::Hud},
        {0xb4396dc9, 0x12e7871c, Family::Hud},
        {0xb84dc207, 0x14327fe2, Family::Hud},
        {0xbc621645, 0x157d78a8, Family::Hud},
        {0xc9d7c102, 0xb16550f2, Family::Hud},
        {0xcbdbe0ff, 0xb861c8e8, Family::Hud},
        {0xced683f7, 0x56048b11, Family::Hud},
        {0xda65c89f, 0x5a07f31a, Family::Hud},
        {0xe5f50d47, 0x5e0b5b23, Family::Hud},
        {0xed4bf9f7, 0x5d128457, Family::Hud},
        {0xf18451ef, 0x620ec32c, Family::Hud},
        {0xf696e86a, 0xa67b79f8, Family::Hud},
        {0xf8db3e9f, 0x6115ec60, Family::Hud},
        {0xfb0d8ab9, 0x5242f25c, Family::Hud},
        {0xfd139697, 0x66122b35, Family::Hud},
        {0xffcacf8e, 0xa7ec24ee, Family::Hud},
    }};
    static const Entry* Find(uint32_t texture);
};

}  // namespace title
