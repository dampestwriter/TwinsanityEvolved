#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"

#include <utility>

#include "engine/ee/cpu/guest_park.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {
constexpr std::array<uint32_t, 5> kBufferEntries{
    0x2ae1c8, 0x2ae860, 0x2af5d0, 0x1e6230, 0x2d46c8};
constexpr uint32_t kLend = 0x1e01b0, kReclaim = 0x1e6230;
constexpr uint32_t kDiscBytes = 0x28040, kSoundBytes = 0x6000;
constexpr uint32_t kGp = 0x3114f0;
uint32_t Reg(const CpuContext& ctx, unsigned r) { return ctx.cpuRegs.GPR.r[r].UL[0]; }
constexpr std::array<uint32_t, 4> kFileEntries{0x2c4610, 0x2c4358, 0x2c3a78, 0x2c3bf0};
bool MovieFileCaller(size_t index, uint32_t caller) {
    switch (index) {
    case 0: return caller == 0x2ae920;
    case 1: return caller == 0x2ae940 || caller == 0x2ae9d0 ||
                   caller == 0x2af5f0 || caller == 0x2af610;
    case 2: return caller == 0x2ae964;
    case 3: return caller == 0x2af668;
    default: return false;
    }
}
}  // namespace

int MovieStream::InstallLookup(const MovieDecoder::Capture& capture,
                               const MovieDecoder::Installer& install, Lookup lookup) {
    if (!installed_ || lookup_installed_ || playback_owned_ || !capture || !install ||
        (lookup && !buffers_installed_)) return 0;
    for (size_t i = 0; i < kFileEntries.size(); ++i) {
        file_originals_[i] = capture(kFileEntries[i]);
        if (!file_originals_[i]) return 0;
    }
    for (size_t i = 0; i < kFileEntries.size(); ++i) {
        if (!install(kFileEntries[i], [this, i, selected = bool(lookup)](CpuContext& c, uint32_t entry) {
                if (MovieFileCaller(i, Reg(c, 31))) {
                    // This generated continuation is incompatible with owned
                    // loans. Do not hide a retained disc fallback or fake its
                    // completion; the whole selected Open/Close owns lookup.
                    if (selected) GuestPark::Forever("movie_owned_file_fallback", entry);
                    ++counts_.file_original_calls[i];
                }
                file_originals_[i](c, entry);
            })) {
            for (size_t j = 0; j < i; ++j) install(kFileEntries[j], file_originals_[j]);
            return 0;
        }
    }
    lookup_ = std::move(lookup);
    lookup_installed_ = true;
    return 4;
}

int MovieStream::InstallBuffers(const MovieDecoder::Capture& capture,
                                const MovieDecoder::Installer& install,
                                MovieDecoder& decoder, BufferServices services) {
    if (!selected_ || buffers_installed_ || playback_owned_ || !capture || !install ||
        !services.borrow || !services.call) return 0;
    for (size_t i = 0; i < kBufferEntries.size(); ++i) {
        buffer_originals_[i] = capture(kBufferEntries[i]);
        if (!buffer_originals_[i]) return 0;
    }
    if (!decoder.SetPlayerBuffers([this] { return buffer_player_ != 0; },
                                 [this](CpuContext& c) { FailBuffers(c); })) return 0;
    buffers_ = std::move(services);
    for (size_t i = 0; i < kBufferEntries.size(); ++i) {
        if (!install(kBufferEntries[i], [this, i](CpuContext& c, uint32_t entry) {
                if ((entry == 0 || entry == kBufferEntries[i]) && BufferBoundary(i, c)) return;
                // Restored continuations cannot consume unpersisted host loans.
                const uint32_t value = Reg(c, 4), caller = Reg(c, 31);
                const bool held = (value && (value == sound_loan_.handle || value == disc_loan_.handle));
                const bool cleanup = caller == 0x2ae704 || caller == 0x2ae714 || caller == 0x2af650;
                if (i < 3 || (buffer_player_ &&
                    (held || cleanup || (entry != 0 && entry != kBufferEntries[i]))))
                    GuestPark::Forever("movie_owned_buffer_boundary", entry);
                ++counts_.buffer_original_calls[i];
                buffer_originals_[i](c, entry);
            })) {
            for (size_t j = 0; j < i; ++j) install(kBufferEntries[j], buffer_originals_[j]);
            decoder.SetPlayerBuffers({}, {});
            buffers_ = {};
            return 0;
        }
    }
    buffers_installed_ = true;
    return 5;
}

