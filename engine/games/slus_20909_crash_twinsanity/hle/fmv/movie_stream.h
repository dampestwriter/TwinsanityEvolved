#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"

namespace title {

// One file cursor shared by MovieDecoder's callbacks and MPEG read-ahead,
// called synchronously by their same game execution owner.
// The base selection replaces three audited file-control calls. Optional
// buffer selection owns Prepare/Open/Close while retaining the game's
// filename and lend/reclaim helpers; neither selection changes its algorithm.
class MovieStream {
public:
    static constexpr uint32_t kPrepare = 0x002c4860;
    static constexpr uint32_t kOpen = 0x002c4890;
    static constexpr uint32_t kClose = 0x002c4900;
    static constexpr uint32_t kSectorBytes = 2048;
    // Stateless backing-file read: absolute byte offset, requested bytes,
    // destination; bytes read or negative error. Preserve the backing file's
    // final-sector padding. This callback must not use a second stream cursor.
    using ReadAt = std::function<int64_t(uint64_t, size_t, uint8_t*)>;
    struct File { uint32_t location = 0, bytes = 0; };
    enum class LookupResult { Found, Missing, Error };
    // Synchronous lookup on the SAME backing file as ReadAt. The provider
    // owns its readiness; a miss/error ends Open without a polling loop.
    using Lookup = std::function<LookupResult(std::string_view, File&)>;
    enum class BufferPart { SoundRing, DiscStream };
    struct BufferLoan {
        // Provider resolves the StreamBuffers token before returning this
        // stable view. No host pointer or offset is published to guest RAM.
        uint32_t handle = 0, bytes = 0;
        uint8_t* data = nullptr;
        std::function<bool()> release;
    };
    struct BufferServices {
        // Bind MusicBuffers::BorrowMovie/Resolve/ReturnMovie as one transaction.
        // Zero lender requests real standalone storage, never a fake address.
        std::function<BufferLoan(CpuContext&, uint32_t lender, BufferPart)> borrow;
        // Current runtime dispatcher, including selected decoder/Graphics.
        // Game LendMusicBuffer/ReclaimMusicBuffers bodies remain unchanged.
        MovieDecoder::Body call;
    };
    struct Counters {
        std::array<uint64_t, 3> original_calls{};
        uint64_t prepared = 0, opened = 0, closed = 0;
        uint64_t seeks = 0, reads = 0, sectors = 0, errors = 0;
        uint64_t sound_loans = 0, disc_loans = 0, returns = 0, buffer_errors = 0;
        std::array<uint64_t, 5> buffer_original_calls{};
        std::array<uint64_t, 4> file_original_calls{};
        uint64_t lookups = 0, missing_files = 0, lookup_errors = 0;
    };
    struct Closed {
        bool owned = false;
        uint32_t gp = 0, location = 0;
        uint64_t position = 0;
    };

    MovieStream(ps2r::ipu::GuestMemory& memory, ReadAt read_at);
    MovieStream(const MovieStream&) = delete;
    MovieStream& operator=(const MovieStream&) = delete;
    // Root selects this only with Movie lifecycle + Stream/Input installed.
    // Capture the actual SDK bindings. Failure rolls back installed wrappers.
    int Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected);
    // Atomic selection of Prepare, OpenStream, CloseStream and both sound
    // cleanup services. Root must not select MusicBuffers before this succeeds.
    // Requires this selected cursor and all decoder software consumers.
    int InstallBuffers(const MovieDecoder::Capture&, const MovieDecoder::Installer&,
                       MovieDecoder&, BufferServices);
    // Install after both buffer consumers. An empty lookup observes actual
    // retained Movie-only Break/Ready/Search/Sync dispatch for the control arm.
    // Unrelated file callers remain in their own service owner.
    int InstallLookup(const MovieDecoder::Capture&, const MovieDecoder::Installer&, Lookup = {});
    // Root must use this same decision for both callback reads and read-ahead.
    // A closed owned session still answers its own reads (error), never reads
    // stale data through a previously retained cursor.
    bool OwnsCursor() const { return owned_; }
    // The selection (hle.movie_start): the Movie owner answers every sceCdSt*
    // cursor intent of the title from here on, whether or not a movie is open
    // now -- the startup contract's "Movie cursor owned" (ServiceStartup).
    bool Selected() const { return installed_ && selected_; }
    bool Active() const { return active_; }
    // Preparation precedes MovieDecoder.Start: the root must include this
    // preflight too, until source Close or restore quiescence ends the session.
    bool CanCheckpoint() const;
    uint32_t Services() const;
    bool CaptureClosed(Closed&, std::string* reason = nullptr) const;
    bool ValidateClosed(const Closed&, std::string* reason = nullptr) const;
    bool RestoreClosed(const Closed&, std::string* reason = nullptr);
    int32_t Seek(uint32_t location);
    int32_t Read(uint8_t* destination, uint32_t sectors);
    // Root quiesces readers first. No movie PCM, decoder, presentation or
    // pacing state is touched; the next source Prepare begins a new session.
    void ResetAfterRestore();
    Counters Counts() const { return counts_; }
    std::string Census() const;

private:
    bool Handle(size_t index, CpuContext&);
    bool BufferBoundary(size_t index, CpuContext&);
    void PrepareBuffers(CpuContext&, uint32_t player);
    void OpenBuffers(CpuContext&, uint32_t player);
    void CloseBuffers(CpuContext&, uint32_t player);
    void FailBuffers(CpuContext&);
    bool Borrow(CpuContext&, BufferPart);
    void ReturnBuffer(BufferPart);
    void ReturnSound(CpuContext&);
    uint32_t Service(CpuContext&, uint32_t address, uint32_t caller,
                     std::initializer_list<uint32_t> args);
    ps2r::ipu::GuestMemory& memory_;
    ReadAt read_at_;
    std::array<MovieDecoder::Body, 3> originals_;
    bool installed_ = false, selected_ = false, owned_ = false, active_ = false, playback_owned_ = false;
    uint32_t gp_ = 0, location_ = 0;
    uint64_t position_ = 0;
    BufferServices buffers_;
    Lookup lookup_;
    std::array<MovieDecoder::Body, 4> file_originals_;
    std::array<MovieDecoder::Body, 5> buffer_originals_;
    BufferLoan sound_loan_, disc_loan_;
    bool buffers_installed_ = false, sound_lent_ = false, disc_lent_ = false;
    bool lookup_installed_ = false;
    bool prepare_failed_ = false;
    uint32_t buffer_player_ = 0, buffer_gp_ = 0;
    Counters counts_;
};

}  // namespace title
