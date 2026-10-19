// Independent geometry, numeric, lifetime and ordered-memory contracts. The
// separate generated oracle compares the complete entry/continuation ABI.
#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engine/ipu/hle/guest_memory.h"

namespace {
using Math = title::CollisionMath;
using Vector = Math::Vector;
int checks = 0, failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

class TestMathLibraryCollision final : public Math::GuestMemory {
public:
    std::array<uint8_t, 8192> bytes{};
    std::vector<std::pair<char, uint32_t>> accesses;

    uint8_t* Translate(uint32_t address, size_t length) override {
        if (address > bytes.size() || length > bytes.size() - address) std::abort();
        return bytes.data() + address;
    }
    uint32_t Read32(uint32_t address) override {
        accesses.emplace_back('r', address);
        uint32_t value;
        std::memcpy(&value, Translate(address, 4), 4);
        return value;
    }
    void Write32(uint32_t address, uint32_t value) override {
        accesses.emplace_back('w', address);
        std::memcpy(Translate(address, 4), &value, 4);
    }
    void Put(uint32_t address, const Vector& value) {
        std::memcpy(Translate(address, 16), value.data(), 16);
    }
    Vector Get(uint32_t address) {
        Vector value;
        std::memcpy(value.data(), Translate(address, 16), 16);
        return value;
    }
};

void TestEdges() {
    const Math::Triangle triangle{{{-1, -1, 0, 1}, {1, -1, 0, 1}, {0, 1, 0, 1}}};
    const auto start = Math::EvaluateStartEdges({0, 0, -1, 1}, {0, 0, 1, 1}, triangle);
    CHECK(start.normal == (Vector{0, 0, 4, 0}));
    CHECK(start.plane_values == (std::array<float, 2>{-4, 4}));
    const auto finish = Math::EvaluateFinishEdges(start);
    CHECK(finish.values == (std::array<float, 6>{-4, 4, -2, 6, -2, 6}));
    const auto outside = Math::EvaluateStartEdges({4, 0, -1, 1}, {4, 0, 1, 1}, triangle);
    CHECK(outside.plane_values == start.plane_values);
    const auto outside_edges = Math::EvaluateFinishEdges(outside);
    CHECK(outside_edges.values[2] > 0 && outside_edges.values[3] > 0);
    const auto miss = Math::EvaluateStartEdges({0, 0, 1, 1}, {0, 0, 2, 1}, triangle);
    CHECK(miss.plane_values == (std::array<float, 2>{4, 8}));

    TestMathLibraryCollision memory;
    for (uint32_t i = 0; i < 3; ++i) memory.Put(0x100 + i * 16, triangle[i]);
    memory.Put(0x200, {0, 0, 1, 1});
    memory.Put(0x210, {0, 0, 2, 1});
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x200;
    context.cpuRegs.GPR.r[5].UL[0] = 0x210;
    context.cpuRegs.GPR.r[6].UL[0] = 0x100;
    context.cpuRegs.GPR.r[7].UL[0] = 0x300;
    context.vuRegs.VF[17].I[3] = 0x0000807f;
    context.vuRegs.ACC.I[3] = 0x7fa12345;
    Math math(memory);
    math.StartTriangleEdgeTests(context);
    CHECK(memory.Read32(0x300) == Bits(4));
    CHECK(memory.Read32(0x304) == Bits(8));
    CHECK(math.Counts().entries[1] == 0);  // Plane miss need not finish the edges.
    CHECK(context.vuRegs.VF[17].I[3] == 0x0000807f);
    CHECK(context.vuRegs.ACC.I[3] == 0x7fa12345);
    memory.Put(0x200, {0, 0, -1, 1});
    memory.Put(0x210, {0, 0, 1, 1});
    math.StartTriangleEdgeTests(context);
    context.cpuRegs.GPR.r[4].UL[0] = 0x300;
    math.FinishTriangleEdgeTests(context);
    for (uint32_t i = 0; i < 6; ++i) CHECK(memory.Read32(0x300 + i * 4) == Bits(finish.values[i]));
    CHECK(context.vuRegs.VF[17].I[3] == 0x0000807f);
    CHECK(math.Counts().entries[0] == 2);
    CHECK(math.Counts().entries[1] == 1);
}

void TestNumericPins() {
    Math::PointGroup group{};
    group[0].fill(-1);
    group[1].fill(Float(0x3f800001));
    const Vector axis{1, Float(0x3f7ffffe), 0, 0};
    CHECK(std::fesetround(FE_TONEAREST) == 0);
    for (float value : Math::ProjectGroup(group, axis).values) CHECK(Bits(value) == 0);
    CHECK(std::fesetround(FE_TOWARDZERO) == 0);
    for (float value : Math::ProjectGroup(group, axis).values) CHECK(Bits(value) == 0xb3800000);
    CHECK(std::fegetround() == FE_TOWARDZERO);

    Math::HullSupport support{};
    for (auto& packed : support) packed[0].fill(Float(0x0000807f));
    const auto tiny = Math::ProjectHullSupport(support, {1, 0, 0, 0});
    CHECK(Bits(tiny.range[0]) == 0x0000807f);
    CHECK(Bits(tiny.range[1]) == 0x0000807f);
    for (auto& row : support[0]) row.fill(Float(0x80000000));
    for (size_t i = 1; i < support.size(); ++i) support[i] = {};
    const auto zeros = Math::ProjectHullSupport(support, {1, 1, 1, 0});
    // Equal comparisons keep the first operand, including its zero sign.
    CHECK(Bits(zeros.range[0]) == 0x80000000);
    CHECK(Bits(zeros.range[1]) == 0x80000000);
    CHECK(std::fesetround(FE_TONEAREST) == 0);
}

void TestPacking() {
    TestMathLibraryCollision memory;
    constexpr uint32_t patterns[] = {0x0000807f, 0x7fa12345, 0x80000000, 0x3f800000};
    std::array<Vector, 12> points;
    for (size_t i = 0; i < points.size(); ++i) {
        for (size_t lane = 0; lane < 4; ++lane) points[i][lane] = Float(patterns[(lane + i) % 4]);
        memory.Put(0x100 + static_cast<uint32_t>(i * 16), points[i]);
    }
    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    context.cpuRegs.GPR.r[0].UD[0] = 0;
    context.cpuRegs.GPR.r[4].UD[0] = 0x107;
    context.cpuRegs.GPR.r[5].UD[0] = 3;
    Math math(memory);
    math.GroupPoints(context);
    for (uint32_t group = 0; group < 3; ++group)
        for (uint32_t lane = 0; lane < 3; ++lane)
            for (uint32_t point = 0; point < 4; ++point)
                CHECK(memory.Read32(0x100 + group * 48 + lane * 16 + point * 4) ==
                      Bits(points[group * 4 + point][lane]));
    CHECK(context.cpuRegs.GPR.r[8].UD[0] == 0x1c7);
    CHECK(context.cpuRegs.GPR.r[25].UD[0] == 0x197);
    CHECK(context.cpuRegs.GPR.r[9].UD[0] == 0);
    for (size_t lane = 0; lane < 4; ++lane)
        CHECK(context.cpuRegs.GPR.r[13].UL[lane] == Bits(points.back()[lane]));
    // The final original row, beyond the packed output, remains untouched.
    CHECK(std::memcmp(memory.bytes.data() + 0x1b0, points.back().data(), 16) == 0);
    CHECK(memory.accesses[15] == std::make_pair('r', 0x13cu));
    CHECK(memory.accesses[16] == std::make_pair('w', 0x100u));
}

void TestVertexDifferences(bool overlap) {
    TestMathLibraryCollision memory;
    auto identity = title::MathLibrary::EvaluateIdentity();
    identity[2][3] = Float(0x0000807f);  // Carried raw as each difference's w.
    for (uint32_t i = 0; i < 4; ++i) memory.Put(0x100 + i * 16, identity[i]);
    identity[2][3] = 0;
    for (uint32_t i = 0; i < 4; ++i) memory.Put(0x140 + i * 16, identity[i]);
    const std::array<Vector, 2> first{{{1, 2, 0, 1}, {3, 4, 0, 1}}};
    const std::array<Vector, 3> second{{{-1, 1, 0, 1}, {-2, 2, 0, 1}, {-3, 3, 0, 1}}};
    for (uint32_t i = 0; i < 2; ++i) memory.Put(0x300 + i * 16, first[i]);
    for (uint32_t i = 0; i < 3; ++i) memory.Put(0x400 + i * 16, second[i]);
    memory.Put(0x320, {31, 32, 33, 34});
    const Vector tail{Float(0x7fa12345), Float(0x0000807f), Float(0x80000000), 99};
    memory.Put(0x430, tail);
    const uint32_t moved = overlap ? 0x310 : 0x500;
    const uint32_t request[] = {0x100, 0x300, 2, 0x140, 0x400, 3, 0x600, moved};
    for (uint32_t i = 0; i < 8; ++i) memory.Write32(0x80 + i * 4, request[i]);
    memory.accesses.clear();
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x80;
    Math math(memory);
    math.MakeVertexDifferences(context);
    for (uint32_t i = 0; i < 2; ++i) CHECK(memory.Get(moved + i * 16) == first[i]);
    for (uint32_t i = 0; i < 3; ++i) CHECK(memory.Get(moved + (i + 2) * 16) == second[i]);
    for (uint32_t a = 0; a < 2; ++a) {
        for (uint32_t b = 0; b < 3; ++b) {
            const auto value = memory.Get(0x600 + (a * 3 + b) * 16);
            CHECK(value[0] == first[a][0] - second[b][0]);
            CHECK(value[1] == first[a][1] - second[b][1]);
            CHECK(value[2] == 0);
            CHECK(Bits(value[3]) == 0x0000807f);
        }
    }
    CHECK(std::memcmp(context.vuRegs.VF[10].F, tail.data(), 16) == 0);
    CHECK(context.cpuRegs.GPR.r[8].UL[0] == moved + 32);
    CHECK(context.cpuRegs.GPR.r[9].UL[0] == moved + 32);
    CHECK(context.cpuRegs.GPR.r[14].UL[0] == 0x660);
    CHECK(context.cpuRegs.GPR.r[15].UL[0] == moved + 80);
    const auto expect_prefetch_before_store = [&](uint32_t store, uint32_t tail_address) {
        // Check the first transformed-point store. With overlapping buffers,
        // the former tail address is read again later as an ordinary moved
        // point during pair generation; that read has a different successor.
        const auto write = std::find(memory.accesses.begin(), memory.accesses.end(), std::make_pair('w', store));
        CHECK(write != memory.accesses.end());
        if (write == memory.accesses.end()) return;
        const auto index = static_cast<size_t>(write - memory.accesses.begin());
        CHECK(index >= 4);
        if (index < 4) return;
        for (uint32_t lane = 0; lane < 4; ++lane)
            CHECK(memory.accesses[index - 4 + lane] == std::make_pair('r', tail_address + lane * 4));
    };
    expect_prefetch_before_store(moved + 16, 0x320);
    expect_prefetch_before_store(moved + 64, 0x430);
    CHECK(math.Counts().entries[2] == 1);
}

void TestProjectionTail(bool sixteen) {
    TestMathLibraryCollision memory;
    const uint32_t groups = sixteen ? 16 : 3;
    for (uint32_t group = 0; group < groups; ++group) {
        memory.Put(0x100 + group * 48, {float(group * 4), float(group * 4 + 1),
                                     float(group * 4 + 2), float(group * 4 + 3)});
        memory.Put(0x110 + group * 48, {0, 0, 0, 0});
        memory.Put(0x120 + group * 48, {0, 0, 0, 0});
    }
    const uint32_t tail = 0x100 + groups * 48;
    for (uint32_t row = 0; row < (sixteen ? 6u : 3u); ++row)
        memory.Put(tail + row * 16, {Float(0x0000807f + row), 42, Float(0x7fa12345), -0.0f});
    memory.Put(0x800, {1, 0, 0, 123});
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x100;
    context.cpuRegs.GPR.r[5].UL[0] = sixteen ? 0x800 : groups;
    context.cpuRegs.GPR.r[6].UL[0] = sixteen ? 0x900 : 0x800;
    context.cpuRegs.GPR.r[7].UL[0] = 0x900;
    Math math(memory);
    if (sixteen) math.SixteenGroupsRange(context);
    else math.GroupsRange(context);
    CHECK(memory.Read32(0x900) == Bits(0));
    CHECK(memory.Read32(0x904) == Bits(float(groups * 4 - 1)));
    for (uint32_t row = 0; row < (sixteen ? 6u : 3u); ++row) {
        const auto original = memory.Get(tail + row * 16);
        CHECK(std::memcmp(context.vuRegs.VF[2 + row].F, original.data(), 16) == 0);
    }
    CHECK(context.cpuRegs.GPR.r[10].UD[0] == 0x60000000);
}

void TestCallerPadding() {
    // MakeHullHullSpace's source pads six differences to eight by writing
    // index eight, leaving indices six and seven as prior scratch contents.
    // Packing/range replacement must consume those contents, not silently
    // repair the caller by repeating its last valid point.
    TestMathLibraryCollision memory;
    for (uint32_t i = 0; i < 6; ++i) memory.Put(0x100 + i * 16, {float(i), 0, 0, 1});
    memory.Put(0x160, {-10, 0, 0, 1});
    memory.Put(0x170, {20, 0, 0, 1});
    memory.Put(0x180, memory.Get(0x150));  // Source's existing one-past write.
    memory.Put(0x300, {1, 0, 0, 0});
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x100;
    context.cpuRegs.GPR.r[5].UL[0] = 2;
    Math math(memory);
    math.GroupPoints(context);
    CHECK(memory.Get(0x130) == (Vector{4, 5, -10, 20}));
    CHECK(memory.Get(0x180) == (Vector{5, 0, 0, 1}));
    context.cpuRegs.GPR.r[6].UL[0] = 0x300;
    context.cpuRegs.GPR.r[7].UL[0] = 0x400;
    math.GroupsRange(context);
    CHECK(memory.Read32(0x400) == Bits(-10));
    CHECK(memory.Read32(0x404) == Bits(20));
}

void TestSupportLifetime() {
    Math::Triangle triangle{{{0, 0, 0, 1}, {2, 0, 0, 1}, {0, 3, 0, 1}}};
    std::array<Vector, 8> hull;
    for (unsigned i = 0; i < 8; ++i)
        hull[i] = {i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, i & 4 ? 1.0f : -1.0f, 1};
    const auto support = Math::BuildHullSupport(triangle, hull);
    constexpr std::array<Vector, 4> axes{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {2, -1, 3, 0}}};
    constexpr std::array<std::array<float, 2>, 4> ranges{{{-1, 3}, {-1, 4}, {-1, 1}, {-9, 10}}};
    TestMathLibraryCollision memory;
    for (uint32_t i = 0; i < 3; ++i) memory.Put(0x100 + i * 16, triangle[i]);
    for (uint32_t i = 0; i < 8; ++i) memory.Put(0x200 + i * 16, hull[i]);
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x100;
    context.cpuRegs.GPR.r[5].UL[0] = 0x200;
    Math math(memory);
    math.LoadTriangleHullSupport(context);
    triangle = {};
    hull = {};  // Owned support does not retain references to its inputs.
    title::MathLibrary matrices(memory);
    for (uint32_t i = 0; i < axes.size(); ++i) {
        CHECK(Math::ProjectHullSupport(support, axes[i]).range == ranges[i]);
        // This matches the source lifetime: unrelated work can clobber the
        // scratch VFs after loading, but the 18 grouped point rows survive.
        context.cpuRegs.GPR.r[4].UL[0] = 0x700;
        matrices.IdentityMatrix(context);
        memory.Put(0x300, axes[i]);
        context.cpuRegs.GPR.r[4].UL[0] = 0x300;
        context.cpuRegs.GPR.r[5].UL[0] = 0x400;
        math.TriangleHullSupportRange(context);
        CHECK(memory.Read32(0x400) == Bits(ranges[i][0]));
        CHECK(memory.Read32(0x404) == Bits(ranges[i][1]));
        for (size_t group = 0; group < 6; ++group)
            for (size_t row = 0; row < 3; ++row)
                CHECK(std::memcmp(context.vuRegs.VF[2 + group * 3 + row].F, support[group][row].data(), 16) == 0);
    }
    CHECK(math.Counts().entries[6] == 1);
    CHECK(math.Counts().entries[7] == 4);
}

