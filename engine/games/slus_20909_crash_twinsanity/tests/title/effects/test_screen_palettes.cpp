#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_palettes.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

// Same-input oracle for the colour-filter palette values: the complete
// generated NEW BlendPalettes body (0x1ae530) writes the model's alternating
// blend buffer on flat RAM; ScreenPalettes::Resolve must produce the same 256
// colours from the authored tables alone. With the retail ELF the authored
// tables and first-colour pointers are the real ones; otherwise synthetic.
namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext&) {
    std::fprintf(stderr, "unexpected palette oracle callee %08x\n", address);
    std::abort();
}
}
namespace {
using P = title::ScreenPalettes;
namespace ram = strip_oracle::memory;
unsigned checks = 0, cases = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("screen palettes case ") + std::to_string(cases) + " line " + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint32_t kTurn = 0x30a7e0, kColoursA = 0x30a7d8, kColoursB = 0x30a7e4;
constexpr uint32_t kBufferA = 0x3e0000, kBufferB = 0x3e1000;

bool Reader(uint32_t at, void* out, size_t size) {
    if (at > ram::bytes.size() || size > ram::bytes.size() - at) return false;
    std::memcpy(out, ram::bytes.data() + at, size);
    return true;
}
// Minimal ELF32 program-header walk: copies [address, address+size) of the
// retail image into the oracle RAM. Returns false when the ELF is absent.
bool LoadRetail(const char* path, uint32_t address, uint32_t size) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(file)), {});
    const auto word = [&](size_t at) { uint32_t v; std::memcpy(&v, elf.data() + at, 4); return v; };
    const auto half = [&](size_t at) { uint16_t v; std::memcpy(&v, elf.data() + at, 2); return v; };
    static const uint8_t magic[4] = {0x7f, 'E', 'L', 'F'};
    if (elf.size() < 0x34 || std::memcmp(elf.data(), magic, 4) != 0) return false;
    const uint32_t phoff = word(0x1c);
    const uint16_t phsize = half(0x2a), phnum = half(0x2c);
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t header = phoff + size_t(i) * phsize;
        if (header + 24 > elf.size()) return false;
        const uint32_t offset = word(header + 4), vaddr = word(header + 8), filesz = word(header + 16);
        if (address >= vaddr && address + size <= vaddr + filesz && size_t(offset) + (address - vaddr) + size <= elf.size()) {
            std::memcpy(ram::bytes.data() + address, elf.data() + offset + (address - vaddr), size);
            return true;
        }
    }
    return false;
}
void Synthetic() {
    for (uint32_t table = 0; table < P::kCount; ++table)
        for (uint32_t index = 0; index < 256; ++index)
            ram::write<uint32_t>(P::kTables + table * P::kStride + index * 4,
                uint32_t(index * 7 + table * 31) | uint32_t(index ^ table) << 8 | uint32_t(255 - index) << 16 |
                uint32_t(table & 1 ? index : 0x80) << 24);
}
void Pointers() {
    for (uint32_t table = 0; table < P::kCount; ++table)
        ram::write<uint32_t>(P::kFirstColours + table * 4, P::kTables + table * P::kStride);
}
void InitBuffers() {
    // MakeFilterPalettes: both blend buffers start as palette 0, turn 0.
    for (uint32_t index = 0; index < 256; ++index) {
        const auto colour = ram::read<uint32_t>(P::kTables + index * 4);
        ram::write<uint32_t>(kBufferA + index * 4, colour);
        ram::write<uint32_t>(kBufferB + index * 4, colour);
    }
    ram::write<uint32_t>(kColoursA, kBufferA); ram::write<uint32_t>(kColoursB, kBufferB);
    ram::write<uint8_t>(kTurn, 0);
    ram::write<uint8_t>(P::kOn, 1);
}
void RunBlend(uint32_t palette, uint32_t second, float amount) {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    context.cpuRegs.GPR.r[4].UL[0] = palette;
    context.cpuRegs.GPR.r[5].UL[0] = second;
    context.fpuRegs.fpr[12].f = amount;
    context.vuRegs.VF[0].w = 1;
    strip_oracle::RunGenerated(0x1ae530, context);
}

void Blends(bool retail) {
    InitBuffers();
    const float amounts[] = {0.f, 1.f, .5f, .25f, .75f, .1f, .9f, 1.5f, -.5f, .333f, 2.f, .999f};
    unsigned turn = 0;
    for (unsigned variant = 0; variant < 48; ++variant) {
        ++cases;
        const uint32_t palette = variant % P::kCount, second = (variant / 3 + 2) % P::kCount;
        const float amount = amounts[variant % 12];
        RunBlend(palette, second, amount);
        turn ^= 1;
        CHECK(ram::read<uint8_t>(kTurn) == turn);
        // The source writes the buffer of the turn BEFORE flipping: turn 0 -> A.
        const uint32_t written = turn ? kBufferA : kBufferB;
        P::Palette model{}, owned{};
        CHECK(Reader(written, model.data(), sizeof(model)));
        CHECK(P::Resolve(Reader, palette, second, true, amount, owned) == P::Result::Ok);
        if (model != owned) {
            for (size_t i = 0; i < 4; ++i)
                std::fprintf(stderr, "entry %zu model %08x owned %08x (palette %u second %u amount %g)\n", i, model[i],
                             owned[i], palette, second, double(amount));
        }
        CHECK(model == owned);
        // Flat colour (retail), palette 0's alpha per entry.
        for (size_t i = 1; i < 256; ++i) CHECK((owned[i] & 0xffffff) == (owned[0] & 0xffffff));
        for (size_t i = 0; i < 256; ++i)
            CHECK((owned[i] >> 24) == (ram::read<uint32_t>(P::kTables + i * 4) >> 24));
        // The authored palette path is the table itself.
        P::Palette authored{}, table{};
        CHECK(P::Resolve(Reader, palette, 0, false, amount, authored) == P::Result::Ok);
        CHECK(Reader(P::kTables + palette * P::kStride, table.data(), sizeof(table)) && authored == table);
    }
    std::printf("ScreenPalettes: %s tables, 48 blends, %llu generated calls\n", retail ? "retail" : "synthetic",
                static_cast<unsigned long long>(strip_oracle::calls));
}

