#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "engine/core/savestate/quiescence.h"
#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_audio.h"

namespace ps2r::app {
namespace {
using savestate::Blob;
constexpr size_t kHeaderBytes = 48, kContextBytes = 64;
bool Fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}
void Put(std::vector<uint8_t>& out, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
uint64_t Get(const std::vector<uint8_t>& in, size_t& at, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= uint64_t(in[at++]) << (i * 8);
    return value;
}
int64_t Signed(uint64_t value) {
    return value <= uint64_t(INT64_MAX) ? int64_t(value) : -1 - int64_t(~value);
}
bool Mapped(const std::vector<uint8_t>& ram, uint32_t address, size_t bytes) {
    return address && uint64_t(address) + bytes <= ram.size();
}
uint32_t Word(const std::vector<uint8_t>& ram, uint32_t address) {
    size_t at = address;
    return uint32_t(Get(ram, at, 4));
}
}  // namespace

MovieCheckpoint::MovieCheckpoint(::title::MovieDecoder& movie, ::title::MovieStream& stream,
                                  ipu::MpegApi& software, Now now, ::title::MovieAudio* audio)
    : movie_(movie), stream_(stream), software_(software), now_(std::move(now)), audio_(audio) {}

bool MovieCheckpoint::Capture(Snapshot& out, std::string* error) {
    if (!now_) return Fail(error, "Movie checkpoint has no game clock");
    Snapshot state;
    if ((audio_ && !audio_->CanCheckpoint(error)) ||
        !movie_.CanCheckpoint(error) || !stream_.CaptureClosed(state.stream, error) ||
        !software_.CaptureIdle(now_(), state.contexts, error)) return false;
    state.decoder_services = DecoderServices();
    state.pcm_gain = audio_ ? audio_->Gain() : 0;
    state.stream_services = stream_.Services();
    state.stopped_decoder = movie_.StoppedDecoder();
    if (!Validate(state, error) || !movie_.ValidateClosed(state.stopped_decoder, error)) return false;
    out = std::move(state);
    return true;
}

bool MovieCheckpoint::CanCheckpoint(std::string* error) {
    Snapshot state;
    return Capture(state, error);
}

bool MovieCheckpoint::Validate(const Snapshot& state, std::string* error) const {
    if (state.decoder_services != DecoderServices() || state.stream_services != stream_.Services())
        return Fail(error, "Movie checkpoint provider selection differs from this session");
    if (state.pcm_gain > 32767 || (!audio_ && state.pcm_gain))
        return Fail(error, "Movie checkpoint has an invalid or unowned PCM gain");
    return stream_.ValidateClosed(state.stream, error) && software_.ValidateIdle(state.contexts, 0, error);
}

bool MovieCheckpoint::Restore(const Snapshot& state, std::string* error) {
    // Incoming RAM may already have replaced the previous stopped descriptor.
    // Check current ownership directly, not Capture's old identity-vs-RAM test.
    if (!Validate(state, error) || (audio_ && !audio_->CanCheckpoint(error)) || !movie_.CanCheckpoint(error) ||
        !movie_.ValidateClosed(state.stopped_decoder, error)) return false;
    if (!stream_.CanCheckpoint()) return Fail(error, "Movie file restore requires both loans returned");
    // Root preflight owns resource preparation. A component-only direct
    // restore remains supported, but an application never restages on apply.
    if (preparation_required_) {
        std::vector<uint8_t> encoded;
        if (!prepared_ || !Encode(state, encoded, error) || encoded != prepared_record_)
            return Fail(error, "Movie restore requires its exact prepared software state");
    }
    const bool software_ok = preparation_required_ ? software_.PublishIdle(*prepared_, now_(), error) :
                                                    software_.RestoreIdle(state.contexts, now_(), error);
    if (!software_ok ||
        !stream_.RestoreClosed(state.stream, error) ||
        !movie_.RestoreClosed(state.stopped_decoder, error) ||
        (audio_ && !audio_->RestoreClosed(state.pcm_gain, error))) return false;
    ++restored_;
    return true;
}

bool MovieCheckpoint::Encode(const Snapshot& state, std::vector<uint8_t>& out, std::string* error) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    if ((state.decoder_services & ~0xffu) || (state.stream_services & ~7u) ||
        state.pcm_gain > 32767 || (!(state.decoder_services & 128u) && state.pcm_gain) ||
        state.contexts.size() > (savestate::kMaxSectionPayload - kHeaderBytes) / kContextBytes)
        return Fail(error, "Movie checkpoint selection or extent is invalid");
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + kContextBytes * state.contexts.size());
    Put(bytes, kTag, 4); Put(bytes, kVersion, 4);
    Put(bytes, state.decoder_services, 4); Put(bytes, state.stream_services, 4);
    Put(bytes, state.stopped_decoder, 4); Put(bytes, state.stream.owned ? 1 : 0, 4);
    Put(bytes, state.stream.gp, 4); Put(bytes, state.stream.location, 4);
    Put(bytes, state.stream.position, 8); Put(bytes, state.contexts.size(), 4); Put(bytes, state.pcm_gain, 4);
    for (const auto& context : state.contexts) {
        uint64_t rate = 0;
        std::memcpy(&rate, &context.authored_fps, sizeof(rate));
        Put(bytes, context.address, 4); Put(bytes, context.context_address, 4);
        Put(bytes, context.width, 4); Put(bytes, context.height, 4); Put(bytes, rate, 8);
        Put(bytes, context.clock.armed ? 1 : 0, 4); Put(bytes, 0, 4);
        Put(bytes, uint64_t(context.clock.ticks_to_next), 8); Put(bytes, uint64_t(context.clock.period), 8);
        Put(bytes, uint64_t(context.clock.reanchor_periods), 8); Put(bytes, context.clock.reanchors, 8);
    }
    out = std::move(bytes);
    return true;
}