void TestInstallationGuards() {
    TestMathLibraryCollision memory;
    Math math(memory);
    std::map<uint32_t, Math::Body> table;
    unsigned originals = 0, captures = 0;
    std::vector<std::pair<uint32_t, uint32_t>> forwarded;
    const Math::Capture capture = [&](uint32_t address) -> Math::Body {
        ++captures;
        return [&, address](CpuContext&, uint32_t entry) {
            uint64_t counted = 0;
            for (const auto value : math.Counts().generated_body_calls) counted += value;
            // Later rollback fixtures use separate owners; only this batch
            // uses the operation count checked here.
            if (originals < 23) CHECK(counted == originals + 1);
            ++originals;
            forwarded.emplace_back(address, entry);
        };
    };
    const Math::Installer install = [&](uint32_t address, Math::Body body) {
        table[address] = std::move(body);
        return true;
    };
    CHECK(!math.Install({}, install));
    CHECK(!math.Install(capture, {}));
    CHECK(!math.Install([&](uint32_t address) -> Math::Body {
        return address == Math::kLoadSupportAddress ? Math::Body{} : capture(address);
    }, install));
    CHECK(table.empty());
    captures = 0;
    CHECK(math.Install(capture, install));
    CHECK(captures == 8);
    CHECK(table.size() == 8);
    CpuContext context{};
    constexpr std::pair<uint32_t, uint32_t> interiors[] = {
        {Math::kVertexDifferencesAddress, 0x292c48}, {Math::kVertexDifferencesAddress, 0x292c70},
        {Math::kVertexDifferencesAddress, 0x292c78}, {Math::kVertexDifferencesAddress, 0x292ca0},
        {Math::kVertexDifferencesAddress, 0x292ca8}, {Math::kVertexDifferencesAddress, 0x292cb4},
        {Math::kVertexDifferencesAddress, 0x292cd0}, {Math::kVertexDifferencesAddress, 0x292ce0},
        {Math::kGroupPointsAddress, 0x292cf4}, {Math::kGroupPointsAddress, 0x292d3c},
        {Math::kGroupsRangeAddress, 0x292d74}, {Math::kGroupsRangeAddress, 0x292da0},
        {Math::kSixteenGroupsRangeAddress, 0x292e08}, {Math::kSixteenGroupsRangeAddress, 0x292e54},
    };
    for (const auto& interior : interiors) {
        table.at(interior.first)(context, interior.second);
        CHECK(forwarded.back() == interior);
    }
    CHECK(math.Counts().generated_continuations == 14);
    CHECK(memory.accesses.empty());
    for (uint32_t count : {0u, 0xffffffffu}) {
        context.cpuRegs.GPR.r[5].UL[0] = count;
        table.at(Math::kGroupPointsAddress)(context, 0);
        table.at(Math::kGroupsRangeAddress)(context, Math::kGroupsRangeAddress);
        memory.Write32(8, count);
        memory.Write32(20, 1);
        table.at(Math::kVertexDifferencesAddress)(context, 0);
        memory.Write32(8, 1);
        memory.Write32(20, count);
        table.at(Math::kVertexDifferencesAddress)(context, 0);
    }
    context.cpuRegs.GPR.r[0].UD[0] = 1;
    table.at(Math::kSixteenGroupsRangeAddress)(context, 0);
    CHECK(math.Counts().unsupported_inputs == 9);
    CHECK(originals == 23);
    const std::array<uint64_t, 8> expected_generated{0, 0, 12, 4, 4, 3, 0, 0};
    CHECK(math.Counts().generated_body_calls == expected_generated);
    for (uint64_t count : math.Counts().entries) CHECK(count == 0);
    for (const bool selected : {false, true}) {
    for (unsigned failure = 0; failure < 8; ++failure) {
    for (unsigned behavior = 0; behavior < 3; ++behavior) {
        std::map<uint32_t, Math::Body> failed_table;
        unsigned calls = 0;
        {
            Math failed(memory);
            bool threw = false;
            try {
                CHECK(!failed.Install(capture, [&](uint32_t address, Math::Body body) {
                    const unsigned request = calls++;
                    if (request > failure && behavior != 0) {
                        if (behavior == 2) throw std::runtime_error("restore rejected");
                        return false;
                    }
                    failed_table[address] = std::move(body);
                    if (request == failure && behavior == 2) throw std::runtime_error("install rejected");
                    return request != failure;  // Publish, then reject.
                }, selected));
            } catch (const std::runtime_error&) { threw = true; }
            CHECK(threw == (behavior == 2));
            CHECK(calls == (failure + 1) * 2);
        }
        const unsigned before = originals;
        for (const auto& [address, body] : failed_table) body(context, address);
        CHECK(originals == before + failure + 1);  // Safe after owner destruction.
    }
    }
    }
}

