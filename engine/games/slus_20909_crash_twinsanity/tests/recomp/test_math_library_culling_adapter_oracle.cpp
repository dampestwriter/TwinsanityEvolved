#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_culling_adapter_oracle.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_library.h"
#include "engine/ee/cpu/guest_park.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_state_codec.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"
#include "math_library_culling_image.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <stdexcept>

MathLibraryCullingAdapterOracle::Memory::Memory() : bytes(4 * 1024 * 1024) {}
uint8_t* MathLibraryCullingAdapterOracle::Memory::Translate(uint32_t address, size_t size) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || size > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}
uint32_t MathLibraryCullingAdapterOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, 4), 4);
    return value;
}
void MathLibraryCullingAdapterOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryCullingAdapterOracle;
using Library = title::CullingLibrary;
using Op = Library::Operation;
using Math = title::CullingMath;
using Dispatch = std::map<uint32_t, Library::Body>;
unsigned checks = 0, failures = 0;
std::map<uint32_t, uint64_t> selected_calls;
unsigned box_out = 0, box_partial = 0, box_inside = 0;
constexpr uint32_t kRenderer = 0x10000, kView = 0x11000, kLinked = 0x11200;
constexpr uint32_t kMatrices = 0x12000, kPlace = 0x12100, kPortal = 0x12200;
constexpr uint32_t kChunk = 0x12400, kObject = 0x12440, kModel = 0x13000, kBox = 0x13100;