uint32_t MovieStream::Service(CpuContext& c, uint32_t address, uint32_t caller,
                              std::initializer_list<uint32_t> args) {
    // Isolate the nested service's ABI scratch from the outer source caller.
    // Guest memory effects (including the game's lending state) stay visible.
    const auto saved = c.cpuRegs.GPR;
    unsigned r = 4;
    for (const auto value : args) c.cpuRegs.GPR.r[r++].UD[0] = value;
    c.cpuRegs.GPR.r[31].UD[0] = caller;
    try {
        if (address == kReclaim) {
            ++counts_.buffer_original_calls[3];
            buffer_originals_[3](c, address); // game decision, not a retired producer
        } else {
            buffers_.call(c, address);
        }
    } catch (...) {
        c.cpuRegs.GPR = saved;
        throw;
    }
    const uint32_t result = Reg(c, 2);
    c.cpuRegs.GPR = saved;
    return result;
}

bool MovieStream::Borrow(CpuContext& c, BufferPart part) {
    const bool sound = part == BufferPart::SoundRing;
    auto& loan = sound ? sound_loan_ : disc_loan_;
    auto& lent = sound ? sound_lent_ : disc_lent_;
    const uint32_t lender = Service(c, kLend, sound ? 0x2ae298 : 0x2ae97c, {});
    lent = lender != 0;
    loan = buffers_.borrow(c, lender, part);
    const uint32_t required = sound ? kSoundBytes : kDiscBytes;
    if (!loan.handle || !loan.data || loan.bytes < required || !loan.release) {
        ++counts_.buffer_errors;
        ReturnBuffer(part);
        // The game has already lent its music player, even if the provider
        // could not resolve the requested range. Undo that source decision.
        if (lent) Service(c, kReclaim, sound ? 0x2ae704 : 0x2af640, {});
        lent = false;
        return false;
    }
    if (sound) ++counts_.sound_loans;
    else ++counts_.disc_loans;
    return true;
}

void MovieStream::ReturnBuffer(BufferPart part) {
    auto& loan = part == BufferPart::SoundRing ? sound_loan_ : disc_loan_;
    if (!loan.release) {
        if (loan.handle || loan.data)
            GuestPark::Forever("movie_buffer_missing_return", 0);
        return;
    }
    if (!loan.release()) {
        ++counts_.buffer_errors;
        GuestPark::Forever("movie_buffer_return_failed", 0);
    }
    loan = {};
    ++counts_.returns;
}

void MovieStream::ReturnSound(CpuContext& c) {
    if (sound_lent_) {
        // Dispatch the unchanged gameplay reclaimer once.
        Service(c, kReclaim, 0x2ae704, {sound_loan_.handle});
    }
    ReturnBuffer(BufferPart::SoundRing);
    sound_lent_ = false;
    memory_.Write32(MovieDecoder::kSoundRing, 0);
    memory_.Write32(MovieDecoder::kSoundRingSize, 0);
}

bool MovieStream::BufferBoundary(size_t index, CpuContext& c) {
    const uint32_t player = Reg(c, 4), gp = Reg(c, 28), caller = Reg(c, 31);
    if (index < 3) {
        // This adapter is for the enumerated NTSC platform Player ABI, whose
        // sound globals are absolute. Do not accept another title's layout.
        if (gp != kGp || !player || uint64_t(player) + 0x4200 > UINT32_MAX ||
            !memory_.Translate(player, 0x4200) || !memory_.Translate(gp - 0x7d44, 0x11b4))
            return false;
        if (index == 0) PrepareBuffers(c, player);
        else if (index == 1) OpenBuffers(c, player);
        else CloseBuffers(c, player);
        return true;
    }
    if (!buffer_player_ || gp != buffer_gp_) return false;
    if (caller != (index == 3 ? 0x2ae704u : 0x2ae714u)) {
        // The disc reclaimer is still a game algorithm, not an IOP consumer.
        if (index == 3) {
            ++counts_.buffer_original_calls[index];
            buffer_originals_[index](c, kReclaim);
            return true;
        }
        return false;
    }
    if (!sound_loan_.handle || player != sound_loan_.handle || sound_lent_ != (index == 3))
        return false;
    if (index == 3) {
        ++counts_.buffer_original_calls[index];
        buffer_originals_[index](c, kReclaim);
    }
    ReturnBuffer(BufferPart::SoundRing);
    sound_lent_ = false;
    if (!disc_loan_.handle) buffer_player_ = buffer_gp_ = 0;
    c.cpuRegs.GPR.r[2].SD[0] = 0;
    return true;
}

