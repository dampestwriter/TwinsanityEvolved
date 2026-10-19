#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_intent.h"

#include <array>
#include <cstring>
#include <utility>

namespace title {
namespace {

uint32_t Word(const uint8_t* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
           uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}

float Number(const uint8_t* data) {
    const uint32_t bits = Word(data);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool ReadRam(const StripIntent::Read& read, uint32_t address, void* out, size_t size) {
    const uint32_t segment = address & 0xe0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) return false;
    const uint32_t physical = address & 0x1fffffffu;
    if (!physical || physical >= 0x02000000u || size > 0x02000000u - physical)
        return false;
    try { return read && read(physical, out, size); }
    catch (...) { return false; }
}

}  // namespace

StripIntent::Result StripIntent::Capture(
    const Read& read, uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2,
    const MaterialState::Environment& environment, Output& out) {
    const bool tinted = entry == kTintedAddress || entry == kPlacedTintedAddress;
    const bool placed = entry == kPlacedAddress || entry == kPlacedTintedAddress;
    if (entry != kPlainAddress && !tinted && !placed) return Result::UnsupportedEntry;

    std::array<uint8_t, 0x1c> shape{};
    if ((a0 & 3) || !ReadRam(read, a0, shape.data(), shape.size()))
        return Result::InvalidMemory;
    const size_t count = shape[0x10];
    const Read ram = [&read](uint32_t address, void* bytes, size_t size) {
        return ReadRam(read, address, bytes, size);
    };
    MaterialState::Frame frame;
    const auto frame_result = MaterialState::ReadFrame(ram, environment, frame);
    if (frame_result == MaterialState::Result::InvalidMemory) return Result::InvalidMemory;
    if (frame_result != MaterialState::Result::Ok) return Result::UnsupportedTarget;
    std::array<uint8_t, 4> target{};
    if (!ram(MaterialState::kRenderTarget, target.data(), target.size()))
        return Result::InvalidMemory;
    MaterialState::Output material;
    const auto state_result = MaterialState::Build(ram, Word(shape.data()),
                                                   Word(target.data()), frame, material);
    if (state_result == MaterialState::Result::InvalidMemory) return Result::InvalidMemory;
    if (state_result == MaterialState::Result::UnsupportedShader ||
        state_result == MaterialState::Result::UnsupportedMaterial)
        return Result::UnsupportedMaterial;
    if (state_result != MaterialState::Result::Ok) return Result::UnsupportedTarget;

    const uint32_t places = Word(shape.data() + 0x14);
    const uint32_t colors = Word(shape.data() + 0x18);
    if (count && ((places & 3) || (colors & 3))) return Result::InvalidMemory;
    std::array<uint8_t, StripDraw::kMaxVertices * 8> position_bytes{};
    std::array<uint8_t, StripDraw::kMaxVertices * 4> color_bytes{};
    if (count && (!ram(places, position_bytes.data(), count * 8) ||
                  !ram(colors, color_bytes.data(), count * 4))) return Result::InvalidMemory;
    std::array<StripDraw::Position, StripDraw::kMaxVertices> positions{};
    std::array<uint32_t, StripDraw::kMaxVertices> rgba{};
    for (size_t i = 0; i != count; ++i) {
        positions[i] = {Number(position_bytes.data() + i * 8),
                        Number(position_bytes.data() + i * 8 + 4)};
        rgba[i] = Word(color_bytes.data() + i * 4);
    }
    StripDraw::Matrix matrix{};
    if (placed && count) {
        std::array<uint8_t, 64> matrix_bytes{};
        if ((a1 & 15) || !ram(a1, matrix_bytes.data(), matrix_bytes.size()))
            return Result::InvalidMemory;
        for (size_t column = 0; column != 4; ++column)
            for (size_t lane = 0; lane != 4; ++lane)
                matrix[column][lane] = Number(matrix_bytes.data() + column * 16 + lane * 4);
    }
    const StripDraw::Effects effects{placed ? &matrix : nullptr, tinted, placed ? a2 : a1};
    Output candidate;
    candidate.bucket = material.bucket;
    candidate.producer = entry;
    if (StripDraw::Build(material.draw, material.coordinates, positions.data(), rgba.data(),
                        count, effects, candidate.draw) != StripDraw::Result::Ok)
        return Result::InvalidGeometry;
    if (candidate.draw.verts.empty()) return Result::Empty;
    out = std::move(candidate);
    return Result::Ok;
}

}  // namespace title