void Check(bool value, const char* text) {
    ++checks;
    if (!value && failures++ < 16) std::fprintf(stderr, "%s\n", text);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
float Input(uint32_t& seed, bool raw) {
    const uint32_t bits = Next(seed);
    float value;
    std::memcpy(&value, &bits, 4);
    return raw ? value : static_cast<float>(static_cast<int32_t>(bits) / 65536) / 256.0f;
}
Math::Matrix Identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}
template<class T> void Put(Oracle::Memory& memory, uint32_t address, const T& value) {
    std::memcpy(memory.Translate(address, sizeof(value)), &value, sizeof(value));
}
void Fixture(Oracle::Memory& memory, uint32_t& seed, unsigned trial) {
    for (uint32_t offset = kRenderer; offset < kBox + 32; offset += 4)
        memory.Write32(offset, Next(seed));
    for (const uint32_t address : {kRenderer + 0x80, kRenderer + 0xc0, kView,
            kMatrices + 0x80, kPlace, kChunk, kObject, kModel}) {
        auto matrix = Identity();
        if (trial % 5) for (auto& row : matrix) for (auto& lane : row) lane = Input(seed, trial % 3 == 0);
        Put(memory, address, matrix);
    }
    const Math::Vector camera{3, 4, 5, 1}, guard{0.2f, 0.2f, 1, 1};
    Put(memory, kView + 0xe0, camera);
    Put(memory, kRenderer + 0x140, guard);
    memory.Write32(kView + 0xf4, kRenderer);
    memory.Write32(0x309840, kRenderer);
    Put(memory, 0x3234d0, Math::Vector{0, 0, 0, 1});
    title::CullingPlanes::Planes view{{{0, 0, 1, 10}, {1, 0, 0, 10}, {-1, 0, 0, 10},
                                      {0, 1, 0, 10}, {0, -1, 0, 10}, {0, 0, -1, 10}}};
    auto far = view;
    for (unsigned i = 1; i < 5; ++i) far[i][3] = 50;
    Put(memory, 0x3b4580, view); Put(memory, 0x3b45e0, far); Put(memory, kPortal, view);
    Math::Box box{{-1, -1, -1, 1}, {1, 1, 1, 1}};
    if (trial % 15 == 5) { box.minimum[2] = -11; box.maximum[2] = -9; }
    if (trial % 15 == 10) { box.minimum[2] = 9; box.maximum[2] = 11; }
    Put(memory, kBox, box);
}
void Arguments(CpuContext& context, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    context.cpuRegs.GPR.r[4].UD[0] = a0;
    context.cpuRegs.GPR.r[5].UD[0] = a1;
    context.cpuRegs.GPR.r[6].UD[0] = a2;
    context.cpuRegs.GPR.r[7].UD[0] = a3;
}
uint32_t Address(Op operation) {
    for (const auto& entry : Library::Entries()) if (entry.operation == operation) return entry.address;
    return 0;
}
void Compare(const Oracle::Memory& control_memory, const Oracle::Memory& selected_memory,
        const CpuContext& control, const CpuContext& selected, Op operation) {
    const bool same = std::memcmp(control_memory.bytes.data() + kRenderer,
        selected_memory.bytes.data() + kRenderer, kBox + 32 - kRenderer) == 0;
    if (!same && failures < 16) {
        for (uint32_t i = kRenderer; i < kBox + 32; ++i)
            if (control_memory.bytes[i] != selected_memory.bytes[i]) {
                std::fprintf(stderr, "entry=%x memory=%x actual=%02x control=%02x\n",
                    Address(operation), i, selected_memory.bytes[i], control_memory.bytes[i]);
                break;
            }
    }
    Check(same, "source objects differ from actual generated boundary");
    for (unsigned reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
        Check(std::memcmp(&control.cpuRegs.GPR.r[reg], &selected.cpuRegs.GPR.r[reg], 16) == 0,
            "callee-preserved ABI differs");
    if (operation == Op::CellResult || operation == Op::MeshResult ||
        operation == Op::InstanceResult || operation == Op::ConstructLinked)
        Check(control.cpuRegs.GPR.r[2].UD[0] == selected.cpuRegs.GPR.r[2].UD[0], "source return ABI differs");
}
void Call(Op operation, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
        Oracle::Memory& control_memory, Oracle::Memory& selected_memory,
        CpuContext& control, CpuContext& selected, const Dispatch& originals, const Dispatch& replacements) {
    Arguments(control, a0, a1, a2, a3); Arguments(selected, a0, a1, a2, a3);
    originals.at(Address(operation))(control, Address(operation));
    const auto before = Oracle::Counts();
    replacements.at(Address(operation))(selected, Address(operation));
    ++selected_calls[Address(operation)];
    const auto after = Oracle::Counts();
    Check(before.generated_bodies == after.generated_bodies &&
        before.actual_culling_executions == after.actual_culling_executions &&
        before.upload_requests == after.upload_requests && before.selection_requests == after.selection_requests,
        "selected boundary executes no generated body, culling model or transport request");
    Compare(control_memory, selected_memory, control, selected, operation);
}
void Restore(Library& library) {
    std::vector<uint8_t> bytes;
    Check(title::CullingStateCodec::Encode(library.Capture(), bytes), "adapter state encodes");
    title::CullingState::Snapshot decoded;
    Check(title::CullingStateCodec::Decode(bytes, decoded), "adapter state decodes");
    library.Reset();
    Check(library.Restore(decoded), "adapter restores before result reader or next producer");
}
void RejectionPins() {
    Oracle::Memory memory;
    Library library(memory);
    CpuContext context{};
    uint32_t seed = 0x47554152;
    Fixture(memory, seed, 0);
    Arguments(context, kView, 0, 0, 0);
    const auto unchanged = [&](Op operation) {
        const auto before_memory = memory.bytes;
        const auto before_context = context;
        std::vector<uint8_t> before_state, after_state;
        Check(title::CullingStateCodec::Encode(library.Capture(), before_state), "guard input state encodes");
        Check(!library.Invoke(operation, context), "unsupported boundary refuses before mutation");
        Check(title::CullingStateCodec::Encode(library.Capture(), after_state), "guard output state encodes");
        Check(memory.bytes == before_memory && std::memcmp(&context, &before_context, sizeof(context)) == 0 &&
            before_state == after_state, "invalid boundary leaves objects, context and owned state untouched");
    };
    unchanged(Op::MeshResult);
    Arguments(context, kView, kModel, 0, 0); unchanged(Op::LoadModel);
    Arguments(context, kView, kBox, kModel, 0); unchanged(Op::TestBoxAt);
    Arguments(context, kView, 0, 0, 0);
    Check(library.Invoke(Op::LoadView, context), "guard fixture acquires valid view");
    Arguments(context, kMatrices, kView, kPlace, 0);
    Check(library.Invoke(Op::SetUpChunk, context), "guard fixture acquires valid planes");
    Arguments(context, kView, kBox, kModel + 1, 0); unchanged(Op::TestBoxAt);
    Arguments(context, kMatrices, kView, kPlace, 0); unchanged(Op::SetUpPortal);
    Arguments(context, kLinked, kChunk, 0xfffffff0, 0); unchanged(Op::ConstructLinked);
    memory.Write32(kView + 0xf4, 0xfffffff0);
    Arguments(context, kView, 0, 0, 0); unchanged(Op::LoadView);
    Arguments(context, kMatrices, kView, kPlace, 0); unchanged(Op::SetUpChunk);
}
void InstallationPins() {
    Oracle::Memory memory;
    for (size_t failure = 0; failure < Library::Entries().size(); ++failure) {
        Library library(memory);
        unsigned installs = 0;
        Check(!library.Install([&](uint32_t address) -> Library::Body {
            if (address == Library::Entries()[failure].address) return {};
            return [](CpuContext&, uint32_t) {};
        }, [&](uint32_t, Library::Body) { ++installs; return true; }, true) && installs == 0,
            "missing original refuses whole batch before any install");
    }
    for (bool reject_rollback : {false, true}) {
        for (size_t failure = 0; failure < Library::Entries().size(); ++failure) {
            Dispatch dispatch;
            unsigned calls = 0, attempts = 0;
            uint32_t delegated = 0;
            const auto capture = [&](uint32_t) -> Library::Body {
                return [&](CpuContext& context, uint32_t entry) {
                    ++calls; delegated = entry; context.cpuRegs.GPR.r[2].UL[0] = 0x43554c4c;
                };
            };
            {
                Library library(memory);
                const bool installed = library.Install(capture, [&](uint32_t address, Library::Body body) {
                    const auto attempt = attempts++;
                    if (attempt > failure && reject_rollback) return false;
                    dispatch[address] = std::move(body);
                    // Reentrant invocation during publication must still be original.
                    CpuContext context{};
                    dispatch[address](context, address);
                    Check(context.cpuRegs.GPR.r[2].UL[0] == 0x43554c4c,
                        "partial publication never exposes selected producers/readers");
                    return attempt != failure;
                }, true);
                Check(!installed, "every injected installer failure rejects whole group");
                for (auto count : library.Counts().owned_calls) Check(count == 0, "failed batch owns no work");
            }
            for (const auto& [address, body] : dispatch) {
                CpuContext context{};
                const auto before = calls;
                body(context, address + 4);
                Check(calls == before + 1 && delegated == address + 4,
                    "failed rollback callbacks retain originals and entry after owner teardown");
            }
        }
    }
    for (size_t failure = 0; failure < Library::Entries().size(); ++failure) {
        Library library(memory);
        Dispatch dispatch;
        unsigned attempts = 0, originals = 0;
        bool threw = false;
        try {
            library.Install([&](uint32_t) -> Library::Body {
                return [&](CpuContext&, uint32_t) { ++originals; };
            }, [&](uint32_t address, Library::Body body) {
                const unsigned attempt = attempts++;
                if (attempt > failure) throw std::runtime_error("rollback refused");
                dispatch[address] = std::move(body);
                if (attempt == failure) throw std::runtime_error("publication failed");
                return true;
            }, true);
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "publication failed";
        }
        Check(threw, "throwing publication preserves its original failure after throwing rollback");
        for (const auto& [address, body] : dispatch) {
            CpuContext context{};
            const auto before = originals;
            body(context, address);
            Check(originals == before + 1, "throwing rollback leaves every published callback inactive");
        }
        for (auto count : library.Counts().owned_calls) Check(count == 0, "throwing batch owns no work");
    }
    Dispatch selected;
    unsigned actual_originals = 0;
    {
        Library library(memory);
        const auto capture = [&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++actual_originals; };
        };
        Check(library.Install(capture, [&](uint32_t address, Library::Body body) {
            selected[address] = std::move(body); return true;
        }, true), "selected group installs atomically");
        Check(!library.Install(capture, [](uint32_t, Library::Body) { return true; }, true),
            "second install cannot capture its own wrapper");
        // A refused selected call parks the leg visibly (one original call
        // against owned state would be incoherent) and never aborts: the test
        // sink turns the park into an unwind carrying the recorded reason.
        struct ParkUnwind { std::string reason, detail; };
        GuestPark::SetSink([](const ParkRecord& record) {
            throw ParkUnwind{record.reason, record.detail ? record.detail : ""};
        });
        CpuContext context{};
        std::string parked, detail;
        try { selected.at(0x1f4e50)(context, 0x1f4e6c); }
        catch (const ParkUnwind& unwind) { parked = unwind.reason; detail = unwind.detail; }
        Check(parked == "culling_library_interior" && detail == "load_view" &&
            library.Counts().unsupported_interiors == 1 && actual_originals == 0,
            "selected interior cannot read incompatible generated state: counted, parked with its reason");
        parked.clear();
        try { selected.at(0x1f6028)(context, 0x1f6028); }
        catch (const ParkUnwind& unwind) { parked = unwind.reason; detail = unwind.detail; }
        Check(parked == "culling_library_state" && detail == "mesh_result" &&
            library.Counts().unsupported_state == 1 && actual_originals == 0,
            "missing native state parks explicitly without unsafe original fallback");
        GuestPark::SetSink(nullptr);
    }
    CpuContext context{};
    selected.at(0x1f4e50)(context, 0x1f4e6c);
    Check(actual_originals == 1, "retained callback after quiescent teardown uses original safely");
    Dispatch control;
    uint32_t delegated = 0;
    Library library(memory);
    Check(library.Install([&](uint32_t) -> Library::Body {
        return [&](CpuContext&, uint32_t entry) { delegated = entry; };
    }, [&](uint32_t address, Library::Body body) {
        control[address] = std::move(body); return true;
    }, false), "control group installs");
    control.at(0x1f4e50)(context, 0x1f4e6c);
    Check(delegated == 0x1f4e6c && library.Counts().generated_body_calls[2] == 1 &&
        library.Counts().unsupported_interiors == 0, "control preserves and counts original interior execution");
}
void ContinuationPins() {
    Oracle::Memory memory;
    for (bool selected : {false, true}) {
        MathLibraryDispatchFixture::Reset();
        unsigned originals = 0;
        uint32_t delegated = 0;
        for (const auto& entry : Library::Entries())
            MathLibraryDispatchFixture::Bind(entry.address, [&](CpuContext&, uint32_t address) {
                ++originals; delegated = address;
            });
        Library library(memory);
        Check(library.Install(DispatchTable::Capture, DispatchTable::Install, selected),
            "source group composes with real dispatch wrappers");
        // Actual generated LoadView post-call continuation, plus a non-entry.
        Check(DispatchTable::CanResumeAt(0x1f4e50, 0x1f4e6c) == !selected,
            "control retains exact original interior proof; selected remains opaque");
        Check(DispatchTable::CanResumeAt(0x1f4e50, 0) == !selected,
            "source adapter never acquires generated full-entry replay proof");
        Check(!DispatchTable::CanResumeAt(0x1f4e50, 0x1f4e70),
            "contained address never invents continuation proof");
        CpuContext context{};
        Check(DispatchTable::InvokeAt(0x1f4e50, 0x1f4e6c, context) == !selected,
            "selected continuation refused before dispatch executes");
        Check(originals == (selected ? 0u : 1u) &&
            library.Counts().generated_body_calls[2] == originals &&
            (selected || delegated == 0x1f4e6c),
            "actual original execution count follows real resumed control body");
    }
    MathLibraryDispatchFixture::Reset();
}
void Sequence(int mode) {
    Check(std::fesetround(mode) == 0, "set caller rounding mode");
    Oracle::Memory control_memory, selected_memory;
    uint32_t seed = 0x424f554e;
    for (unsigned trial = 0; trial < 400; ++trial) {
        Fixture(control_memory, seed, trial);
        selected_memory.bytes = control_memory.bytes;
        CpuContext control{};
        control.cpuRegs.GPR.r[29].UD[0] = 0x3fe000;
        control.vuRegs.VF[0].F[3] = 1;
        for (unsigned reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 30, 31})
            for (auto& word : control.cpuRegs.GPR.r[reg].UL) word = Next(seed);
        auto selected = control;
        VuUnit unit(0);
        unit.WriteMicroMem(0, math_culling_oracle::kImage.data(), math_culling_oracle::kImage.size());
        Library library(selected_memory);
        Library control_library(control_memory);
        Dispatch originals, replacements;
        const auto install = [&](Library& owner, Dispatch& dispatch, Oracle::Memory& memory, bool replace) {
            Check(owner.Install([&](uint32_t address) -> Library::Body {
                return [&, address](CpuContext& context, uint32_t) {
                    Oracle::Invoke(address, context, memory, unit);
                };
            }, [&](uint32_t address, Library::Body body) {
                dispatch[address] = std::move(body); return true;
            }, replace), "selection-aware source group installs");
        };
        install(control_library, originals, control_memory, false);
        install(library, replacements, selected_memory, true);
        const auto call = [&](Op op, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
            Call(op, a0, a1, a2, a3, control_memory, selected_memory, control, selected, originals, replacements);
        };
        call(Op::LoadView, kView);
        Restore(library);
        const uint32_t place = trial % 7 == 0 ? kMatrices : kPlace;
        const uint32_t model = trial % 11 == 0 ? kView + 0x80 : kModel;
        call(Op::SetUpChunk, kMatrices, kView, place);
        call(Op::TestCell, kView, kBox);
        Restore(library);
        call(Op::CellResult, kView);
        call(Op::LoadModel, kView, model);
        Restore(library);
        call(Op::DrawnModelResult, kView);
        call(Op::TestBoxAt, kView, kBox, kModel);
        Restore(library);
        call(Op::InstanceResult, kView);
        call(Op::MeshResult, kView);
        const auto visibility = control_memory.Read32(kView + 0xf8);
        if (visibility == 0) ++box_out;
        else if (visibility == 1) ++box_inside;
        else if (visibility == 2) ++box_partial;
        const uint32_t chunk = trial % 13 == 0 ? kLinked + 0x110 : kChunk;
        call(Op::ConstructLinked, kLinked, chunk, kObject);
        Restore(library);
        call(Op::SetUpPortal, kMatrices, kLinked, kPlace, kPortal);
        call(Op::TestCell, kLinked, kBox);
        call(Op::CellResult, kLinked);
        call(Op::TestBoxAt, kLinked, kBox, kModel);
        Restore(library);
        call(Op::MeshResult, kLinked);
        call(Op::LoadModel, kLinked, kModel);
        call(Op::DrawnModelResult, kLinked);
        const auto on = library.Counts(), off = control_library.Counts();
        for (size_t index = 0; index < Library::Entries().size(); ++index) {
            Check(on.owned_calls[index] > 0 && on.owned_calls[index] == off.generated_body_calls[index],
                "both runtime selections count matching actual source operations");
            Check(on.generated_body_calls[index] == 0 && off.owned_calls[index] == 0,
                "selected original-body counters are zero with positive native work");
        }
        Check(on.unsupported_interiors == 0 && on.unsupported_state == 0,
            "verified sequences need no unsupported entry or state");
        Check(std::fegetround() == mode, "complete adapter sequence retains caller rounding");
    }
}
}

int main() {
    InstallationPins();
    ContinuationPins();
    RejectionPins();
    const int previous = std::fegetround();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) Sequence(mode);
    std::fesetround(previous);
    Check(box_out && box_partial && box_inside, "actual generated readers cover all three visibility outcomes");
    const auto counts = Oracle::Counts();
    std::printf("MathLibraryCullingAdapterOracle checks=%u failures=%u sequences=1600 control_culling_executions=%llu\n",
        checks, failures, static_cast<unsigned long long>(counts.actual_culling_executions));
    std::printf("control_upload_requests=%llu control_selection_requests=%llu visibility=%u/%u/%u\n",
        static_cast<unsigned long long>(counts.upload_requests),
        static_cast<unsigned long long>(counts.selection_requests), box_out, box_partial, box_inside);
    for (const auto& entry : Library::Entries())
        std::printf("%s actual_generated=%llu selected=%llu\n", entry.name,
            static_cast<unsigned long long>(counts.generated_bodies.at(entry.address)),
            static_cast<unsigned long long>(selected_calls[entry.address]));
    return failures ? 1 : 0;
}