bool MovieCheckpoint::Decode(const Blob& blob, Snapshot& out, std::string* error) {
    const auto& bytes = blob.payload;
    if (blob.tag != kTag || (blob.version != 1 && blob.version != kVersion) ||
        blob.flags != savestate::kSectionRequired ||
        bytes.size() < kHeaderBytes || bytes.size() > savestate::kMaxSectionPayload)
        return Fail(error, "Movie checkpoint envelope or extent is invalid");
    size_t at = 0;
    if (Get(bytes, at, 4) != kTag || Get(bytes, at, 4) != blob.version)
        return Fail(error, "Movie checkpoint payload version is invalid");
    Snapshot state;
    state.decoder_services = uint32_t(Get(bytes, at, 4));
    state.stream_services = uint32_t(Get(bytes, at, 4));
    state.stopped_decoder = uint32_t(Get(bytes, at, 4));
    const auto owned = Get(bytes, at, 4);
    state.stream.owned = owned != 0;
    state.stream.gp = uint32_t(Get(bytes, at, 4)); state.stream.location = uint32_t(Get(bytes, at, 4));
    state.stream.position = Get(bytes, at, 8);
    const auto count = Get(bytes, at, 4), gain = Get(bytes, at, 4);
    if ((blob.version == 1 && ((state.decoder_services & 128u) || gain)) ||
        owned > 1 || gain > 32767 || (!(state.decoder_services & 128u) && gain) ||
        (state.decoder_services & ~0xffu) || (state.stream_services & ~7u) ||
        count * kContextBytes != bytes.size() - kHeaderBytes)
        return Fail(error, "Movie checkpoint header or context count is invalid");
    state.pcm_gain = static_cast<uint16_t>(gain);
    state.contexts.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) {
        ipu::MpegApi::IdleContext context;
        context.address = uint32_t(Get(bytes, at, 4)); context.context_address = uint32_t(Get(bytes, at, 4));
        context.width = uint32_t(Get(bytes, at, 4)); context.height = uint32_t(Get(bytes, at, 4));
        const uint64_t rate = Get(bytes, at, 8);
        std::memcpy(&context.authored_fps, &rate, sizeof(rate));
        const auto armed = Get(bytes, at, 4), pad = Get(bytes, at, 4);
        if (armed > 1 || pad) return Fail(error, "Movie checkpoint clock flags are invalid");
        context.clock.armed = armed != 0;
        context.clock.ticks_to_next = Signed(Get(bytes, at, 8)); context.clock.period = Signed(Get(bytes, at, 8));
        context.clock.reanchor_periods = Signed(Get(bytes, at, 8)); context.clock.reanchors = Get(bytes, at, 8);
        state.contexts.push_back(context);
    }
    out = std::move(state);
    return true;
}

