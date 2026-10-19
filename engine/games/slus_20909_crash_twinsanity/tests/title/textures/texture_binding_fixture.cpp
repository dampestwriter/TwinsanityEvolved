#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "tests/lib/private_fixtures.h"

namespace {
// The title's startup table words (data/ram_data_map_v50.txt: the ELF .data section, game bytes)
// are a PRIVATE fixture (cmake/Ps2rPrivateFixtures.cmake). Every test built on this fixture
// SKIPs (exit 77) when the map is absent -- never fails.
constexpr const char* kDataMapRel = "games/slus_20909_crash_twinsanity/data/ram_data_map_v50.txt";
}

TextureBindingFixture::TextureBindingFixture(bool indexed)
    : ram(4 * 1024 * 1024), scratchpad(16 * 1024), record(TextureTestData::Record(indexed)),
      resources(backend), assets(queue, resources, backend),
      bindings(assets, {ram.data(), ram.size(), scratchpad.data(), scratchpad.size()},
               [this](uint32_t, uint32_t) { return selected; }) {
    // Current NEW's actual startup table words, not a fabricated dispatch map.
    const std::string map = PrivateFixtures::Path(kDataMapRel);
    if (!PrivateFixtures::Exists(map)) std::exit(PrivateFixtures::Skip(map));
    std::ifstream file(map);
    if (!file) throw std::runtime_error("unreadable NTSC data map: " + map);
    std::string line;
    while (std::getline(file, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto address = std::stoul(line.substr(0, colon), nullptr, 16);
        if (address + 4 <= ram.size()) Put(address, std::stoul(line.substr(colon + 1), nullptr, 16));
    }
    Put(object, 0x80000001); Put(object + 4, 19); Put(object + 0x60, 0xffffffff);
    Put(stream, title::TextureBindings::kMemoryStream);
    Put(stream + 4, 0x10010); // alignment/ownsMemory survive
    Put(stream + 8, data); Put(stream + 12, data); Put(stream + 16, record.size() + 16);
    Put(title::TextureBindings::kMemoryStream + 0x14, 0x2b5dd8);
    Put(title::TextureBindings::kMemoryStream + 0xac, 0x2b5850);
    std::copy(record.begin(), record.end(), ram.begin() + data);
}
void TextureBindingFixture::Shader(uint32_t type, int k, int l, bool linear, bool stq, bool textured) {
    constexpr uint32_t tables[] = {
        0x2fada8, 0x2faec8, 0x2fae38, 0x2fad28, 0x2fac08, 0x2fac98, 0x2fab78, 0x2faae8,
        0x2faa58, 0x2fa9c8, 0x2fa948, 0x2fa8c8, 0x2fa848, 0x2f9ed8, 0x2f9db8, 0x2fa7c8,
        0x2fa738, 0x2fa6a8, 0x2fa618, 0x2fa588, 0x2f9e48, 0x2fa4f8, 0x2fa468, 0x2fa3d8,
        0x2fa348, 0x2fa2b8, 0x2fa228, 0x2fa198, 0x2fa108, 0, 0x2fa078, 0x2f9fe8, 0x2f9f58,
    };
    Put(shader, uint64_t(textured) << 27 | uint64_t(stq) << 28 | uint64_t(linear) << 55, 8);
    Put(shader + 0x14, object); Put(shader + 0x18, uint16_t(k), 2); Put(shader + 0x1a, uint16_t(l), 2);
    Put(shader + 0x60, 19); Put(shader + 0x64, type);
    Put(shader + 0x6c, type < 33 ? tables[type] : 0);
    // Poison generated settings pointers. The adapter must never read them.
    Put(shader + 0x58, 0xffffffff); Put(shader + 0x5c, 0xffffffff);
}
void TextureBindingFixture::Put(size_t at, uint64_t word, unsigned size) {
    TextureTestData::Put(ram, at, word, size);
}
uint32_t TextureBindingFixture::Word(size_t at) const { return TextureTestData::Word(ram.data() + at); }