void MovieStream::PrepareBuffers(CpuContext& c, uint32_t player) {
    if (buffer_player_ || sound_loan_.release || disc_loan_.release)
        GuestPark::Forever("movie_buffers_prepare_twice", kBufferEntries[0]);
    const uint32_t mode = memory_.Read32(player + 0x34);
    if (mode > 1) GuestPark::Forever("movie_buffers_unknown_audio", kBufferEntries[0]);
    const uint32_t gp = Reg(c, 28);
    if (*memory_.Translate(gp - 0x74c0, 1)) {
        Service(c, MovieDecoder::kInitialise, 0x2ae1f8, {});
        *memory_.Translate(gp - 0x74c0, 1) = 0;
    }
    memory_.Write32(player + 0x1e4, 0);
    // NTSC's graphics handoff precedes SetArena. Keep those service decisions
    // in their owners; TR's portable Prepare omits this title wrapper prefix.
    Service(c, 0x2cf560, 0x2ae20c, {0});
    Service(c, 0x1818b0, 0x2ae214, {});
    Service(c, 0x17dbf8, 0x2ae21c, {});
    Service(c, 0x2c1b50, 0x2ae228, {0, 0});
    *memory_.Translate(gp - 0x7d44, 1) = 1;
    Service(c, 0x17ff50, 0x2ae234, {0x323658});
    Service(c, 0x2b1630, 0x2ae250,
            {memory_.Read32(0x30a760), memory_.Read32(0x309824) + 0x540000});
    buffer_player_ = player;
    buffer_gp_ = gp;
    owned_ = playback_owned_ = true;
    active_ = false;
    gp_ = gp;
    prepare_failed_ = false;
    memory_.Write32(MovieDecoder::kSoundRing, 0);
    memory_.Write32(MovieDecoder::kSoundMode, mode);
    memory_.Write32(MovieDecoder::kSoundRingSize, mode == 1 ? kSoundBytes : 0);
    if (mode == 1) {
        if (Borrow(c, BufferPart::SoundRing)) {
            memory_.Write32(MovieDecoder::kSoundRing, sound_loan_.handle);
            *memory_.Translate(gp - 0x6ba4, 1) = sound_lent_ ? 1 : 0;
        } else {
            prepare_failed_ = true;
            memory_.Write32(MovieDecoder::kSoundRingSize, 0);
        }
    }
    for (const uint32_t word : {0x30a938u, 0x30a948u, 0x30a944u, 0x30a950u})
        memory_.Write32(word, 0);
    c.cpuRegs.GPR.r[2].SD[0] = 1; // NTSC Prepare is void, with this final scratch result.
}