// TR renderer/effects.cpp:705-713 (InitialiseScreenEffects) places index i's
// colour i*0x01010101 at PaletteSlot(i): read back through Logical, the
// identity palette must be the identity. OLD R201 finding 14 recorded two
// disagreeing PSMT8H entry folds in the old decoder; the owned lookup applies
// the source-specified swap exactly once, at the index, never on the bytes.
void StorageOrder() {
    std::array<bool, 256> seen{};
    for (uint32_t i = 0; i < 256; ++i) {
        const auto slot = P::Slot(i);
        CHECK(slot < 256 && P::Slot(slot) == i); // an involution
        CHECK(((slot ^ i) & ~0x18u) == 0);       // only bits 3 and 4 move
        CHECK((P::Slot(i) == i) == (((i >> 3) & 1) == ((i >> 4) & 1)));
        CHECK(!seen[slot]);
        seen[slot] = true;
    }
    CHECK(P::Slot(8) == 16 && P::Slot(16) == 8 && P::Slot(0x0f) == 0x17 && P::Slot(0x17) == 0x0f);
    CHECK(P::Slot(0x28) == 0x30 && P::Slot(0xff) == 0xff && P::Slot(0x18) == 0x18);
    P::Palette identity_upload{}, logical{};
    for (uint32_t i = 0; i < 256; ++i) identity_upload[P::Slot(i)] = i * 0x01010101u;
    P::Logical(identity_upload, logical);
    for (uint32_t i = 0; i < 256; ++i) CHECK(logical[i] == i * 0x01010101u);
    // Not the identity in storage order: 128 entries move.
    unsigned moved = 0;
    for (uint32_t i = 0; i < 256; ++i) moved += identity_upload[i] != i * 0x01010101u;
    CHECK(moved == 128);
    // Aliasing in place is allowed and reversible.
    P::Palette in_place = identity_upload;
    P::Logical(in_place, in_place);
    CHECK(in_place == logical);
    P::Logical(in_place, in_place);
    CHECK(in_place == identity_upload);
    // The shadow depth palette (TR shadows.cpp:318-326): alpha ramps with the
    // logical index, white colour; storage order interleaves the ramp.
    P::Palette shadow_upload{};
    for (uint32_t i = 0; i < 256; ++i) shadow_upload[P::Slot(i)] = ((i + 1) & 0xffu) << 24 | 0x00ffffffu;
    P::Logical(shadow_upload, logical);
    for (uint32_t i = 0; i < 256; ++i) CHECK((logical[i] >> 24) == ((i + 1) & 0xffu) && (logical[i] & 0xffffffu) == 0xffffffu);
    CHECK((shadow_upload[8] >> 24) == 17 && (shadow_upload[16] >> 24) == 9);
}

void Refusals() {
    P::Palette out{};
    ram::write<uint8_t>(P::kOn, 0);
    CHECK(P::Resolve(Reader, 0, 1, true, .5f, out) == P::Result::Disabled);
    ram::write<uint8_t>(P::kOn, 1);
    CHECK(P::Resolve(Reader, P::kCount, 1, true, .5f, out) == P::Result::UnsupportedSource);
    CHECK(P::Resolve(Reader, 1, P::kCount, true, .5f, out) == P::Result::UnsupportedSource);
    CHECK(P::Resolve(Reader, 1, P::kCount, false, .5f, out) == P::Result::Ok); // second unused without blends
    ram::write<uint32_t>(P::kFirstColours + 12, P::kTables + 0x10); // a moved first-colour pointer
    CHECK(P::Resolve(Reader, 0, 1, true, .5f, out) == P::Result::UnsupportedSource);
    Pointers();
    CHECK(P::Resolve(nullptr, 0, 1, true, .5f, out) == P::Result::InvalidMemory);
}
}

int main(int argc, char** argv) {
    try {
        ram::bytes.fill(0);
        const bool retail = argc > 1 && LoadRetail(argv[1], P::kFirstColours, P::kCount * 4) &&
                            LoadRetail(argv[1], P::kTables, P::kCount * P::kStride);
        if (!retail) { Synthetic(); Pointers(); }
        else {
            // The retail pointer table must name the authored tables (pins the NTSC addresses).
            for (uint32_t table = 0; table < P::kCount; ++table)
                CHECK(ram::read<uint32_t>(P::kFirstColours + table * 4) == P::kTables + table * P::kStride);
        }
        Blends(retail);
        Refusals();
        StorageOrder();
        std::printf("ScreenPalettes: %u checks/%u cases; blend arithmetic, alpha and CSM1 storage order pinned against the generated body\n",
                    checks, cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
