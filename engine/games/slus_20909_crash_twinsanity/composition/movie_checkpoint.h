#pragma once

#include "engine/core/savestate/section_registry.h"
#include "engine/ipu/hle/mpeg_api.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"

namespace ps2r::savestate { class Quiescence; }
namespace ps2r::verify { class RunMeta; }
namespace title { class MovieAudio; }
class StateRestorer;
namespace ps2r::app {

// Closed Movie ownership, captured on the completed game owner. Active input,
// pictures, PCM and either buffer loan require source Close; EOF is not Close.
// Register after RAM and the logical clock. No renderer or IOP state is encoded.
class MovieCheckpoint {
public:
    static constexpr uint32_t kTag = savestate::Tag('M', 'O', 'V', 'I');
    static constexpr uint32_t kVersion = 2;
    struct Snapshot {
        uint32_t decoder_services = 0, stream_services = 0, stopped_decoder = 0;
        uint16_t pcm_gain = 0;
        ::title::MovieStream::Closed stream;
        std::vector<ipu::MpegApi::IdleContext> contexts;
    };
    using Now = std::function<int64_t()>;
    // Explicit boot-import approval over the complete incoming owned image.
    // Interactive reproduction restore must require MOVI; absence never
    // authorizes this migration by itself.
    using Legacy = savestate::SectionRegistry::ImageValidation;

    MovieCheckpoint(::title::MovieDecoder&, ::title::MovieStream&, ipu::MpegApi&, Now,
                    ::title::MovieAudio* audio = nullptr);
    bool Register(savestate::SectionRegistry&, Legacy approved_legacy = {});
    bool RegisterBoot(savestate::SectionRegistry&, const std::string& frozen_inventory,
                      const std::string& load_file);
    // Register after MOVI and GCLK. All application loads require complete
    // MPEG allocations before any live owner unwinds; apply never restages.
    bool RegisterPreparation(StateRestorer&);
    bool HasPrepared() const { return bool(prepared_); }
    void RegisterConditions(savestate::Quiescence&);
    void Record(verify::RunMeta&) const;
    bool CanCheckpoint(std::string* error = nullptr);
    bool Capture(Snapshot&, std::string* error = nullptr);
    bool Validate(const Snapshot&, std::string* error = nullptr) const;
    bool Restore(const Snapshot&, std::string* error = nullptr);
    bool ValidateImage(const std::vector<savestate::Blob>&, std::string* error = nullptr) const;
    // Validate the original image before another owner replaces its legacy
    // sections. An approved OLD image gains a real closed MOVI record; a new
    // image missing Movie state still rejects. Publishes no live state.
    bool PrepareBoot(const std::vector<savestate::Blob>& original,
                     std::vector<savestate::Blob>& owned, bool& imported,
                     std::string* error = nullptr) const;
    static bool Encode(const Snapshot&, std::vector<uint8_t>&, std::string* error = nullptr);
    static bool Decode(const savestate::Blob&, Snapshot&, std::string* error = nullptr);
    // Bounded boot migration for the title's frozen restore_mhash_expected
    // inventory only. No new owner tags may be present and all three memory
    // payloads must match its approved hash. Never an interactive policy.
    static bool ValidateLegacy(const std::vector<savestate::Blob>&, uint64_t approved_memory_hash,
                               std::string* error = nullptr);
    uint64_t Restored() const { return restored_; }
    uint64_t Migrated() const { return migrated_; }
    bool Applied() const { return restored_ || migrated_; }

private:
    friend class MovieBoot;
    uint32_t DecoderServices() const { return movie_.Services() | (audio_ ? 128u : 0u); }
    bool PrepareImage(const std::vector<savestate::Blob>&, std::string*);
    void DiscardPrepared();
    ::title::MovieDecoder& movie_;
    ::title::MovieStream& stream_;
    ipu::MpegApi& software_;
    Now now_;
    ::title::MovieAudio* audio_ = nullptr;
    Legacy legacy_;
    bool preparation_required_ = false;
    std::unique_ptr<ipu::MpegIdleState> prepared_;
    std::vector<uint8_t> prepared_record_;
    uint64_t restored_ = 0, migrated_ = 0;
    uint64_t preparations_ = 0;
};

}  // namespace ps2r::app
