#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using Assets = title::SkyAssets;
using Codec = title::SkyState;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "SkyState line%d: %s\n", __LINE__, #x); } } while (false)
class TestSkyState {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x3000);
    Assets::Snapshot snapshot;
    static constexpr uint32_t model = 0x100, texture = 0x200, counts = 0x300;
    TestSkyState() {
        title::SkyGeometry::Strip strip;
        for (unsigned i = 0; i < 3; ++i) {
            title::SkyGeometry::Vertex v{};
            v.position = {float(i), -0.0f, float(i) / 3};
            for (unsigned j = 0; j < 3; ++j) {
                const uint32_t bits = 0x3f800000u | (17 * i + j);
                std::memcpy(&v.stq[j], &bits, 4);
                v.colour[j] = uint8_t(bits);
            }
            v.colour[3] = uint8_t(128 + i);
            v.restart = i == 1;
            strip.vertices.push_back(v);
        }
        snapshot.models.push_back({model, 0x1234, {{{{strip}}}}});
        title::SkyTexture::Image image;
        image.width = image.height = 32; image.buffer_width = 64;
        image.levels = 3; image.function = 1; image.alpha = true;
        image.rgba.resize(32 * 32 * 4);
        for (size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = uint8_t(i * 37);
        snapshot.textures.push_back({texture, 0x4321, image});
        Put(model + 4, 0x1234); Put(model + 8, 1); Put(model + 12, counts); Put(counts, 3);
        Put(texture + 4, 0x4321); Put(texture + 8, 10000);
        Put(texture + 12 + 4, 5 | 5 << 16);
        Put(texture + 12 + 8, 3 | 0x13 << 8 | 1 << 24);
        Put(texture + 12 + 12, 1 << 8); Put(texture + 12 + 0x2c, 1);
    }
    void Put(uint32_t at, uint32_t value) { std::memcpy(ram.data() + at, &value, 4); }
    title::MaterialState::Read Read() const {
        return [this](uint32_t at, void* out, size_t size) {
            if (at > ram.size() || size > ram.size() - at) return false;
            std::memcpy(out, ram.data() + at, size);
            return true;
        };
    }
};
void RoundTrip() {
    TestSkyState f;
    CHECK(Assets::Preflight(f.snapshot, f.Read()));
    std::vector<uint8_t> bytes;
    CHECK(Codec::Encode(f.snapshot, bytes));
    Assets::Snapshot decoded;
    CHECK(Codec::Decode(bytes, decoded));
    std::vector<uint8_t> again;
    CHECK(Codec::Encode(decoded, again) && bytes == again);
    Assets owner;
    CHECK(owner.Restore(decoded));
    CHECK(!owner.Restore(decoded)); // never reset an existing allocation behind consumers
    auto lease = owner.FindModel(f.model);
    Assets::Snapshot captured;
    CHECK(!owner.Capture(captured));
    CHECK(lease->parts[0].strips[0].vertices[1].restart);
    CHECK(std::signbit(lease->parts[0].strips[0].vertices[0].position[1]));
    owner.ForgetModel(f.model);
    CHECK(lease->parts[0].strips[0].vertices[2].colour[3] == 130);
    CHECK(!owner.FindModel(f.model)); // old queued lease cannot become new generation
    lease.reset();
    auto image = owner.FindTexture(f.texture);
    CHECK(!owner.Capture(captured));
    owner.ForgetTexture(f.texture);
    CHECK(image->rgba == decoded.textures[0].image.rgba);
    CHECK(owner.Capture(captured) && captured.models.empty() && captured.textures.empty());
    CHECK(owner.Restore(decoded));
    CHECK(owner.FindTexture(f.texture) != image);
    image.reset();
    CHECK(owner.Capture(captured));
    CHECK(Codec::Encode(captured, again) && bytes == again);
    // Incoming software identity matters; relocation/slot bookkeeping does not.
    f.Put(f.model, 0x10005); f.Put(f.texture + 0x60, 123); f.Put(f.texture + 0x64, 0x9870);
    CHECK(Assets::Preflight(captured, f.Read()));
    f.Put(f.model + 4, 0x1235);
    CHECK(!Assets::Preflight(captured, f.Read()));
    f.Put(f.model + 4, 0x1234); f.Put(f.counts, 4);
    CHECK(!Assets::Preflight(captured, f.Read()));
    f.Put(f.counts, 3); f.Put(f.texture + 16, 6 | 5 << 16);
    CHECK(!Assets::Preflight(captured, f.Read()));
}
void InvalidState() {
    TestSkyState f;
    std::vector<uint8_t> bytes;
    CHECK(Codec::Encode(f.snapshot, bytes));
    for (size_t length : {size_t(0), size_t(15), size_t(16), size_t(28), bytes.size() - 1}) {
        Assets::Snapshot sentinel = f.snapshot;
        CHECK(!Codec::Decode({bytes.begin(), bytes.begin() + length}, sentinel));
        CHECK(sentinel.models.size() == 1 && sentinel.textures.size() == 1);
    }
    for (size_t at : {size_t(0), size_t(4), size_t(8), size_t(24), size_t(28), size_t(32), size_t(64)}) {
        auto invalid = bytes;
        for (size_t i = 0; i < 4; ++i) invalid[at + i] = 255;
        CHECK(!Codec::Validate(invalid));
    }
    auto trailing = bytes; trailing.push_back(0);
    CHECK(!Codec::Validate(trailing));
    auto invalid = f.snapshot;
    invalid.models[0].model.parts[0].strips[0].vertices[0].stq[0] = std::numeric_limits<float>::infinity();
    CHECK(!Assets::ValidateSnapshot(invalid));
    invalid = f.snapshot;
    invalid.models[0].model.parts[0].strips[0].vertices[0].colour[0] ^= 1;
    CHECK(!Assets::ValidateSnapshot(invalid)); // preserve UV mantissa/colour dual meaning
    invalid = f.snapshot; invalid.textures[0].object = f.model;
    CHECK(!Assets::ValidateSnapshot(invalid));
    invalid = f.snapshot; invalid.textures[0].image.rgba.pop_back();
    const auto previous = bytes;
    CHECK(!Codec::Encode(invalid, bytes) && bytes == previous);
}
}
int main() {
    RoundTrip(); InvalidState();
    std::printf("SkyState: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
