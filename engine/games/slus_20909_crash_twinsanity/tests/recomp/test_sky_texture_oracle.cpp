// Load-time conversion of authored sky textures vs NEW's unchanged transfer
// and texture decoder. No captured frames or external emulator oracle.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/hle/texsrc/texture_decoder.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_texture.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

namespace {
size_t cases = 0, pixels = 0;
[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL texture %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define REQUIRE(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void Compare(const std::vector<uint8_t>& file) {
    using Texture = title::SkyTexture;
    REQUIRE(file.size() >= 132);
    const auto* header = file.data() + 4;
    const auto* data = file.data() + 132;
    const size_t size = file.size() - 132;
    Texture::Image selected;
    const auto status = Texture::Decode(header, 96, data, size, selected);
    REQUIRE(status == Texture::Result::Ok);
    auto model = std::make_unique<ps2r::gs::GsState>();
    model->WriteRegister(0x50, uint64_t(1) << 48); // upload storage image at its local origin
    GsStateSink sink(*model);
    GifRegs regs;
    GifParser parser(sink, regs);
    parser.ReceiveData(GifPath::Path2, data + 16, size - 16);
    REQUIRE(parser.CanAcceptDirect());
    ps2r::gs::TexReadView view;
    view.psm = header[9]; view.tbp = Word(header + 0x10) * 256;
    view.tbw = Word(header + 0x2c) * 64;
    view.cbp = Word(header + 0x48) * 256;
    view.tw = header[4]; view.th = header[6];
    view.tcc = header[11]; view.tfx = header[13];
    std::vector<uint32_t> expected;
    ps2r::gs::TextureDecoder::Decode(model->Vram().Bytes(), view, nullptr, nullptr, &expected);
    REQUIRE(selected.rgba.size() == expected.size() * 4);
    REQUIRE(std::memcmp(selected.rgba.data(), expected.data(), selected.rgba.size()) == 0);
    pixels += expected.size();
    // Textures at load (hle.graphics_textures): the record is the texture owner's
    // converted TextureCodec asset and the object carries the ownership word, not
    // a DiskManager handle. The sky's image taken from that asset is Decode's.
    title::TextureCodec::Image converted;
    REQUIRE(title::TextureCodec::Decode(file.data(), file.size(), converted) == title::TextureCodec::Result::Ok);
    Texture::Image from_converted;
    REQUIRE(Texture::FromConverted(header, 96, converted, from_converted) == Texture::Result::Ok);
    REQUIRE(from_converted.width == selected.width && from_converted.height == selected.height &&
            from_converted.buffer_width == selected.buffer_width && from_converted.levels == selected.levels &&
            from_converted.function == selected.function && from_converted.alpha == selected.alpha);
    REQUIRE(from_converted.rgba == selected.rgba);
    {
        // A converted asset that is not this family (RGBA8, or a level-0 size the
        // header does not declare) declines; the output is untouched.
        auto other = converted;
        other.format = title::TextureCodec::Format::Rgba8;
        Texture::Image rejected;
        rejected.width = 0x1234;
        REQUIRE(Texture::FromConverted(header, 96, other, rejected) != Texture::Result::Ok);
        other = converted;
        other.levels[0].width *= 2;
        REQUIRE(Texture::FromConverted(header, 96, other, rejected) != Texture::Result::Ok);
        REQUIRE(rejected.width == 0x1234 && rejected.rgba.empty());
    }
    // Every truncation near a command boundary is an atomic rejection.
    for (size_t end : {size_t(0), size_t(16), size_t(80), size_t(95), size - 1}) {
        Texture::Image rejected;
        rejected.width = 0x1234;
        REQUIRE(Texture::Decode(header, 96, data, end, rejected) != Texture::Result::Ok);
        REQUIRE(rejected.width == 0x1234 && rejected.rgba.empty());
    }
    auto corrupted = file;
    corrupted[132 + 40] ^= 1; // unknown rectangle record must never be interpreted
    Texture::Image rejected;
    REQUIRE(Texture::Decode(header, 96, corrupted.data() + 132, size, rejected) != Texture::Result::Ok);
    ++cases;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::is_directory(argv[1])) return 77;
    std::vector<std::filesystem::path> files;
    for (const auto& file : std::filesystem::directory_iterator(argv[1]))
        if (file.path().filename().string().find("texture_") == 0) files.push_back(file.path());
    std::sort(files.begin(), files.end());
    REQUIRE(!files.empty());
    for (const auto& file : files) {
        std::ifstream stream(file, std::ios::binary);
        Compare({std::istreambuf_iterator<char>(stream), {}});
    }
    std::printf("Sky texture NEW self-oracle: %zu authored variants, %zu pixels; zero differences\n", cases, pixels);
}
