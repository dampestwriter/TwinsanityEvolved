#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_state.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_pass_fixture.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

// DSTN v1: the distortion owner's value state round-trips exactly, a model-only
// section is the fixed four-byte form, and every malformed or foreign image is
// refused before any RAM write could happen.
namespace {
using S = title::DistortionState;
using L = title::DistortionLibrary;
using F = DistortionPassFixture;
using Blob = ps2r::savestate::Blob;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion state line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Settings = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 56 | uint64_t{1} << 59;

L::Snapshot Selected() {
    L::Snapshot snapshot;
    snapshot.selected = true;
    snapshot.blocks.generation = 5; snapshot.blocks.pool = 0x210000;
    snapshot.blocks.next = {{F::Block0, F::Block1}, {F::Block1, 0}, {F::Block2, 0}};
    snapshot.settings = {1, F::Shader, 3, {Settings, -200, 1}};
    return snapshot;
}
Blob Ram() {
    Blob ram;
    ram.tag = ps2r::savestate::TAG_RAM;
    ram.version = 1;
    ram.payload.resize(0x02000000);
    CHECK(F::Reader(0, ram.payload.data(), 0x400000));
    return ram;
}

void RoundTrip() {
    const auto selected = Selected();
    std::vector<uint8_t> bytes;
    CHECK(S::Encode(selected, bytes));
    CHECK(bytes.size() == 4 + 4 + 8 + 4 + 3 * 8 + 4 + 8 + 8 + 2 + 2);
    L::Snapshot decoded;
    CHECK(S::Decode(bytes, decoded));
    CHECK(decoded.selected && decoded.blocks.generation == 5 && decoded.blocks.pool == 0x210000 &&
          decoded.blocks.next == selected.blocks.next && decoded.settings.shader == F::Shader &&
          decoded.settings.generation == 3 && decoded.settings.settings.attributes == Settings &&
          decoded.settings.settings.lod_k == -200 && decoded.settings.settings.lod_l == 1);
    std::vector<uint8_t> again;
    CHECK(S::Encode(decoded, again) && again == bytes);
    // Selected but never initialised/observed: legal empty owned values.
    L::Snapshot fresh; fresh.selected = true;
    CHECK(S::Encode(fresh, bytes) && S::Decode(bytes, decoded) && decoded.selected &&
          !decoded.blocks.generation && !decoded.settings.shader);
    // Model-only is the fixed four-byte form and carries no owned values.
    L::Snapshot model;
    CHECK(S::Encode(model, bytes) && bytes == std::vector<uint8_t>({1, 0, 0, 0}));
    CHECK(S::Decode(bytes, decoded) && !decoded.selected);
    model.blocks.generation = 1; model.blocks.pool = 0x210000;
    CHECK(!S::Encode(model, bytes));
    model = {}; model.settings.generation = 1;
    CHECK(!S::Encode(model, bytes));
}

void Refusals() {
    const auto selected = Selected();
    std::vector<uint8_t> bytes;
    CHECK(S::Encode(selected, bytes));
    L::Snapshot out;
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + long(cut));
        CHECK(!S::Decode(truncated, out));
    }
    auto extra = bytes; extra.push_back(0);
    CHECK(!S::Decode(extra, out));
    auto version = bytes; version[0] = 2;
    CHECK(!S::Decode(version, out));
    auto padding = bytes; padding[2] = 1;
    CHECK(!S::Decode(padding, out));
    auto model_with_payload = bytes; model_with_payload[1] = 0;
    CHECK(!S::Decode(model_with_payload, out));
    auto pool = bytes; pool[4] |= 1; // misaligned pool fails DistortionBlocks::Validate
    CHECK(!S::Decode(pool, out));
    std::vector<uint8_t> swapped = bytes; // the encoder emits sorted links; a swapped pair is refused
    std::swap_ranges(swapped.begin() + 20, swapped.begin() + 28, swapped.begin() + 28);
    CHECK(!S::Decode(swapped, out));
    auto fog = selected; fog.settings.settings.attributes |= uint64_t{1} << 30;
    CHECK(!S::Encode(fog, bytes));
    auto unaligned = selected; unaligned.settings.shader |= 2;
    CHECK(!S::Encode(unaligned, bytes));
    auto foreign = selected; foreign.blocks.next[F::Block0] = 0x201234; // link to a block outside the set
    CHECK(!S::Encode(foreign, bytes));
}

void Preflight() {
    F fixture(true);
    F::Write<uint32_t>(title::DistortionMaterial::kVtable + 0x2c, title::DistortionMaterial::kSettings);
    F::Write<uint64_t>(F::Shader, Settings);
    const Blob ram = Ram();
    const auto selected = Selected();
    std::vector<uint8_t> bytes;
    CHECK(S::Encode(selected, bytes));
    const Blob section{S::kTag, S::kVersion, ps2r::savestate::kSectionRequired, bytes};
    L::Snapshot out;
    title::MaterialState::Read incoming;
    std::string error;
    const std::vector<Blob> image{ram, section}; // the bound reader borrows this image
    CHECK(S::Preflight(image, true, out, incoming, &error) && out.selected &&
          out.settings.generation == 3 && incoming);
    uint32_t type = 0;
    CHECK(incoming(F::Shader + 0x64, &type, 4) && type == 0x18); // the bound reader sees the image
    incoming = {};
    CHECK(!S::Preflight({ram}, true, out, incoming, &error)); // selected needs its section
    CHECK(!S::Preflight({ram, section, section}, true, out, incoming, &error)); // duplicate
    CHECK(!S::Preflight({ram, ram, section}, true, out, incoming, &error));
    CHECK(!S::Preflight({section}, true, out, incoming, &error)); // no RAM0
    auto optional = section; optional.flags = 0;
    CHECK(!S::Preflight({ram, optional}, true, out, incoming, &error)); // selected must be required
    auto other = section; other.version = 2;
    CHECK(!S::Preflight({ram, other}, true, out, incoming, &error));
    auto flagged = section; flagged.flags |= 2;
    CHECK(!S::Preflight({ram, flagged}, true, out, incoming, &error));
    CHECK(!S::Preflight({ram, section}, false, out, incoming, &error)); // selection differs
    // The versioned shader must still be the retail type18 object in the image.
    auto moved = selected; moved.settings.shader = F::Shader + 0x100;
    std::vector<uint8_t> moved_bytes;
    CHECK(S::Encode(moved, moved_bytes));
    CHECK(!S::Preflight({ram, {S::kTag, S::kVersion, 1, moved_bytes}}, true, out, incoming, &error));
    Blob mutated = ram;
    mutated.payload[F::Shader + 0x6c] ^= 0x10; // vtable word changed
    CHECK(!S::Preflight({mutated, section}, true, out, incoming, &error));
    Blob short_ram = ram; short_ram.payload.resize(0x01000000);
    CHECK(!S::Preflight({short_ram, section}, true, out, incoming, &error));
    // Model-only images: the section may be absent or the four-byte form.
    L::Snapshot model;
    std::vector<uint8_t> model_bytes;
    CHECK(S::Encode(model, model_bytes));
    CHECK(S::Preflight({ram}, false, out, incoming, &error) && !out.selected);
    CHECK(S::Preflight({ram, {S::kTag, S::kVersion, 0, model_bytes}}, false, out, incoming, &error));
    CHECK(!S::Preflight({ram, {S::kTag, S::kVersion, 0, model_bytes}}, true, out, incoming, &error));
}
}

int main() {
    try {
        RoundTrip();
        Refusals();
        Preflight();
        std::printf("DistortionState: %u checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
