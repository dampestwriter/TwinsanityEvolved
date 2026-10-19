#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_trace.h"

#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace title {
struct ServiceTrace::State {
    Snapshot snapshot;
    Sink sink;
    uint64_t sequence = 0, failures = 0;
    std::vector<uint64_t> stack;
    bool installed = false, observing = false;
};
namespace {
// NTSC full entries already verified by the stream/audio owners. Include
// intent read/wait plus send/reply so direct selected file calls and retained
// asynchronous client callbacks can be distinguished in the same timeline.
constexpr std::array<uint32_t, 17> kEntries{
    0x2aac80, 0x2ad1a0, 0x2ada30, 0x2ada68, 0x1e8278, 0x1e2958, 0x1e1f48,
    0x1e6e60, 0x1e6dd0, 0x1e6e18, 0x1e7cb8, 0x1e3688, 0x1e1500, 0x1e30d0,
    0x1e6c28, 0x1e3e80, 0x1e18e8};
}
ServiceTrace::ServiceTrace(Snapshot snapshot, Sink sink) : state_(std::make_shared<State>()) {
    state_->snapshot = std::move(snapshot);
    state_->sink = std::move(sink);
}
ServiceTrace::~ServiceTrace() { Disable(); }
void ServiceTrace::Disable() {
    state_->snapshot = {};
    state_->sink = {};
}
uint64_t ServiceTrace::ObserverFailures() const { return state_->failures; }
bool ServiceTrace::Install(const StreamLibrary::Capture& capture, const StreamLibrary::Installer& install) {
    if (state_->installed) return true;
    if (!capture || !install || !state_->snapshot || !state_->sink) return false;
    std::array<StreamLibrary::Body, kEntries.size()> originals;
    for (size_t i = 0; i < kEntries.size(); ++i) {
        originals[i] = capture(kEntries[i]);
        if (!originals[i]) return false;
    }
    for (size_t i = 0; i < kEntries.size(); ++i) {
        const auto address = kEntries[i];
        const auto original = originals[i];
        if (!install(address, [state = state_, original, address](CpuContext& ctx, uint32_t entry) {
            Invoke(state, original, address, ctx, entry);
        })) {
            for (size_t j = 0; j < i; ++j) install(kEntries[j], originals[j]);
            return false;
        }
    }
    state_->installed = true;
    return true;
}
void ServiceTrace::Observe(State& state, Event& event) {
    if (!state.snapshot || !state.sink) return;
    state.observing = true;
    try {
        event.position = state.snapshot();
        // Copy the sink so it may disable its own observation safely.
        const auto sink = state.sink;
        if (sink) sink(event);
    } catch (...) {
        // An observation failure is not a service failure. Preserve the
        // original body's behavior, and expose lost evidence explicitly.
        ++state.failures;
    }
    state.observing = false;
}
void ServiceTrace::Invoke(const std::shared_ptr<State>& state, const StreamLibrary::Body& original,
                          uint32_t address, CpuContext& ctx, uint32_t entry) {
    if (!state->snapshot || !state->sink || state->observing) { original(ctx, entry); return; }
    Event event;
    event.call = ++state->sequence;
    event.parent = state->stack.empty() ? 0 : state->stack.back();
    event.depth = static_cast<uint32_t>(state->stack.size());
    event.address = address; event.entry = entry; event.caller = ctx.cpuRegs.GPR.r[31].UL[0];
    for (size_t i = 0; i < event.arguments.size(); ++i) event.arguments[i] = ctx.cpuRegs.GPR.r[4 + i].UL[0];
    state->stack.push_back(event.call);
    Observe(*state, event);
    try {
        original(ctx, entry);
    } catch (...) {
        state->stack.pop_back();
        throw;
    }
    event.leaving = true;
    event.result = ctx.cpuRegs.GPR.r[2].SD[0];
    Observe(*state, event);
    state->stack.pop_back();
}
std::string ServiceTrace::Format(const Event& e) {
    std::ostringstream out;
    out << (e.leaving ? "leave" : "enter") << " call=" << e.call << " parent=" << e.parent
        << " depth=" << e.depth << " address=0x" << std::hex << e.address
        << " entry=0x" << e.entry << " caller=0x" << e.caller << " args=";
    for (size_t i = 0; i < e.arguments.size(); ++i) out << (i ? "," : "") << e.arguments[i];
    out << std::dec << " result=" << e.result << " presented=" << e.position.presented
        << " domain=" << e.position.domain << " service_us=" << e.position.service_us
        << " service_frame_us=" << e.position.service_frame_us << " replies=" << e.position.replies
        << std::setprecision(17) << " busy_ms=" << e.position.read_busy_ms
        << " end_ms=" << e.position.read_end_ms << " head=" << e.position.read_head;
    return out.str();
}
} // namespace title
