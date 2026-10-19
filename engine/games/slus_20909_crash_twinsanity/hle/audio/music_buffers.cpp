#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"

#include <array>
#include <stdexcept>

#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"

namespace title {
namespace {
constexpr uint32_t kAllocate = 0x1e1ca8, kResize = 0x1e1e10, kClose = 0x1e6ea8,
    kQuery = 0x1e77b8, kStatus = 0x1e3308, kReply = 0x1e1f48;
constexpr std::array<uint32_t, 6> kEntries{kAllocate, kResize, kClose, kQuery, kStatus, kReply};
std::array<StreamLibrary::Body, kEntries.size()> g_originals;
std::array<bool, StreamBuffers::kStreams> g_selected{};
StreamBuffers* g_buffers = nullptr;
bool g_installed = false;
uint32_t g_free_word = 0, g_free_value = 0;
MusicBuffers::Counters g_count;

uint32_t Arg(const CpuContext& ctx, unsigned reg) { return ctx.cpuRegs.GPR.r[reg].UL[0]; }
void Result(CpuContext& ctx, int32_t value) { ctx.cpuRegs.GPR.r[2].SD[0] = value; }
StreamLibrary::Body& Original(uint32_t address) {
    for (size_t i = 0; i < kEntries.size(); ++i)
        if (kEntries[i] == address) return g_originals[i];
    throw std::logic_error("unknown music buffer service entry");
}
void Publish() {
    if (!g_buffers) return;
    for (uint32_t stream = 0; stream < StreamBuffers::kStreams; ++stream) {
        if (!g_selected[stream]) continue;
        const auto slot = g_buffers->Inspect(stream);
        memory::write<uint32_t>(0x3b3fe0 + 4 * stream, slot.handle);
        // This is source metadata used to associate the paired audio window,
        // not an allocation or mirror of SPU memory.
        memory::write<uint32_t>(0x3b3e80 + 4 * stream, slot.sound_address);
    }
    if (g_free_word) memory::write<uint32_t>(g_free_word, g_free_value);
}
void Apply(const BufferRequest& request) {
    switch (request.kind) {
    case BufferRequest::Kind::Allocate:
        if (!g_buffers->Allocate(request.stream, request.size, request.sound_address)) ++g_count.errors;
        ++g_count.allocations;
        break;
    case BufferRequest::Kind::Resize:
        if (!g_buffers->Resize(request.stream, request.size)) ++g_count.errors;
        ++g_count.resizes;
        break;
    case BufferRequest::Kind::Close:
        // Cancel delivery/paired voices before storage changes owners.
        MusicLibrary::CloseBuffer(request.stream);
        if (!g_buffers->Close(request.stream)) ++g_count.errors;
        ++g_count.closes;
        break;
    case BufferRequest::Kind::Query:
        g_free_word = request.free_word;
        g_free_value = g_buffers->Available();
        ++g_count.queries;
        break;
    }
    Publish();
}
void Sizes(uint32_t stream, uint32_t size) {
    for (const auto base : {0x2e7740u, 0x2e7800u, 0x2e78c0u})
        memory::write<uint32_t>(base + 4 * stream, size);
    memory::write<uint8_t>(0x2e7650 + stream, 0);
}
void Enter(uint32_t address, CpuContext& ctx, uint32_t entry) {
    auto& original = Original(address);
    auto fallback = [&] { ++g_count.fallback; original(ctx, entry); };
    if (!g_buffers || (entry != 0 && entry != address)) { fallback(); return; }
    const auto a = Arg(ctx, 4), b = Arg(ctx, 5), c = Arg(ctx, 6);
    if (address == kReply) { original(ctx, entry); Publish(); return; }
    if (address == kStatus) {
        Publish();
        original(ctx, entry); // source pure client view preserves full ABI
        ++g_count.statuses;
        return;
    }
    BufferRequest request;
    uint32_t words = 0;
    if (address == kQuery) {
        request.kind = BufferRequest::Kind::Query;
        request.free_word = Arg(ctx, 28) - 0x7954;
        words = 2;
    } else {
        if (a >= StreamBuffers::kStreams) { ++g_count.errors; Result(ctx, -1); return; }
        request.stream = a;
        if (address == kAllocate) {
            // Data buffers belong to the file retirement slice. Never pass
            // a previously selected host handle to that retained producer.
            if (b == 1) {
                if (g_selected[a]) { ++g_count.errors; Result(ctx, -1); }
                else fallback();
                return;
            }
            request.kind = BufferRequest::Kind::Allocate;
            request.size = c; request.sound_address = b; words = 7;
        } else {
            if (!g_selected[a]) { fallback(); return; }
            if (address == kClose) { request.kind = BufferRequest::Kind::Close; words = 3; }
            else {
                if (static_cast<int32_t>(a) >= memory::read<int32_t>(Arg(ctx, 28) - 0x794c)) {
                    Result(ctx, -1); return;
                }
                if (b != 0 && b < 1024) { Result(ctx, -2); return; }
                const auto aligned = (uint64_t{b} + 63) & ~uint64_t{63};
                if (!memory::read<uint32_t>(0x2e7740 + 4 * a) ||
                    aligned > memory::read<uint32_t>(0x2e7800 + 4 * a)) {
                    Result(ctx, -1); return;
                }
                request.kind = BufferRequest::Kind::Resize;
                request.size = static_cast<uint32_t>(aligned); words = 5;
            }
        }
    }
    if (!SoundControls::StageBuffer(ctx, request, words)) {
        ++g_count.errors; Result(ctx, -1); return;
    }
    if (request.kind == BufferRequest::Kind::Allocate) { g_selected[a] = true; Sizes(a, c); }
    else if (request.kind == BufferRequest::Kind::Close) Sizes(a, 0);
    else if (request.kind == BufferRequest::Kind::Resize)
        memory::write<uint32_t>(0x2e78c0 + 4 * a, request.size);
    if (address != kQuery) Result(ctx, 0);
}
} // namespace

bool MusicBuffers::Install(const Capture& capture, const Installer& install, StreamBuffers* selected) {
    if (selected && (!SoundControls::Enabled() || !MusicLibrary::Enabled())) return false;
    if (selected == g_buffers && g_installed) return true;
    if ((g_buffers && !g_buffers->Empty()) || (selected && !selected->Empty())) return false;
    if (!g_installed) {
        for (size_t i = 0; i < kEntries.size(); ++i) {
            g_originals[i] = capture(kEntries[i]);
            if (!g_originals[i]) return false;
        }
        for (size_t i = 0; i < kEntries.size(); ++i) {
            const auto address = kEntries[i];
            if (!install(address, [address](CpuContext& ctx, uint32_t entry) { Enter(address, ctx, entry); })) {
                for (size_t j = 0; j < i; ++j) install(kEntries[j], g_originals[j]);
                return false;
            }
        }
        g_installed = true;
    }
    if (!SoundControls::BindBuffers(selected ? Apply : SoundControls::BufferApply{})) return false;
    g_buffers = selected;
    g_selected.fill(false); g_free_word = 0; g_free_value = 0;
    return true;
}
bool MusicBuffers::Enabled() { return g_buffers != nullptr; }
bool MusicBuffers::Reset() {
    if (g_buffers && !g_buffers->Reset()) return false;
    g_selected.fill(false); g_free_word = 0; g_free_value = 0;
    return true;
}
bool MusicBuffers::CanCheckpoint() { return !Enabled(); }
bool MusicBuffers::CanPlay(uint32_t stream) { return !g_buffers || g_buffers->CanPlay(stream); }
uint32_t MusicBuffers::BufferBytes(uint32_t stream) {
    if (!g_buffers || stream >= StreamBuffers::kStreams) return 0;
    const auto slot = g_buffers->Inspect(stream);
    return slot.allocated ? slot.bytes : 0;
}
StreamBuffers::Loan MusicBuffers::BorrowMovie(CpuContext& ctx, uint32_t handle, StreamBuffers::MoviePart part) {
    if (!g_buffers || !SoundControls::Flush(ctx)) { ++g_count.errors; return {}; }
    if (handle) {
        bool found = false;
        for (uint32_t stream = 0; stream < StreamBuffers::kStreams; ++stream) {
            if (g_buffers->Inspect(stream).handle != handle) continue;
            found = true;
            const auto phase = MusicLibrary::Inspect(stream).phase;
            if (phase != MusicLibrary::Phase::Stopped && phase != MusicLibrary::Phase::Failed) {
                ++g_count.errors; return {};
            }
        }
        if (!found) { ++g_count.errors; return {}; }
    }
    const auto loan = g_buffers->BorrowMovie(handle, part);
    if (!loan) ++g_count.errors;
    else if (part == StreamBuffers::MoviePart::SoundRing) ++g_count.sound_loans;
    else ++g_count.disc_loans;
    return loan;
}
StreamBuffers::View MusicBuffers::Resolve(const StreamBuffers::Loan& loan) {
    return g_buffers ? g_buffers->Resolve(loan) : StreamBuffers::View{};
}
bool MusicBuffers::ReturnMovie(const StreamBuffers::Loan& loan) {
    if (!g_buffers || !g_buffers->ReturnMovie(loan)) { ++g_count.errors; return false; }
    ++g_count.returns;
    return true;
}
MusicBuffers::Counters MusicBuffers::Count() { return g_count; }
bool MusicBuffers::CaptureState(State& state, std::string* error) {
    State snapshot;
    if (!g_buffers || !g_buffers->Capture(snapshot.storage, error)) return false;
    snapshot.selected = g_selected;
    snapshot.free_word = g_free_word; snapshot.free_value = g_free_value;
    if (!ValidateState(snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool MusicBuffers::ValidateState(const State& state, std::string* error) {
    if (!g_buffers || !g_buffers->Validate(state.storage, error)) return false;
    if ((state.free_word && (state.free_word < 0x10000 || state.free_word > 0x02000000 - 4 ||
                            state.free_word % 4)) || (!state.free_word && state.free_value) ||
        state.free_value > state.storage.capacity) {
        if (error) *error = "invalid buffer reply metadata";
        return false;
    }
    for (uint32_t stream = 0; stream < StreamBuffers::kStreams; ++stream)
        if (state.storage.slots[stream].allocated && !state.selected[stream]) {
            if (error) *error = "allocated buffer has no service owner";
            return false;
        }
    return true;
}
bool MusicBuffers::RestoreState(const State& state, StreamBuffers::RenewedLoans& loans, std::string* error) {
    if (!ValidateState(state, error) || !g_buffers->Restore(state.storage, loans, error)) return false;
    g_selected = state.selected;
    g_free_word = state.free_word; g_free_value = state.free_value;
    Publish(); // RAM restored first; source status sees the restored owner
    return true;
}
} // namespace title
