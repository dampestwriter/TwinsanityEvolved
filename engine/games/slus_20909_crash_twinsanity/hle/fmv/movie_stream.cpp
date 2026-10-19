#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "engine/ee/cpu/guest_park.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {
constexpr std::array<uint32_t, 3> kEntries{
    MovieStream::kPrepare, MovieStream::kOpen, MovieStream::kClose};
constexpr std::array<uint32_t, 3> kCallers{0x002ae9c4, 0x002ae9ec, 0x002af608};
constexpr uint32_t kReadModeGp = 0x74b8;
uint32_t Arg(const CpuContext& context, size_t index) {
    return context.cpuRegs.GPR.r[4 + index].UL[0];
}
}  // namespace

MovieStream::MovieStream(ps2r::ipu::GuestMemory& memory, ReadAt read_at)
    : memory_(memory), read_at_(std::move(read_at)) {}

int MovieStream::Install(const MovieDecoder::Capture& capture,
                         const MovieDecoder::Installer& install, bool selected) {
    if (installed_ || !capture || !install || (selected && !read_at_)) return 0;
    for (size_t i = 0; i < kEntries.size(); ++i) {
        originals_[i] = capture(kEntries[i]);
        if (!originals_[i]) return 0;
    }
    for (size_t i = 0; i < kEntries.size(); ++i) {
        if (!install(kEntries[i], [this, i, selected](CpuContext& context, uint32_t entry) {
                if (buffer_player_ != 0)
                    GuestPark::Forever("movie_owned_buffer_stream_fallback", entry);
                if (selected && (entry == 0 || entry == kEntries[i]) && Handle(i, context)) return;
                // A foreign control call takes over the shared file service.
                // Subsequent reads and closes must follow that actual owner.
                if (selected) ResetAfterRestore();
                ++counts_.original_calls[i];
                originals_[i](context, entry);
            })) {
            for (size_t j = 0; j < i; ++j) install(kEntries[j], originals_[j]);
            return 0;
        }
    }
    installed_ = true;
    selected_ = selected;
    return 3;
}

bool MovieStream::Handle(size_t index, CpuContext& context) {
    if (context.cpuRegs.GPR.r[31].UL[0] != kCallers[index]) return false;
    const uint32_t gp = context.cpuRegs.GPR.r[28].UL[0];
    if (index == 0) {
        // NTSC OpenStream keeps the public controller in s1. The source has
        // already found the file and chosen/allocated its loaned memory.
        const uint32_t player = context.cpuRegs.GPR.r[17].UL[0];
        if (gp < MovieDecoder::kFileLocationGp ||
            !memory_.Translate(gp - MovieDecoder::kFileLocationGp, 8) ||
            player == 0 || uint64_t(player) + 0x4200 > uint64_t(UINT32_MAX) ||
            !memory_.Translate(player + 0x41f8, 8) ||
            Arg(context, 0) != 0x50 || Arg(context, 1) != 5) return false;
        const uint32_t buffer = memory_.Read32(player + 0x41f8);
        const uint32_t location = memory_.Read32(gp - MovieDecoder::kFileLocationGp);
        if (Arg(context, 2) != ((buffer + 0x3fu) & ~0x3fu) || location == UINT32_MAX) return false;
        gp_ = gp;
        location_ = location;
        position_ = 0;
        owned_ = true;
        active_ = false;
        playback_owned_ = true;
        ++counts_.prepared;
    } else if (index == 1) {
        if (!owned_ || gp != gp_ || Arg(context, 0) != location_ ||
            Arg(context, 1) != gp_ - kReadModeGp ||
            memory_.Read32(gp_ - MovieDecoder::kFileLocationGp) != location_) return false;
        position_ = uint64_t(location_) * kSectorBytes;
        active_ = true;
        playback_owned_ = true;
        ++counts_.opened;
    } else {
        if (!owned_ || gp != gp_) return false;
        active_ = false;
        playback_owned_ = false;
        ++counts_.closed;
    }
    context.cpuRegs.GPR.r[2].SD[0] = 1;
    return true;
}

int32_t MovieStream::Seek(uint32_t location) {
    if (!owned_) { ++counts_.errors; return -1; }
    position_ = uint64_t(location) * kSectorBytes;
    ++counts_.seeks;
    return 1;
}

