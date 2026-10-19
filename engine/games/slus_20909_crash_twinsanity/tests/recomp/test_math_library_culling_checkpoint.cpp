#include "games/slus_20909_crash_twinsanity/hle/math/culling_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_state_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
using Library = title::CullingLibrary;
using Math = title::CullingMath;
using State = title::CullingState;
using Codec = title::CullingStateCodec;
using Registry = ps2r::savestate::SectionRegistry;
using Blob = ps2r::savestate::Blob;
std::vector<uint8_t> Bytes(const Library& library) {
    std::vector<uint8_t> bytes;
    Check(Codec::Encode(library.Capture(), bytes), "live source state encodes");
    return bytes;
}
Math::Matrix Identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}
std::vector<State::Snapshot> Snapshots() {
    State state;
    std::vector<State::Snapshot> states{state.Capture()};
    Math::Frustum planes{};
    planes.sides.offsets = {10, 10, 10, 10};
    planes.depth.offsets = {10, 10, 10, 10};
    planes.far_sides.offsets = {20, 20, 20, 20};
    state.LoadPlanes(planes);
    states.push_back(state.Capture());
    Math::View view{Identity(), Identity(), {3, 4, 5, 1}};
    const uint32_t words[]{0x80000000, 0x007fffff, 0x7fc01234, 0xff801234};
    std::memcpy(view.to_screen[1].data(), words, sizeof(words));
    state.LoadView(view);
    states.push_back(state.Capture());
    auto link = Identity();
    link[3] = {3, -2, 9, 1};
    Check(state.LoadLink(link, Identity()), "linked checkpoint fixture");
    states.push_back(state.Capture());
    Check(state.TestCell({{-1, -1, -1, 1}, {1, 1, 1, 1}}), "cell checkpoint fixture");
    states.push_back(state.Capture());
    Check(state.LoadModel(Identity()), "model checkpoint fixture");
    states.push_back(state.Capture());
    for (unsigned kind = 0; kind < 3; ++kind) {
        auto snapshot = state.Capture();
        Math::BoxView result{};
        result.distance = 123;
        if (kind == 0) result.visibility.outside = 0x40;
        else {
            result.to_screen = view.to_screen;
            if (kind == 2) {
                result.visibility.clipped = 0x20;
                result.to_clip = Identity();
            }
        }
        snapshot.result = result;
        states.push_back(snapshot);
    }
    return states;
}
}

class MathLibraryCullingCheckpointTest {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::vector<uint8_t> bytes = std::vector<uint8_t>(8192, 0xa5);
        uint8_t* Translate(uint32_t address, size_t length) override {
            if (address > bytes.size() || length > bytes.size() - address) return nullptr;
            return bytes.data() + address;
        }
        uint32_t Read32(uint32_t address) override {
            uint32_t result;
            std::memcpy(&result, Translate(address, 4), 4);
            return result;
        }
        void Write32(uint32_t address, uint32_t value) override {
            std::memcpy(Translate(address, 4), &value, 4);
        }
    };
    static std::optional<Library::Operation> Reader(const State::Snapshot& snapshot) {
        switch (snapshot.result.index()) {
            case 1: return Library::Operation::CellResult;
            case 2: return Library::Operation::DrawnModelResult;
            case 3: return Library::Operation::MeshResult;
            default: return std::nullopt;
        }
    }
