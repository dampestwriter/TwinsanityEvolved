#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.h"

class TextureBindingFixture {
public:
    static constexpr uint32_t object = 0x100000, stream = 0x110000;
    static constexpr uint32_t data = 0x120003, shader = 0x130000;
    // The title's main-thread stack is the scratchpad (TR entry.cpp:83); a
    // Stream passed to TextureRead lives there (sp 0x70003d30 observed).
    static constexpr uint32_t stack_stream = 0x70003d30;
    std::vector<uint8_t> ram, scratchpad, record;
    bool selected = true;
    TextureTestBackend backend;
    title::GraphicsResources resources;
    title::Graphics queue;
    title::TextureAssets assets;
    title::TextureBindings bindings;

    explicit TextureBindingFixture(bool indexed = true);
    void Shader(uint32_t type, int k = -69, int l = 0, bool linear = true, bool stq = true, bool textured = true);
    void Put(size_t at, uint64_t word, unsigned size = 4);
    uint32_t Word(size_t at) const;
};
