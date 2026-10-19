#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_blocks.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace title {
namespace {
uint32_t Address(uint32_t value) {
    const auto segment = value >> 29;
    return segment == 0 || segment == 4 || segment == 5 ? value & 0x1fffffff : 0;
}
template<class T> T ReadAt(const MaterialState::Read& read, uint32_t address) {
    T value{};
    address = Address(address);
    if (!read || !address || address >= 0x02000000 || sizeof(T) > 0x02000000 - address ||
        (address & (std::min<size_t>(alignof(T), 4) - 1)) || !read(address, &value, sizeof(T)))
        throw std::runtime_error("Distortion block CPU object unavailable");
    return value;
}
template<class T, size_t N> T Field(const std::array<uint8_t, N>& bytes, size_t offset) {
    T value; std::memcpy(&value, bytes.data() + offset, sizeof(T)); return value;
}
void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
}
DistortionBlocks::DistortionBlocks(Read read) : read_(std::move(read)) {}
DistortionBlocks::Runtime DistortionBlocks::RuntimeAt(uint32_t address) const {
    Runtime result;
    result.address = Address(address);
    const auto bytes = ReadAt<std::array<uint8_t, 0x110>>(read_, address);
    result.count = Field<int16_t>(bytes, 0x100); result.system = Field<int16_t>(bytes, 0x102);
    result.wanted = Field<int16_t>(bytes, 0x10c); result.maximum = Field<int16_t>(bytes, 0x10e);
    Require(result.count >= 0 && result.count <= 32 && result.wanted >= 0 && result.wanted <= 32 &&
            result.system >= 0 && result.system < 300, "Distortion runtime bounds");
    const auto system = ReadAt<uint32_t>(read_, kSystems + uint32_t(result.system) * 4);
    Require(Address(system) && Address(system) <= 0x01ffffdd, "Distortion system bounds");
    result.hexagons = ReadAt<int8_t>(read_, Address(system) + 0x22) == 7;
    std::set<uint32_t> unique;
    for (int i = 0; i < result.count; ++i) {
        result.blocks[size_t(i)] = Address(Field<uint32_t>(bytes, size_t(i) * 4));
        Require(result.blocks[size_t(i)] && unique.insert(result.blocks[size_t(i)]).second,
                "Distortion runtime duplicates a block");
    }
    return result;
}
DistortionBlocks::Snapshot DistortionBlocks::Initialized() const {
    const auto count = ReadAt<int32_t>(read_, kCount);
    const auto pool = Address(ReadAt<uint32_t>(read_, kPool));
    Require(count >= 0 && count <= 4096 && pool && pool <= 0x02000000 - uint32_t(count) * 4 &&
            state_.generation != UINT64_MAX, "Distortion pool bounds/generation");
    Snapshot next;
    next.pool = pool; next.generation = state_.generation + 1;
    for (int32_t i = 0; i < count; ++i) {
        const auto block = Address(ReadAt<uint32_t>(read_, pool + uint32_t(i) * 4));
        Require(block && !(block & 15) && block <= 0x02000000 - 0x1b0 &&
                next.next.emplace(block, 0).second, "Distortion pool block bounds/identity");
    }
    Require(Validate(next), "Distortion initialized pool overlaps");
    return next;
}
void DistortionBlocks::Chain(Snapshot& state, const Runtime& runtime) {
    for (int i = 0; i < runtime.count; ++i)
        Require(state.next.count(runtime.blocks[size_t(i)]) != 0, "Distortion runtime has a foreign block");
    for (int i = 0; i < runtime.count; ++i)
        state.next.at(runtime.blocks[size_t(i)]) = i + 1 == runtime.count ? 0 : runtime.blocks[size_t(i + 1)];
}
DistortionBlocks::Snapshot DistortionBlocks::Resized(const Runtime& before, const Runtime& after) const {
    auto candidate = state_;
    Require(before.address == after.address && before.system == after.system &&
            before.hexagons == after.hexagons, "Distortion resize changed emitter identity");
    if (!before.hexagons || before.count == before.wanted) return candidate;
    if (after.count == before.count) {
        for (int i = 0; i < before.count; ++i)
            Require(before.blocks[size_t(i)] == after.blocks[size_t(i)], "Failed resize changed a live block");
        return candidate; // source capacity/event exhaustion preserved
    }
    Require(after.count == before.wanted && before.wanted == after.wanted,
            "Distortion resize did not publish the source block count");
    for (int i = 0; i < before.count; ++i)
        Require(candidate.next.count(before.blocks[size_t(i)]) != 0, "Distortion lost an old emitter block");
    if (after.count < before.count) {
        // The first released block of an all-block release keeps its old
        // successor. Later released blocks become independent draw entries.
        for (int i = after.count; i < before.count; ++i)
            if (i) candidate.next.at(before.blocks[size_t(i)]) = 0;
    }
    if (after.maximum != 0) Chain(candidate, after);
    Require(Validate(candidate), "Distortion resized block ownership is invalid");
    return candidate;
}
DistortionBlocks::EventPlan DistortionBlocks::Events() const {
    EventPlan plan{state_, {}};
    const auto index = ReadAt<int32_t>(read_, kEventIndex);
    Require(index >= 0 && index < 32, "Distortion event wheel index");
    auto address = Address(ReadAt<uint32_t>(read_, kEventWheel + uint32_t(index) * 4));
    std::set<uint32_t> seen;
    while (address) {
        Require(seen.size() < 16384 && seen.insert(address).second, "Distortion event wheel cycle/limit");
        const auto event = ReadAt<std::array<uint32_t, 5>>(read_, address);
        address = Address(event[4]);
        Require(!event[4] || address, "Distortion event successor segment");
        if (event[1]) continue; // source handles delay before dispatching kind
        const auto block = Address(event[0]);
        if (event[2] == 9) {
            Require(plan.state.next.count(block) != 0, "Distortion end event has no owned block");
            plan.state.next.at(block) = 0;
        } else if (event[2] == 1) {
            const auto runtime_address = Address(event[3]);
            auto found = plan.final_runtimes.find(runtime_address);
            if (found == plan.final_runtimes.end()) {
                auto runtime = RuntimeAt(runtime_address);
                if (!runtime.hexagons) continue;
                found = plan.final_runtimes.emplace(runtime_address, std::move(runtime)).first;
            }
            auto& runtime = found->second;
            const auto end = runtime.blocks.begin() + runtime.count;
            const auto at = std::find(runtime.blocks.begin(), end, block);
            Require(at != end, "Distortion block-taken event has no matching block");
            std::move(at + 1, end, at); runtime.blocks[size_t(--runtime.count)] = 0;
            if (runtime.count) Chain(plan.state, runtime);
        } else if (event[2] == 2) {
            Require(!plan.state.next.count(block), "Hexagon block has an ordinary-particle end event");
        }
    }
    Require(Validate(plan.state), "Distortion event plan has invalid ownership");
    return plan;
}
void DistortionBlocks::VerifyEvents(const EventPlan& plan) const {
    for (const auto& pair : plan.final_runtimes) {
        const auto& expected = pair.second;
        const auto actual = RuntimeAt(pair.first);
        Require(actual.count == expected.count, "Distortion event source count differs from plan");
        for (int i = 0; i < actual.count; ++i)
            Require(actual.blocks[size_t(i)] == expected.blocks[size_t(i)],
                    "Distortion event source block order differs from plan");
    }
}
[[noreturn]] void DistortionBlocks::Fail(const char* text) {
    if (!faulted_) ++counts_.failures;
    faulted_ = true;
    throw std::runtime_error(text);
}
void DistortionBlocks::Observe(uint32_t boundary, CpuContext& context, uint32_t entry, const Body& original) {
    if (faulted_ || busy_ || !original || (entry && entry != boundary) ||
        (boundary != kInitialize && boundary != kResize && boundary != kEvents))
        Fail("Distortion block observer requires a whole serialized source operation");
    busy_ = true;
    try {
        if (boundary == kInitialize) {
            original(context, entry);
            state_ = Initialized(); ++counts_.initializes;
        } else {
            Require(state_.generation != 0, "Distortion block owner was not initialized/imported");
            if (boundary == kResize) {
                const auto before = RuntimeAt(context.cpuRegs.GPR.r[4].UL[0]);
                original(context, entry);
                state_ = Resized(before, RuntimeAt(before.address)); ++counts_.resizes;
            } else {
                auto plan = Events();
                original(context, entry);
                VerifyEvents(plan); state_ = std::move(plan.state); ++counts_.event_turns;
            }
        }
        busy_ = false;
    } catch (...) {
        busy_ = false;
        if (!faulted_) ++counts_.failures;
        faulted_ = true;
        throw;
    }
}
DistortionBlocks::Result DistortionBlocks::Capture(uint32_t first,
    std::vector<DistortionGeometry::Block>& out) {
    if (faulted_) return Result::Faulted;
    if (!state_.generation || busy_) return Result::NotInitialized;
    std::vector<DistortionGeometry::Block> blocks;
    std::set<uint32_t> seen;
    auto address = Address(first);
    if (!address) return Result::UnsupportedSource;
    try {
        while (address) {
            const auto found = state_.next.find(address);
            if (found == state_.next.end() || !seen.insert(address).second) return Result::UnsupportedSource;
            if (blocks.size() >= DistortionGeometry::kMaxBlocks) return Result::LimitExceeded;
            blocks.push_back(ReadAt<DistortionGeometry::Block>(read_, address + 0x20));
            address = found->second;
        }
    } catch (...) { return Result::InvalidMemory; }
    counts_.captured_blocks += blocks.size();
    out = std::move(blocks);
    return Result::Ok;
}
bool DistortionBlocks::CaptureState(Snapshot& out) const {
    if (Busy()) return false;
    out = state_; return true;
}
bool DistortionBlocks::Validate(const Snapshot& snapshot) {
    if (snapshot.version != 1 || snapshot.next.size() > 4096) return false;
    if (!snapshot.generation) return !snapshot.pool && snapshot.next.empty();
    if (!snapshot.pool || (snapshot.pool & 3) || snapshot.pool >= 0x02000000 ||
        snapshot.next.size() * 4 > 0x02000000 - snapshot.pool) return false;
    uint32_t end = 0;
    for (const auto& pair : snapshot.next) {
        if (!pair.first || (pair.first & 15) || pair.first < end || pair.first > 0x02000000 - 0x1b0 ||
            (pair.second && !snapshot.next.count(pair.second))) return false;
        end = pair.first + 0x1b0;
    }
    std::set<uint32_t> complete;
    for (const auto& pair : snapshot.next) {
        std::set<uint32_t> visit;
        for (auto at = pair.first; at && !complete.count(at); at = snapshot.next.at(at))
            if (!visit.insert(at).second) return false;
        complete.insert(visit.begin(), visit.end());
    }
    return true;
}
bool DistortionBlocks::Restore(const Snapshot& snapshot) {
    if (Busy() || state_.generation || !Validate(snapshot)) return false;
    state_ = snapshot; return true;
}
bool DistortionBlocks::Busy() const { return busy_ || faulted_; }
DistortionBlocks::Counters DistortionBlocks::Counts() const { return counts_; }
}  // namespace title
