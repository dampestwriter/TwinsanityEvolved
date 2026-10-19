#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "engine/ee/cpu/cpu_state.h"

namespace ps2r::ipu { class GuestMemory; }

namespace title {

// Owns the MPEG movie's software context and picture-delivery boundary.
// File/demux services and the picture upload/draw path remain separate consumers.
class MovieDecoder {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using DemuxService = std::function<int32_t(uint32_t decoder, uint32_t data, int32_t bytes)>;

    struct Hooks {
        // Lifecycle accounting and native decoder reset; neither hook may
        // dispatch the generated SDK body.
        std::function<void(CpuContext&)> create;
        std::function<void(CpuContext&)> reset;
        // Existing native decode, output conversion, audio and pacing service.
        // RGB16 writes the final picture plane, without an IPU CSC operation.
        std::function<void(CpuContext&)> picture_rgb32;
        std::function<void(CpuContext&)> picture_rgb16;
        // Software-only playback lifetime services. Their PCM and pacing
        // policy belongs to the provider, not to the guest transport.
        std::function<void()> initialize;
        std::function<void(uint32_t)> destroy;
        // Invoke retained seek, demux, stream registration and upload builders
        // on the calling guest thread. No transport setup is dispatched here.
        Body call;
        // Accepted input or queued pictures still belong to this playback.
        // File EOF alone is not the end of a movie.
        std::function<bool(uint32_t)> has_pending{};
        // Set both movie input gains, in the source's 0..32767 units. Audio
        // owns the mixer routing; this adapter neither submits RPC nor feeds
        // PCM a second time. Required only by InstallAudio.
        std::function<void(uint16_t)> volume{};
    };

    struct StreamServices {
        // Both operations MUST use the same cursor as the MPEG picture
        // provider's read-ahead. Seek returns the existing service result.
        std::function<int32_t(uint32_t location)> seek;
        // Whole 2048-byte sectors, including the existing final-sector padding.
        // Zero means no data; negative means a terminal file-service error.
        // Retryable device conditions must be settled by the file provider.
        // InstallInput treats zero as exhausted synchronous file input.
        std::function<int32_t(uint8_t* destination, uint32_t sectors)> read;
    };
    struct PictureServices {
        // Optional RGB32 picture owner, selected before a playback begins.
        // Begin must accept the complete image or fail Start atomically.
        std::function<bool(uint32_t decoder, uint32_t width, uint32_t height)> begin;
        // Same v0 contract and clock as picture_rgb32, but keeps pixels owned
        // by the consumer. Never writes a guest plane or builds an upload.
        std::function<void(CpuContext&)> serve;
        std::function<void()> close;
    };

    static constexpr uint32_t kCreate = 0x002b70a8u;
    static constexpr uint32_t kReset = 0x002b7cb0u;
    static constexpr uint32_t kDecodePicture = 0x002b01e8u;
    static constexpr uint32_t kInitialise = 0x002b0d30u;
    static constexpr uint32_t kStart = 0x002afc00u;
    static constexpr uint32_t kFinished = 0x002b0f68u;
    static constexpr uint32_t kClose = 0x002b1000u;
    static constexpr uint32_t kDecoderPointer = 0x0030a700u;
    static constexpr uint32_t kArenaNext = 0x0030a704u;
    static constexpr uint32_t kArenaEnd = 0x0030a708u;
    // Movie player's software phase: Prepare clears this; StartSound sets it
    // after priming and programming the movie volume. No SPU register read.
    static constexpr uint32_t kSoundStarted = 0x0030a938u;
    static constexpr uint32_t kSoundMode = 0x0030a934u;
    static constexpr uint32_t kSetVolume = 0x002af458u;
    static constexpr uint32_t kFeedAudio = 0x002b0ef8u;
    static constexpr uint32_t kPlayerFeedSound = 0x002ae018u;
    static constexpr uint32_t kSeekStream = 0x002af688u;
    static constexpr uint32_t kReadStream = 0x002af6b0u;
    static constexpr uint32_t kDemultiplex = 0x002b0ae0u;
    static constexpr uint32_t kSoundRemote = 0x002c4cf0u;
    static constexpr uint32_t kSoundRing = 0x0030a93cu;
    static constexpr uint32_t kSoundRingSize = 0x0030a940u;
    // NTSC small-data words, relative to the callback's saved gp.
    static constexpr uint32_t kFileLocationGp = 0x74ccu;
    static constexpr uint32_t kFileSizeGp = 0x74c8u;
    static constexpr uint32_t kReadPositionGp = 0x74c4u;