void TestControlInstallation() {
    TestMathLibraryCollision memory;
    Math math(memory);
    std::map<uint32_t, Math::Body> table;
    std::vector<uint32_t> addresses;
    std::array<uint64_t, 8> entered{};
    CHECK(math.Install([&](uint32_t address) -> Math::Body {
        const size_t index = addresses.size();
        addresses.push_back(address);
        return [&, index](CpuContext& context, uint32_t entry) {
            CHECK(math.Counts().generated_body_calls[index] == ++entered[index]);
            context.cpuRegs.GPR.r[2].UL[0] = entry;
        };
    }, [&](uint32_t address, Math::Body body) {
        table[address] = std::move(body);
        return true;
    }, false));
    for (const uint32_t address : addresses) {
        for (const uint32_t entry : {0u, address, address + 4}) {
            CpuContext context{};
            // Control preserves the original call without evaluating selected
            // helpers' input guards or reading invalid source pointers.
            context.cpuRegs.GPR.r[0].UD[0] = 1;
            context.cpuRegs.GPR.r[4].UL[0] = 0xffffffffu;
            table.at(address)(context, entry);
            CHECK(context.cpuRegs.GPR.r[2].UL[0] == entry);
        }
    }
    for (const auto count : math.Counts().generated_body_calls) CHECK(count == 3);
    for (const auto count : math.Counts().entries) CHECK(count == 0);
    CHECK(math.Counts().generated_continuations == 8 && math.Counts().unsupported_inputs == 0);
    CHECK(memory.accesses.empty());
    CHECK(math.Census().find("generated_body_sixteen_groups_range=3") != std::string::npos);
    for (const bool selected : {false, true}) {
        Math throwing(memory);
        CHECK(throwing.Install([](uint32_t) -> Math::Body {
            return [](CpuContext&, uint32_t) { throw std::runtime_error("original body entered"); };
        }, [&](uint32_t address, Math::Body body) {
            table[address] = std::move(body);
            return true;
        }, selected));
        bool caught = false;
        CpuContext context{};
        // A control full entry and a selected unsupported full entry both
        // enter the original; a throw must not erase their entry count.
        context.cpuRegs.GPR.r[0].UD[0] = 1;
        try { table.at(Math::kStartEdgesAddress)(context, 0); }
        catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && throwing.Counts().generated_body_calls[0] == 1);
        CHECK(throwing.Counts().unsupported_inputs == (selected ? 1 : 0));
        CHECK(throwing.Counts().entries[0] == 0);
    }
}
}  // namespace

int main() {
    const int saved = std::fegetround();
    CHECK(std::fesetround(FE_TONEAREST) == 0);
    TestEdges();
    TestNumericPins();
    TestPacking();
    TestVertexDifferences(false);
    TestVertexDifferences(true);
    TestProjectionTail(false);
    TestProjectionTail(true);
    TestCallerPadding();
    TestSupportLifetime();
    TestInstallationGuards();
    TestControlInstallation();
    CHECK(std::fesetround(saved) == 0);
    std::printf("math_library_collision: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