public:
    static int Run() {
        Memory memory, restored_memory;
        Library library(memory), restored(restored_memory);
        Registry registry, destination;
        Check(title::CullingCheckpoint::Register(registry, library), "checkpoint owner registers");
        Check(!title::CullingCheckpoint::Register(registry, library), "duplicate owner refuses");
        unsigned unrelated_restores = 0;
        ps2r::savestate::SectionSpec marker;
        marker.tag = 0x54534554;  // TEST: check whole-candidate validation order.
        marker.required = false;
        marker.restore = [&](const Blob&, std::string*) { ++unrelated_restores; return true; };
        Check(destination.Register(marker), "earlier unrelated owner registers");
        Check(title::CullingCheckpoint::Register(destination, restored), "fresh source owner registers");
        for (const auto& snapshot : Snapshots()) {
            Check(library.Restore(snapshot), "source pending result initialized");
            const auto captured = registry.Capture();
            Check(captured.size() == 1 && captured[0].tag == Codec::kTag &&
                captured[0].version == 1 && captured[0].flags == ps2r::savestate::kSectionRequired,
                "CULL captured with mandatory versioned ownership");
            ps2r::savestate::FileMeta metadata;
            metadata.vsync_frame = 37;
            const auto file = ps2r::savestate::SerializeStateFile(metadata, captured);
            std::vector<Blob> decoded;
            std::string error;
            const auto tags = registry.KnownTags();
            Check(ps2r::savestate::ParseStateFile(file.data(), file.size(), &metadata, &decoded,
                &error, tags.data(), tags.size()), "CULL survives real file envelope and CRC");
            // Seed a different pending result to expose boot/live-state reuse.
            restored.Reset();
            auto unrelated = snapshot;
            unrelated.result = std::monostate{};
            Check(restored.Restore(unrelated), "different pre-restore state");
            Check(destination.Restore(decoded, &error), "fresh process owner restores CULL");
            Check(Bytes(restored) == captured[0].payload, "every raw source bit and pending kind restores");
            std::vector<uint8_t> appended{0x67, 0x89};
            registry.Specs()[0].serialize(appended);
            Check(appended.size() == captured[0].payload.size() + 2 && appended[0] == 0x67 &&
                appended[1] == 0x89 &&
                std::equal(captured[0].payload.begin(), captured[0].payload.end(), appended.begin() + 2),
                "owner follows section append contract");
            if (const auto reader = Reader(snapshot)) {
                memory.bytes.assign(memory.bytes.size(), 0xa5);
                restored_memory.bytes = memory.bytes;
                CpuContext before{}, after{};
                before.cpuRegs.GPR.r[4].UL[0] = after.cpuRegs.GPR.r[4].UL[0] = 0x1000;
                Check(library.Invoke(*reader, before) && restored.Invoke(*reader, after),
                    "pending reader succeeds after fresh restore");
                Check(memory.bytes == restored_memory.bytes &&
                    std::memcmp(&before, &after, sizeof(before)) == 0,
                    "restored reader produces identical owned objects and return ABI");
            }
            const auto stable = Bytes(restored);
            const auto refuse = [&](Blob bad) {
                const auto count = unrelated_restores;
                Check(!destination.Restore({Blob{marker.tag, 1, 0, {}}, std::move(bad)}, &error),
                    "malformed CULL refuses whole candidate");
                Check(Bytes(restored) == stable && unrelated_restores == count,
                    "prevalidation protects every owner before any restore");
            };
            for (size_t length = 0; length < captured[0].payload.size(); ++length) {
                auto bad = captured[0];
                bad.payload.resize(length);
                refuse(std::move(bad));
            }
            auto bad = captured[0]; ++bad.version; refuse(bad);
            bad = captured[0]; bad.flags = 0; refuse(bad);
            bad = captured[0]; bad.payload.push_back(0); refuse(bad);
            bad = captured[0]; bad.payload[4] = 2; refuse(bad);
            bad = captured[0]; bad.payload[0] ^= 1; refuse(bad);
            Check(!destination.Restore({}, &error) && Bytes(restored) == stable,
                "selected owner rejects legacy missing CULL without boot-state substitution");
            Registry control;
            Check(!control.Validate(captured, &error),
                "control selection rejects required CULL that it cannot own");
        }
        std::printf("MathLibraryCullingCheckpoint checks=%u failures=%u\n", checks, failures);
        return failures ? 1 : 0;
    }
};