    MovieDecoder(ps2r::ipu::GuestMemory& memory, Hooks hooks);
    // Retain the previous bindings for other formats and restored continuations.
    // The picture-only host installs three entries. Lifecycle integration
    // installs seven and requires initialize/destroy/call/has_pending.
    int Install(const Capture& capture, const Installer& installer, bool include_lifecycle = false);
    // Optional two audio service boundaries, after successful lifecycle
    // installation. Player allocation/start/stop and presentation remain
    // separate: this does not install a hook on the gameplay state machine.
    int InstallAudio(const Capture& capture, const Installer& installer);
    // The player's two audited ring start/stop calls. Selected FeedAudio has
    // already delivered PCM directly; the ring never carries it. Preserve
    // source volume/Close ordering without starting an unused IOP block model.
    // All other SDK callers/commands retain their captured service.
    int InstallAudioPlayback(const Capture& capture, const Installer& installer, bool selected);
    // Two file callbacks only. File open/close, the music-buffer loan and ring
    // allocation remain at their separate service boundaries. No new cursor,
    // file handle, decoder EOF or PCM lifetime is introduced by this adapter.
    int InstallStream(const Capture& capture, const Installer& installer, StreamServices services);
    // Retire the player's refill/demux body and generated callback registration.
    // Requires the synchronous shared-cursor stream selection above.
    int InstallInput(const Capture& capture, const Installer& installer, DemuxService demux);
    // Optional player buffer owner. Install only after all software consumers
    // are selected; while active, no fallback may interpret an opaque loan as
    // an IOP pointer. A failed decoder Start returns both preflight loans.
    bool SetPlayerBuffers(std::function<bool()> active,
                          std::function<void(CpuContext&)> failed_start);
    bool SetPictureServices(PictureServices services);
    void Create(CpuContext& context);
    void Reset(CpuContext& context);
    void DecodePicture(CpuContext& context);
    void Initialise(CpuContext& context);
    void Start(CpuContext& context);
    void Finished(CpuContext& context);
    void Close(CpuContext& context);
    void FeedAudio(CpuContext& context);
    void SetVolume(CpuContext& context);
    void SeekStream(CpuContext& context);
    void ReadStream(CpuContext& context);
    void Demultiplex(CpuContext& context);
    // The player still owns buffers after decoder EOF, until Close. Combine
    // this with the provider's active-input check at root save quiescence.
    bool CanCheckpoint(std::string* reason = nullptr) const;
    // Only closed ownership is persisted. These bits identify the installed
    // service composition, not counters or a new playback configuration.
    uint32_t Services() const;
    uint32_t StoppedDecoder() const { return stopped_decoder_; }
    bool ValidateClosed(uint32_t stopped, std::string* reason = nullptr) const;
    // Apply after incoming RAM and before restoring the provider's closed
    // clocks. No Initialize/Close callback, PCM flush or RAM write here.
    bool RestoreClosed(uint32_t stopped, std::string* reason = nullptr);
    // After restoring guest memory from a non-movie checkpoint, discard the
    // unpersisted software session and clock. Ordinary Close must not do this.
    // No guest words are changed; the next Start/Create owns fresh playback.
    void ResetAfterRestore();
    std::string Census() const;
    // A5 skip: an owned playback is in progress (Start accepted, not Closed).
    bool Playing() const { return playback_owned_; }
    // The owned Movie stop: the next Finished poll of the owned playback
    // answers 1 (the title's own Step sees the movie end and runs its Stop /
    // idle transition -- no hook on the state machine). Cleared by Close/Start.
    void RequestSkip();

private:
    bool Owns(uint32_t decoder);
    void ResetSoftware(CpuContext& context);
    void Fallback(const Body& body, CpuContext& context, uint32_t address);
    uint32_t Allocate(uint64_t size);
    bool Configure(uint32_t decoder, const std::array<uint32_t, 12>& args);
    bool StartPlayback(CpuContext& context, const std::array<uint32_t, 12>& args);
    bool AllocatePictures(CpuContext& context, uint32_t decoder);
    void Release(uint32_t decoder);
    bool OwnsStream(CpuContext& context);
    bool AudioPlayback(CpuContext& context);

    ps2r::ipu::GuestMemory& memory_;
    Hooks hooks_;
    Body create_original_;
    Body reset_original_;
    Body picture_original_;
    Body initialize_original_;
    Body start_original_;
    Body finished_original_;
    Body close_original_;
    Body feed_audio_original_;
    Body volume_original_;
    Body seek_original_;
    Body read_original_;
    Body demux_original_;
    StreamServices stream_;
    DemuxService demux_;
    PictureServices picture_services_;
    uint32_t picture_decoder_ = 0;
    bool stream_installed_ = false;
    bool input_installed_ = false;
    bool lifecycle_installed_ = false;
    bool audio_installed_ = false;
    bool audio_playback_installed_ = false;
    bool audio_playback_selected_ = false;
    std::function<bool()> player_buffers_active_;
    std::function<void(CpuContext&)> player_buffers_failed_;
    bool audio_output_owned_ = false;
    // Close/failure cannot serve again through stale guest descriptor words.
    // Successful Create/Reset/Start begin a new playback on the same object.
    uint32_t stopped_decoder_ = 0;
    uint32_t lifecycle_decoder_ = 0;
    bool playback_owned_ = false;
    uint64_t creates_ = 0;
    uint64_t resets_ = 0;
    uint64_t pictures_ = 0;
    uint64_t rgb16_ = 0;
    uint64_t no_picture_ = 0;
    uint64_t invalid_work_ = 0;
    uint64_t fallbacks_ = 0;
    uint64_t continuations_ = 0;
    uint64_t initialized_ = 0;
    uint64_t started_ = 0;
    uint64_t closed_ = 0;
    uint64_t start_failed_ = 0;
    uint64_t eof_deferred_ = 0;
    uint64_t invalid_picture_ = 0;
    uint64_t priming_eof_ = 0;
    uint64_t decode_errors_ = 0;
    uint64_t audio_feeds_ = 0;
    uint64_t volume_changes_ = 0;
    uint64_t stream_seeks_ = 0;
    uint64_t stream_reads_ = 0;
    uint64_t stream_bytes_ = 0;
    uint64_t stream_eof_ = 0;
    uint64_t stream_errors_ = 0;
    uint64_t restore_resets_ = 0;
    uint64_t demux_windows_ = 0;
    bool skip_requested_ = false;
    uint64_t skips_ = 0;
    uint64_t demux_bytes_ = 0;
    uint64_t input_errors_ = 0;
    uint64_t registrations_retired_ = 0;
    uint64_t audio_original_calls_ = 0;
    uint64_t stream_original_calls_ = 0;
    uint64_t input_original_calls_ = 0;
    uint64_t audio_playback_original_calls_ = 0;
    uint64_t audio_started_ = 0, audio_stopped_ = 0;
};

}  // namespace title