bool MovieCheckpoint::ValidateImage(const std::vector<Blob>& blobs, std::string* error) const {
    const Blob* saved = nullptr;
    const Blob* ram = nullptr;
    for (const auto& blob : blobs) {
        if (blob.tag == kTag) {
            if (saved) return Fail(error, "Movie checkpoint is duplicated");
            saved = &blob;
        }
        if (blob.tag == savestate::TAG_RAM) {
            if (ram) return Fail(error, "Movie incoming RAM is duplicated");
            ram = &blob;
        }
    }
    if (!saved) {
        if (!legacy_) return Fail(error, "Movie owner state is missing; no boot migration was approved");
        return legacy_(blobs, error);
    }
    Snapshot state;
    if (!Decode(*saved, state, error) || !Validate(state, error)) return false;
    if (!ram || !Mapped(ram->payload, ::title::MovieDecoder::kDecoderPointer, 4))
        return Fail(error, "Movie checkpoint needs its incoming RAM owner");
    if ((state.stopped_decoder && !Mapped(ram->payload, state.stopped_decoder, 0x160)) ||
        (((state.decoder_services & 1u) || state.stopped_decoder) &&
         state.stopped_decoder != Word(ram->payload, ::title::MovieDecoder::kDecoderPointer)))
        return Fail(error, "Movie stopped decoder disagrees with incoming RAM");
    if (state.stream.owned && !Mapped(ram->payload, state.stream.gp - ::title::MovieDecoder::kFileLocationGp, 8))
        return Fail(error, "Movie file cursor is outside incoming RAM");
    for (const auto& context : state.contexts)
        if (!Mapped(ram->payload, context.address, 0x160))
            return Fail(error, "Movie software context is outside incoming RAM");
    return true;
}

bool MovieCheckpoint::Register(savestate::SectionRegistry& registry, Legacy approved_legacy) {
    if (!now_ || registry.Has(kTag)) return false;
    legacy_ = std::move(approved_legacy);
    savestate::SectionSpec spec;
    spec.tag = kTag; spec.version = kVersion; spec.owner = "Movie";
    spec.required = false; spec.capture_required = true;
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        Snapshot state;
        std::string error;
        if (!Capture(state, &error) || !Encode(state, bytes, &error)) throw std::runtime_error(error);
    };
    spec.validate = [this](const Blob& blob, std::string* error) {
        Snapshot state;
        return Decode(blob, state, error) && Validate(state, error);
    };
    spec.restore = [this](const Blob& blob, std::string* error) {
        Snapshot state;
        return Decode(blob, state, error) && Restore(state, error);
    };
    spec.absent = [this] {
        // Reached only after the explicitly supplied legacy classifier agrees.
        // OLD files never carried decoder clocks/PCM; preserve that documented
        // import policy, with the migration visible in the retirement census.
        movie_.ResetAfterRestore();
        stream_.ResetAfterRestore();
        if (!(movie_.Services() & 1u)) software_.Init();
        if (audio_) {
            std::string error;
            if (!audio_->RestoreClosed(0, &error)) throw std::runtime_error(error);
        }
        ++migrated_;
    };
    if (!registry.RegisterValidation("Movie", [this](const auto& blobs, std::string* error) {
            return ValidateImage(blobs, error);
        })) return false;
    return registry.Register(spec);
}

void MovieCheckpoint::RegisterConditions(savestate::Quiescence& conditions) {
    conditions.Register("movie_software", [this](std::string* error) { return CanCheckpoint(error); });
}

void MovieCheckpoint::Record(verify::RunMeta& meta) const {
    meta.Set("movie_checkpoint", "restored=" + std::to_string(restored_) +
        ",legacy_migrated=" + std::to_string(migrated_) +
        ",prepared=" + std::to_string(preparations_));
}
}  // namespace ps2r::app