int32_t MovieStream::Read(uint8_t* destination, uint32_t sectors) {
    ++counts_.reads;
    constexpr uint32_t maximum = uint32_t(std::min<uint64_t>(
        INT32_MAX, std::numeric_limits<size_t>::max() / kSectorBytes));
    if (!owned_ || !active_ || !destination || sectors > maximum) {
        ++counts_.errors;
        return -1;
    }
    if (sectors == 0) return 0;
    const size_t bytes = size_t(sectors) * kSectorBytes;
    const int64_t received = read_at_(position_, bytes, destination);
    if (received < 0 || uint64_t(received) > bytes) {
        ++counts_.errors;
        return -1;
    }
    const uint32_t delivered = uint32_t(uint64_t(received) / kSectorBytes);
    position_ += uint64_t(delivered) * kSectorBytes;
    counts_.sectors += delivered;
    // EOF belongs to the input caller. Never close/flush audio here: read-ahead
    // can exhaust the file while accepted pictures and PCM are still queued.
    return int32_t(delivered);
}

bool MovieStream::CanCheckpoint() const {
    return !playback_owned_ && !active_ && !buffer_player_ && !buffer_gp_ &&
        !sound_loan_.handle && !disc_loan_.handle && !sound_loan_.data && !disc_loan_.data &&
        !sound_loan_.release && !disc_loan_.release &&
        !sound_lent_ && !disc_lent_ && !prepare_failed_;
}

uint32_t MovieStream::Services() const {
    return uint32_t(selected_) | uint32_t(buffers_installed_) << 1 | uint32_t(bool(lookup_)) << 2;
}

bool MovieStream::CaptureClosed(Closed& out, std::string* reason) const {
    if (!CanCheckpoint()) {
        if (reason) *reason = "Movie file or either buffer loan still requires Close";
        return false;
    }
    Closed state{owned_, gp_, location_, position_};
    if (!ValidateClosed(state, reason)) return false;
    out = state;
    return true;
}

bool MovieStream::ValidateClosed(const Closed& state, std::string* reason) const {
    const bool valid = state.owned ?
        selected_ && state.gp >= MovieDecoder::kFileLocationGp &&
        memory_.Translate(state.gp - MovieDecoder::kFileLocationGp, 8) &&
        state.position % kSectorBytes == 0 :
        !state.gp && !state.location && !state.position;
    if (!valid && reason) *reason = "Movie closed cursor is invalid for this file provider";
    return valid;
}

bool MovieStream::RestoreClosed(const Closed& state, std::string* reason) {
    if (!ValidateClosed(state, reason)) return false;
    if (!CanCheckpoint()) {
        if (reason) *reason = "Movie file restore requires both loans returned";
        return false;
    }
    owned_ = state.owned;
    gp_ = state.gp;
    location_ = state.location;
    position_ = state.position;
    return true;
}

void MovieStream::ResetAfterRestore() {
    ReturnBuffer(BufferPart::DiscStream);
    ReturnBuffer(BufferPart::SoundRing);
    buffer_player_ = buffer_gp_ = 0;
    prepare_failed_ = sound_lent_ = disc_lent_ = false;
    owned_ = active_ = playback_owned_ = false;
    gp_ = location_ = 0;
    position_ = 0;
}

std::string MovieStream::Census() const {
    return "prepared=" + std::to_string(counts_.prepared) +
        ",opened=" + std::to_string(counts_.opened) + ",closed=" + std::to_string(counts_.closed) +
        ",seeks=" + std::to_string(counts_.seeks) + ",reads=" + std::to_string(counts_.reads) +
        ",sectors=" + std::to_string(counts_.sectors) + ",errors=" + std::to_string(counts_.errors) +
        ",prepare_original=" + std::to_string(counts_.original_calls[0]) +
        ",open_original=" + std::to_string(counts_.original_calls[1]) +
        ",close_original=" + std::to_string(counts_.original_calls[2]) +
        ",sound_loans=" + std::to_string(counts_.sound_loans) +
        ",disc_loans=" + std::to_string(counts_.disc_loans) +
        ",loan_returns=" + std::to_string(counts_.returns) +
        ",buffer_errors=" + std::to_string(counts_.buffer_errors) +
        ",player_prepare_original=" + std::to_string(counts_.buffer_original_calls[0]) +
        ",player_open_original=" + std::to_string(counts_.buffer_original_calls[1]) +
        ",player_close_original=" + std::to_string(counts_.buffer_original_calls[2]) +
        ",game_reclaim_calls=" + std::to_string(counts_.buffer_original_calls[3]) +
        ",heap_free_original=" + std::to_string(counts_.buffer_original_calls[4]) +
        ",file_break_original=" + std::to_string(counts_.file_original_calls[0]) +
        ",file_ready_original=" + std::to_string(counts_.file_original_calls[1]) +
        ",file_search_original=" + std::to_string(counts_.file_original_calls[2]) +
        ",file_sync_original=" + std::to_string(counts_.file_original_calls[3]) +
        ",lookups=" + std::to_string(counts_.lookups) +
        ",missing_files=" + std::to_string(counts_.missing_files) +
        ",lookup_errors=" + std::to_string(counts_.lookup_errors);
}

}  // namespace title