// Legacy import contract pins. The THRD wire layout used here is the frozen
// one of engine/ee/savestate/thread_section.h (80-byte record header, parked
// CpuContext of ctx_size bytes, chain_depth x {func_addr, ra}); the
// application's ThreadSection::Parse is the codec of record and this reader
// exists only so the isolated test can hand chains to the contract.
namespace {
using Checkpoint = title::CullingCheckpoint;
uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
bool ParseThrd(const std::vector<uint8_t>& payload, std::vector<Checkpoint::ThreadChain>& chains, std::string* error) {
    if (payload.size() < 4) { if (error) *error = "short THRD"; return false; }
    const uint32_t count = U32(payload.data());
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 80 > payload.size()) { if (error) *error = "truncated record"; return false; }
        const uint8_t* header = payload.data() + offset;
        Checkpoint::ThreadChain chain;
        chain.tid = static_cast<int32_t>(U32(header));
        const bool started = header[60] != 0, has_ctx = header[61] != 0;
        const uint32_t ctx_size = U32(header + 72), depth = U32(header + 76);
        chain.live = started && has_ctx;
        offset += 80 + (has_ctx ? ctx_size : 0);
        for (uint32_t level = 0; level < depth; ++level, offset += 8) {
            if (offset + 8 > payload.size()) { if (error) *error = "truncated chain"; return false; }
            chain.functions.push_back(U32(payload.data() + offset));
        }
        chains.push_back(std::move(chain));
    }
    return true;
}
std::vector<uint8_t> Thrd(const std::vector<Checkpoint::ThreadChain>& chains) {
    std::vector<uint8_t> payload(4, 0);
    payload[0] = static_cast<uint8_t>(chains.size());
    for (const auto& chain : chains) {
        std::vector<uint8_t> header(80, 0);
        std::memcpy(header.data(), &chain.tid, 4);
        header[60] = header[61] = chain.live ? 1 : 0;
        const uint32_t ctx_size = chain.live ? sizeof(CpuContext) : 0, depth = static_cast<uint32_t>(chain.functions.size());
        std::memcpy(header.data() + 72, &ctx_size, 4);
        std::memcpy(header.data() + 76, &depth, 4);
        payload.insert(payload.end(), header.begin(), header.end());
        if (chain.live) payload.resize(payload.size() + sizeof(CpuContext), 0);
        for (uint32_t function : chain.functions) {
            uint8_t frame[8]{};
            std::memcpy(frame, &function, 4);
            payload.insert(payload.end(), frame, frame + 8);
        }
    }
    return payload;
}
Blob ThrdBlob(const std::vector<Checkpoint::ThreadChain>& chains) {
    Blob blob;
    blob.tag = ps2r::savestate::TAG_THRD;
    blob.payload = Thrd(chains);
    return blob;
}
const std::vector<Checkpoint::ThreadChain>& BoundaryChains() {
    // Every legacy anchor's relaunched chains: the main loop at its frame
    // boundary and the parked second thread (anchor-thrd-contexts.log).
    static const std::vector<Checkpoint::ThreadChain> chains{
        {2, true, {0x1e7f80}},
        {1, true, {0x1a0150, 0x19fd58, 0x17d558, 0x17ce50}},
        {3, false, {}}};
    return chains;
}
void LegacyImportPins(const char* anchors_root) {
    std::string reason;
    // Pure contract over sections.
    Check(Checkpoint::LegacyImportAdmissible({ThrdBlob(BoundaryChains())}, ParseThrd, nullptr),
        "boundary chains without a chunk view admit the legacy image");
    Check(!Checkpoint::LegacyImportAdmissible({}, ParseThrd, &reason) && reason.find("no THRD") != std::string::npos,
        "legacy image without THRD is refused with the reason");
    Check(!Checkpoint::LegacyImportAdmissible({ThrdBlob(BoundaryChains())}, {}, &reason) &&
        reason.find("thread chain codec") != std::string::npos,
        "legacy image without an application chain codec is refused");
    for (uint32_t live : {Checkpoint::kDrawScene, 0x1f4e50u, 0x1f5fb0u, 0x1e8778u}) {
        auto chains = BoundaryChains();
        chains[1].functions.insert(chains[1].functions.begin(), {0x1f5f90u == live ? 0x1ecb58u : live});
        if (live != Checkpoint::kDrawScene) chains[1].functions.insert(chains[1].functions.begin() + 1, Checkpoint::kDrawScene);
        Check(!Checkpoint::LegacyImportAdmissible({ThrdBlob(chains)}, ParseThrd, &reason) &&
            reason.find("live chunk view") != std::string::npos && reason.find("tid=1 level=0") != std::string::npos,
            "a relaunched frame inside DrawScene or a culling entry refuses the legacy image");
    }
    {
        auto chains = BoundaryChains();
        chains[2].functions = {Checkpoint::kDrawScene};  // Not started: never relaunched.
        Check(Checkpoint::LegacyImportAdmissible({ThrdBlob(chains)}, ParseThrd, nullptr),
            "only live (started, parked) chains decide the import");
    }
    Blob owned;
    owned.tag = Codec::kTag;
    Check(Checkpoint::LegacyImportAdmissible({owned}, {}, nullptr), "an image carrying CULL needs no import");
    // Registered behaviour: the whole-candidate validation refuses before any
    // owner restores; an admitted legacy image restores the empty boundary
    // state and counts the import; new captures still flag CULL required.
    MathLibraryCullingCheckpointTest::Memory memory;
    Library library(memory);
    Registry registry;
    unsigned unrelated_restores = 0;
    ps2r::savestate::SectionSpec marker;
    marker.tag = ps2r::savestate::TAG_THRD;
    marker.required = false;
    marker.restore = [&](const Blob&, std::string*) { ++unrelated_restores; return true; };
    Check(registry.Register(marker), "thread owner stand-in registers");
    Check(Checkpoint::Register(registry, library, ParseThrd), "selected owner registers with the chain codec");
    State::Snapshot live = Snapshots().back();
    Check(library.Restore(live), "live owned state before the legacy restore");
    const auto stable = Bytes(library);
    const auto captured = registry.Capture();
    Check(captured.size() == 2 && captured[1].tag == Codec::kTag && captured[1].flags == ps2r::savestate::kSectionRequired,
        "new captures carry CULL as a required section");
    std::string error;
    auto refused = BoundaryChains();
    refused[1].functions.insert(refused[1].functions.begin(), Checkpoint::kDrawScene);
    Check(!registry.Restore({ThrdBlob(refused)}, &error) && unrelated_restores == 0 && Bytes(library) == stable &&
        error.find("checkpoint rejected by CullingLibrary") != std::string::npos,
        "a live chunk view refuses the whole candidate before any owner restores");
    Check(registry.Restore({ThrdBlob(BoundaryChains())}, &error) && unrelated_restores == 1,
        "boundary legacy image restores");
    const auto snapshot = library.Capture();
    Check(!snapshot.view && !snapshot.link && !snapshot.planes && snapshot.result.index() == 0 &&
        library.Counts().legacy_imports == 1,
        "legacy import leaves the owned state empty (the source's boundary state) and is counted");
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x1000;
    Check(!library.Invoke(Library::Operation::CellResult, context),
        "no reader result is fabricated after a legacy import");
    Check(registry.Restore({ThrdBlob(BoundaryChains()), captured[1]}, &error) && library.Counts().legacy_imports == 1 &&
        Bytes(library) == stable, "an image carrying CULL restores the owned state without an import");
    // The eleven standing legacy anchors (read-only, skipped when absent). Only
    // the section headers and the THRD payload are read (the frozen envelope,
    // savestate_format.h); the 32 MB RAM payloads are seeked over, not hashed.
    // The anchors root is the test command's argument (CMake passes
    // ${PS2R_OG_ROOT}/save_states); the single getenv site stays in config_loader.
    const char* roots[]{anchors_root};
    const char* names[]{"artdet_f37152", "ps2state_slot0_f36935", "ps2state_slot0_f39807", "ps2state_slot0_f40391",
        "ps2state_slot0_f44111", "ps2state_slot0_f46203_red_crystal", "ps2state_slot0_f46949_yellow_crystal",
        "ps2state_slot0_f57292", "ps2state_slot0_f59991", "slowspot_f37092_s1789428268", "slowspot_f40298_s1789693059"};
    unsigned admitted = 0;
    for (const char* root : roots) {
        if (!root) continue;
        for (const char* name : names) {
            std::ifstream file(std::string(root) + "/" + name + ".ps2st", std::ios::binary);
            if (!file) continue;
            ps2r::savestate::FileHeader header{};
            file.read(reinterpret_cast<char*>(&header), sizeof(header));
            std::vector<Blob> sections;
            for (uint32_t i = 0; file && i < header.section_count; ++i) {
                ps2r::savestate::SectionHeader section{};
                file.read(reinterpret_cast<char*>(&section), sizeof(section));
                Blob blob;
                blob.tag = section.tag;
                blob.version = section.version;
                blob.flags = section.flags;
                if (section.tag == ps2r::savestate::TAG_THRD) {
                    blob.payload.resize(static_cast<size_t>(section.payload_len));
                    file.read(reinterpret_cast<char*>(blob.payload.data()), static_cast<std::streamsize>(section.payload_len));
                } else {
                    file.seekg(static_cast<std::streamoff>(section.payload_len), std::ios::cur);
                }
                sections.push_back(std::move(blob));
            }
            Check(std::memcmp(header.magic, ps2r::savestate::kMagic, sizeof(header.magic)) == 0 && !sections.empty(),
                "standing legacy anchor envelope reads");
            Check(Checkpoint::LegacyImportAdmissible(sections, ParseThrd, &reason), "standing legacy anchor is admissible");
            ++admitted;
        }
        if (admitted) break;
    }
    std::printf("MathLibraryCullingCheckpoint legacy anchors admitted=%u\n", admitted);
}
}

int main(int argc, char** argv) {
    const char* anchors_root = argc > 1 ? argv[1] : nullptr;
    const int result = MathLibraryCullingCheckpointTest::Run();
    LegacyImportPins(anchors_root);
    std::printf("MathLibraryCullingCheckpoint total checks=%u failures=%u\n", checks, failures);
    return result || failures ? 1 : 0;
}
