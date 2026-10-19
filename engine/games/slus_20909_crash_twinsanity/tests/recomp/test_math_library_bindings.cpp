#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"
#include "games/slus_20909_crash_twinsanity/hle/math/animation_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"
#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"
#include "games/slus_20909_crash_twinsanity/recomp/continuations.h"
#include "engine/ipu/hle/guest_memory.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <vector>

class TestMathLibraryBindings final : public ps2r::ipu::GuestMemory {
public:
    uint8_t* Translate(uint32_t, size_t) override { ++accesses; return nullptr; }
    uint32_t Read32(uint32_t) override { ++accesses; throw std::runtime_error("unexpected memory read"); }
    void Write32(uint32_t, uint32_t) override { ++accesses; throw std::runtime_error("unexpected memory write"); }
    unsigned accesses = 0;
};

namespace {
unsigned checks = 0, failures = 0;
uint64_t exact_interiors = 0;
void Check(bool good, const char* message) {
    ++checks;
    if (!good && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
template<class Library>
uint64_t Generated(const Library& library) {
    const auto values = library.Counts().generated_body_calls;
    if constexpr (std::is_same_v<Library, title::AnimationMath>) return values;
    else return std::accumulate(values.begin(), values.end(), uint64_t{0});
}
template<class Library>
bool Accepted(uint32_t address) {
    return std::is_same_v<Library, title::MathLibrary> &&
        address == title::MathLibrary::kTransformPointAddress;
}
bool OriginalProof(uint32_t address, uint32_t entry) {
    const auto known = recomp_continuations(address);
    return known.count && std::binary_search(known.entries, known.entries + known.count,
        entry ? entry : address);
}
template<class Library>
void Selection(bool selected) {
    MathLibraryDispatchFixture::Reset();
    TestMathLibraryBindings memory;
    std::vector<uint32_t> addresses;
    uint64_t originals = 0;
    {
        Library library(memory);
        const auto capture = [&](uint32_t address) {
            addresses.push_back(address);
            MathLibraryDispatchFixture::Bind(address, [&, address](CpuContext& context, uint32_t entry) {
                ++originals;
                Check(Generated(library) == originals, "actual original counter increments before body execution");
                context.cpuRegs.GPR.r[2].UL[0] = address ^ entry;
            });
            return DispatchTable::Capture(address);
        };
        Check(library.Install(capture, DispatchTable::Install, selected), "real dispatch installation succeeds");
        unsigned repeated = 0;
        Check(!library.Install([&](uint32_t) -> typename Library::Body {
            ++repeated; return {};
        }, DispatchTable::Install, selected) && repeated == 0, "live owner refuses a duplicate batch before capture");
        uint64_t interiors = 0;
        for (uint32_t address : addresses) {
            const bool control = !selected && !Accepted<Library>(address);
            const bool full = control && OriginalProof(address, 0);
            Check(DispatchTable::CanResumeAt(address, 0) == full &&
                DispatchTable::CanResumeAt(address, address) == full,
                "control preserves original proof or its absence; selected/accepted entries remain opaque");
            Check(!DispatchTable::CanResumeAt(address, address + 1), "arbitrary byte offset has no replay proof");
            if (control) {
                for (uint32_t entry : {0u, address}) {
                    CpuContext context{};
                    if (full) Check(DispatchTable::InvokeAt(address, entry, context), "proven control entry resumes");
                    else {
                        Check(!DispatchTable::InvokeAt(address, entry, context),
                            "an original without continuation metadata remains unavailable to replay");
                        DispatchTable::Capture(address)(context, entry);
                    }
                    Check(context.cpuRegs.GPR.r[2].UL[0] == (address ^ entry),
                        "ordinary control dispatch still executes the exact original entry");
                }
            }
            const auto known = recomp_continuations(address);
            for (size_t i = 0; i < known.count; ++i) {
                const auto entry = known.entries[i];
                if (entry == address) continue;
                CpuContext context{};
                context.cpuRegs.GPR.r[4].UL[0] = 0xffffffff;
                Check(DispatchTable::CanResumeAt(address, entry) &&
                    DispatchTable::InvokeAt(address, entry, context) &&
                    context.cpuRegs.GPR.r[2].UL[0] == (address ^ entry),
                    "every generated interior resumes its captured body without entry arithmetic");
                ++interiors;
                ++exact_interiors;
            }
        }
        Check(library.Counts().generated_continuations == interiors && memory.accesses == 0,
            "actual interior census is preserved in both selections without evaluating math inputs");
        // Replace only the synthetic originals' observer for lifetime checks:
        // captured originals below must not themselves retain this library.
    }
    MathLibraryDispatchFixture::Reset();
    addresses.clear();
    originals = 0;
    {
        Library library(memory);
        Check(library.Install([&](uint32_t address) {
            addresses.push_back(address);
            MathLibraryDispatchFixture::Bind(address, [&, address](CpuContext& context, uint32_t entry) {
                ++originals; context.cpuRegs.GPR.r[2].UL[0] = address ^ entry;
            });
            return DispatchTable::Capture(address);
        }, DispatchTable::Install, selected), "lifetime fixture installs");
    }
    for (uint32_t address : addresses) {
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = 0xffffffff;
        DispatchTable::Capture(address)(context, 0);
        Check(context.cpuRegs.GPR.r[2].UL[0] == address, "callback forwards safely after quiescent owner destruction");
    }
    Check(originals == addresses.size() && memory.accesses == 0, "teardown never dereferences the former owner");
    MathLibraryDispatchFixture::Reset();
}

template<class Library>
void Rollback() {
    MathLibraryDispatchFixture::Reset();
    TestMathLibraryBindings memory;
    std::map<uint32_t, DispatchTable::Body> pristine;
    uint64_t originals = 0;
    Library library(memory);
    const auto capture = [&](uint32_t address) {
        if (!pristine.count(address)) {
            MathLibraryDispatchFixture::Bind(address, [&](CpuContext&, uint32_t) { ++originals; });
            pristine[address] = DispatchTable::Capture(address);
        }
        return DispatchTable::Capture(address);
    };
    unsigned attempts = 0;
    Check(!library.Install(capture, [&](uint32_t address, DispatchTable::Body body) {
        const bool installed = DispatchTable::Install(address, std::move(body));
        return installed && attempts++ != 0;  // Publish first callback, then reject.
    }, true), "rejected real publication rolls back");
    for (const auto& [address, body] : pristine) {
        (void)body;
        Check(DispatchTable::CanResumeAt(address, 0) == OriginalProof(address, 0),
            "rollback restores exact pristine full-entry metadata, including an absent proof");
    }
    Check(library.Install(capture, DispatchTable::Install, false), "failed owner may retry against pristine originals");
    for (const auto& [address, body] : pristine) {
        (void)body;
        if (Accepted<Library>(address)) continue;
        CpuContext context{};
        Check(DispatchTable::CanResumeAt(address, 0) == OriginalProof(address, 0),
            "retried control retains exact original proof");
        DispatchTable::Capture(address)(context, 0);
    }
    Check(Generated(library) == originals && originals > 0, "retry does not double-wrap actual execution counters");
    MathLibraryDispatchFixture::Reset();
}

void InteriorMode() {
    using Mode = DispatchTable::CountMode;
    constexpr uint32_t address = title::AnimationMath::kReadJointAngleAddress;
    const auto known = recomp_continuations(address);
    Check(known.count > 1, "mode fixture has an actual generated interior");
    if (known.count < 2) return;
    auto count = std::make_shared<std::atomic<uint64_t>>(0);
    auto enabled = std::make_shared<std::atomic<bool>>(false);
    const auto original = DispatchTable::GeneratedBody(address,
        [](CpuContext&, uint32_t) {}, known);
    const auto body = DispatchTable::CountCalls(address, original, count, Mode::InteriorCalls, enabled);
    CpuContext context{};
    body(context, known.entries[1]);
    Check(count->load() == 0, "inactive interior observer is disabled");
    enabled->store(true);
    body(context, 0);
    body(context, address);
    Check(count->load() == 0, "interior mode excludes both forms of full entry");
    body(context, known.entries[1]);
    Check(count->load() == 1, "interior mode counts the actual delegated call");
    Check(!DispatchTable::CountCalls(address, original, count, static_cast<Mode>(99)),
        "unknown counter modes still refuse");
}

void MissingMetadata() {
    // These diagnostic-prefixed generated originals currently have no sidecar.
    // Counting/selection must never invent replay proof to make a test pass.
    Check(recomp_continuations(title::MathLibrary::kTransformPointAddress).count == 0 &&
        recomp_continuations(title::MathLibrary::kRotationMatrixAddress).count == 0,
        "the two known metadata exclusions remain explicit checkpoint dependencies");
}
}

int main() {
    MissingMetadata();
    InteriorMode();
    for (bool selected : {false, true}) {
        Selection<title::MathLibrary>(selected);
        Selection<title::CollisionMath>(selected);
        Selection<title::TrigMath>(selected);
        Selection<title::AnimationMath>(selected);
    }
    Rollback<title::MathLibrary>();
    Rollback<title::CollisionMath>();
    Rollback<title::TrigMath>();
    Rollback<title::AnimationMath>();
    std::printf("MathLibraryBindings checks=%u failures=%u exact_interiors=%llu\n",
        checks, failures, static_cast<unsigned long long>(exact_interiors));
    return failures ? 1 : 0;
}