void MovieStream::OpenBuffers(CpuContext& c, uint32_t player) {
    if (buffer_player_ != player || buffer_gp_ != Reg(c, 28) || disc_loan_.release || active_)
        GuestPark::Forever("movie_buffers_open_order", kBufferEntries[1]);
    if (prepare_failed_) { FailBuffers(c); return; }
    const uint32_t stack = Reg(c, 29);
    if (stack < 0x60 || !memory_.Translate(stack - 0x60, 0x60))
        GuestPark::Forever("movie_buffers_open_stack", kBufferEntries[1]);
    // Keep the original filename construction and lookup ABI, including its
    // extension/path strings, uppercase conversion and DString destruction.
    const uint32_t work = stack - 0x60, path = work + 0x30;
    c.cpuRegs.GPR.r[29].UD[0] = work;
    bool constructed = false;
    try {
        Service(c, 0x203dc8, 0x2ae888, {path, 0x30a040});
        constructed = true;
        if (memory_.Read32(player + 0x10))
            Service(c, 0x204060, 0x2ae8a0, {path, memory_.Read32(player + 0xc)});
        Service(c, 0x204060, 0x2ae8b0, {path, 0x30a048});
        if (memory_.Read32(player + 0x24))
            Service(c, 0x204060, 0x2ae8c8, {path, memory_.Read32(player + 0x20)});
        Service(c, 0x204060, 0x2ae8dc, {path, 0x30a050});
        const uint32_t text = memory_.Read32(path), bytes = memory_.Read32(path + 4);
        if (int32_t(bytes) > 0) {
            uint8_t* string = memory_.Translate(text, bytes);
            if (!string) GuestPark::Forever("movie_buffers_filename", kBufferEntries[1]);
            for (uint32_t i = 0; i < bytes; ++i)
                string[i] = uint8_t(Service(c, 0x2c6cb0, 0x2ae8f4, {uint32_t(int32_t(int8_t(string[i])))}));
        }
        bool found = false;
        File file;
        if (lookup_) {
            ++counts_.lookups;
            // DString length excludes its terminator. Reject malformed input
            // instead of truncating it into another asset's name.
            const auto* name = text ? memory_.Translate(text, uint64_t(bytes) + 1) : nullptr;
            LookupResult result = LookupResult::Error;
            try {
                if (name && int32_t(bytes) >= 0 && name[bytes] == 0)
                    result = lookup_(std::string_view(reinterpret_cast<const char*>(name), bytes), file);
                else if (!text && bytes == 0)
                    result = lookup_({}, file);
            } catch (...) {
                ++counts_.lookup_errors;
                throw;
            }
            found = result == LookupResult::Found && file.location != UINT32_MAX;
            if (result == LookupResult::Missing) ++counts_.missing_files;
            else if (!found) ++counts_.lookup_errors;
        } else {
            while (Service(c, 0x2c4610, 0x2ae920, {}) == 0) {}
            while (Service(c, 0x2c4358, 0x2ae940, {1}) != 2) {}
            found = Service(c, 0x2c3a78, 0x2ae964, {work, text ? text : 0x30a020}) != 0;
            if (found) file = {memory_.Read32(work), memory_.Read32(work + 4)};
        }
        if (found) {
            location_ = file.location;
            memory_.Write32(buffer_gp_ - MovieDecoder::kFileSizeGp, file.bytes);
            memory_.Write32(buffer_gp_ - MovieDecoder::kFileLocationGp, location_);
        }
        if (!found || !Borrow(c, BufferPart::DiscStream)) {
            FailBuffers(c);
        } else {
            memory_.Write32(player + 0x41f8, disc_loan_.handle);
            *memory_.Translate(player + 0x41fc, 1) = disc_lent_ ? 1 : 0;
            // The selected file service consumes bytes directly. The disc
            // reservation remains real and leased, but needs no IOP address,
            // alignment arithmetic, stream-bank model or redundant copy.
            ++counts_.prepared;
            if (!lookup_)
                while (Service(c, 0x2c4358, 0x2ae9d0, {1}) != 2) {}
            position_ = uint64_t(location_) * kSectorBytes;
            active_ = true;
            ++counts_.opened;
        }
        Service(c, 0x203ba8, 0x2ae9f4, {path});
        constructed = false;
    } catch (...) {
        if (constructed) Service(c, 0x203ba8, 0x2ae9f4, {path});
        c.cpuRegs.GPR.r[29].UD[0] = stack;
        FailBuffers(c);
        throw;
    }
    c.cpuRegs.GPR.r[29].UD[0] = stack;
}

void MovieStream::CloseBuffers(CpuContext& c, uint32_t player) {
    if (buffer_player_ && buffer_player_ != player)
        GuestPark::Forever("movie_buffers_close_owner", kBufferEntries[2]);
    if (!lookup_)
        while (Service(c, 0x2c4358, 0x2af5f0, {1}) != 2) {}
    active_ = playback_owned_ = false;
    ++counts_.closed;
    if (!lookup_)
        while (Service(c, 0x2c4358, 0x2af610, {1}) != 2) {}
    ReturnBuffer(BufferPart::DiscStream);
    if (disc_lent_) Service(c, kReclaim, 0x2af640, {});
    disc_lent_ = false;
    memory_.Write32(Reg(c, 28) - MovieDecoder::kFileLocationGp, UINT32_MAX);
    memory_.Write32(player + 0x41f8, 0);
    if (!lookup_)
        while (Service(c, 0x2c3bf0, 0x2af668, {1}) != 0) {}
    if (!sound_loan_.handle) buffer_player_ = buffer_gp_ = 0;
}

void MovieStream::FailBuffers(CpuContext& c) {
    if (!buffer_player_) return;
    const uint32_t player = buffer_player_, gp = buffer_gp_;
    ReturnBuffer(BufferPart::DiscStream);
    if (disc_lent_) Service(c, kReclaim, 0x2af640, {});
    disc_lent_ = false;
    ReturnSound(c);
    memory_.Write32(player + 0x41f8, 0);
    memory_.Write32(gp - MovieDecoder::kFileLocationGp, UINT32_MAX);
    buffer_player_ = buffer_gp_ = 0;
    active_ = playback_owned_ = false;
    prepare_failed_ = false;
}
}  // namespace title
