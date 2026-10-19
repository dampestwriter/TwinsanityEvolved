#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "engine/ee/cpu/guest_park.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {

uint32_t Arg(const CpuContext& context, uint32_t index) {
    return context.cpuRegs.GPR.r[4 + index].UL[0];
}

void Return(CpuContext& context, int32_t value) {
    context.cpuRegs.GPR.r[2].SD[0] = value;
}

void Arguments(CpuContext& context, std::initializer_list<uint32_t> values) {
    uint32_t index = 4;
    for (uint32_t value : values) context.cpuRegs.GPR.r[index++].UD[0] = value;
}

void RestoreCaller(CpuContext& context, const GPRregs& saved) {
    for (uint32_t index : {16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 28u, 29u, 30u, 31u})
        context.cpuRegs.GPR.r[index] = saved.r[index];
}

MovieDecoder::Body CountOriginal(MovieDecoder::Body body, uint64_t* calls) {
    if (!body) return {};
    return [body = std::move(body), calls](CpuContext& context, uint32_t entry) {
        ++*calls; // Actual captured-body invocation, including continuations.
        body(context, entry);
    };
}

}  // namespace

MovieDecoder::MovieDecoder(ps2r::ipu::GuestMemory& memory, Hooks hooks)
    : memory_(memory), hooks_(std::move(hooks)) {}

bool MovieDecoder::SetPictureServices(PictureServices services) {
    if (!lifecycle_installed_ || playback_owned_) return false;
    const bool any = services.begin || services.serve || services.close;
    if (any && (!services.begin || !services.serve || !services.close)) return false;
    picture_services_ = std::move(services);
    return true;
}

int MovieDecoder::Install(const Capture& capture, const Installer& installer, bool include_lifecycle) {
    if (!capture || !installer) return 0;
    create_original_ = capture(kCreate);
    reset_original_ = capture(kReset);
    picture_original_ = capture(kDecodePicture);
    if (!create_original_ || !reset_original_ || !picture_original_ ||
        !hooks_.create || !hooks_.reset || !hooks_.picture_rgb32 || !hooks_.picture_rgb16) return 0;
    if (include_lifecycle) {
        initialize_original_ = capture(kInitialise);
        start_original_ = capture(kStart);
        finished_original_ = capture(kFinished);
        close_original_ = capture(kClose);
        if (!initialize_original_ || !start_original_ || !finished_original_ || !close_original_ ||
            !hooks_.initialize || !hooks_.destroy || !hooks_.call || !hooks_.has_pending) return 0;
    }
    const auto bind = [this, &installer](uint32_t address, Body original,
                                         void (MovieDecoder::*method)(CpuContext&)) {
        return installer(address, [this, address, original, method](CpuContext& context, uint32_t entry) {
            if (entry != 0 && entry != address) {
                if (player_buffers_active_ && player_buffers_active_())
                    GuestPark::Forever("movie_owned_buffer_continuation", entry);
                ++continuations_;
                original(context, entry);
                return;
            }
            (this->*method)(context);
        });
    };
    int installed = bind(kCreate, create_original_, &MovieDecoder::Create) ? 1 : 0;
    installed += bind(kReset, reset_original_, &MovieDecoder::Reset) ? 1 : 0;
    installed += bind(kDecodePicture, picture_original_, &MovieDecoder::DecodePicture) ? 1 : 0;
    if (include_lifecycle) {
        installed += bind(kInitialise, initialize_original_, &MovieDecoder::Initialise) ? 1 : 0;
        installed += bind(kStart, start_original_, &MovieDecoder::Start) ? 1 : 0;
        installed += bind(kFinished, finished_original_, &MovieDecoder::Finished) ? 1 : 0;
        installed += bind(kClose, close_original_, &MovieDecoder::Close) ? 1 : 0;
    }
    lifecycle_installed_ = include_lifecycle && installed == 7;
    return installed;
}

int MovieDecoder::InstallAudio(const Capture& capture, const Installer& installer) {
    if (!lifecycle_installed_ || !capture || !installer || !hooks_.volume) return 0;
    feed_audio_original_ = CountOriginal(capture(kFeedAudio), &audio_original_calls_);
    volume_original_ = CountOriginal(capture(kSetVolume), &audio_original_calls_);
    if (!feed_audio_original_ || !volume_original_) return 0;
    const auto bind = [this, &installer](uint32_t address, Body original,
                                        void (MovieDecoder::*method)(CpuContext&)) {
        return installer(address, [this, address, original, method](CpuContext& context, uint32_t entry) {
            if (entry != 0 && entry != address) {
                if (player_buffers_active_ && player_buffers_active_())
                    GuestPark::Forever("movie_owned_buffer_continuation", entry);
                ++continuations_;
                original(context, entry);
                return;
            }
            (this->*method)(context);
        });
    };
    int installed = bind(kFeedAudio, feed_audio_original_, &MovieDecoder::FeedAudio) ? 1 : 0;
    installed += bind(kSetVolume, volume_original_, &MovieDecoder::SetVolume) ? 1 : 0;
    audio_installed_ = installed == 2;
    return installed;
}

int MovieDecoder::InstallAudioPlayback(const Capture& capture, const Installer& installer, bool selected) {
    if (!lifecycle_installed_ || !audio_installed_ || audio_playback_installed_ ||
        playback_owned_ || !capture || !installer) return 0;
    const auto body = capture(kSoundRemote);
    if (!body) return 0;
    const auto tracked = CountOriginal(body, &audio_playback_original_calls_);
    if (!installer(kSoundRemote, [this, selected, tracked](CpuContext& context, uint32_t entry) {
            if (selected && (entry == 0 || entry == kSoundRemote) && AudioPlayback(context)) return;
            if (player_buffers_active_ && player_buffers_active_() &&
                Arg(context, 1) == 0x80e0 && Arg(context, 2) == 0)
                GuestPark::Forever("movie_owned_buffer_audio_fallback", entry);
            // A foreign transfer may take over the same output after the
            // movie's acknowledged start. Its later stop must reach its real
            // consumer even when it arrives at the audited movie call site.
            if ((entry == 0 || entry == kSoundRemote) &&
                Arg(context, 1) == 0x80e0 && Arg(context, 2) == 0)
                audio_output_owned_ = false;
            tracked(context, entry);
        })) return 0;
    audio_playback_installed_ = true;
    audio_playback_selected_ = selected;
    return 1;
}

bool MovieDecoder::AudioPlayback(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    // A source abort can close the player before StartSound or after failed
    // priming. Owned loans never armed an IOP transfer in either case.
    if (player_buffers_active_ && player_buffers_active_() && !audio_output_owned_ &&
        context.cpuRegs.GPR.r[31].UL[0] == 0x2ae6e4 &&
        Arg(context, 0) == 1 && Arg(context, 1) == 0x80e0 && Arg(context, 2) == 0 &&
        Arg(context, 3) == 2 && Arg(context, 4) == 0 && Arg(context, 5) == 0) {
        ++audio_stopped_;
        Return(context, 0);
        return true;
    }
    // These two source calls are service boundaries inside Start/Stop, not
    // replacements of their gameplay algorithms. Requiring both ABI and caller
    // prevents a bank, another format, or a foreign SDK user joining this path.
    if (!playback_owned_ || !Owns(decoder) || memory_.Read32(kSoundMode) != 1 ||
        memory_.Read32(decoder + 0x14c) != 1 || memory_.Read32(decoder + 0x110) != 0 ||
        Arg(context, 0) != 1 || Arg(context, 1) != 0x80e0 || Arg(context, 2) != 0) return false;
    const uint32_t caller = context.cpuRegs.GPR.r[31].UL[0];
    if (caller == 0x002ae620 && Arg(context, 3) == 0x13) {
        const uint32_t ring = memory_.Read32(kSoundRing);
        if (ring == 0 || memory_.Read32(kSoundRingSize) != 0x6000 ||
            Arg(context, 4) != ring || Arg(context, 5) != 0x6000 || Arg(context, 6) != ring)
            return false;
        // PCM's first-picture gate and authored clock already own delivery.
        // The caller programs volume immediately after this acknowledgement.
        // Do not open another FIFO, arm a timer or reset that delivery clock.
        audio_output_owned_ = true;
        ++audio_started_;
    } else if (caller == 0x002ae6e4 && Arg(context, 3) == 2 &&
               Arg(context, 4) == 0 && Arg(context, 5) == 0 && audio_output_owned_) {
        // The source muted before this call; decoder Close then cuts its PCM.
        // There is no ring consumer to stop and no extra flush/drain to invent.
        audio_output_owned_ = false;
        ++audio_stopped_;
    } else {
        return false;
    }
    Return(context, 0);
    return true;
}

int MovieDecoder::InstallStream(const Capture& capture, const Installer& installer, StreamServices services) {
    if (!lifecycle_installed_ || stream_installed_ || !capture || !installer ||
        !services.seek || !services.read) return 0;
    const std::array<uint32_t, 2> addresses{kSeekStream, kReadStream};
    const std::array<Body, 2> originals{capture(kSeekStream), capture(kReadStream)};
    if (!originals[0] || !originals[1]) return 0;
    const std::array<Body, 2> tracked{
        CountOriginal(originals[0], &stream_original_calls_),
        CountOriginal(originals[1], &stream_original_calls_)};
    for (size_t i = 0; i < addresses.size(); ++i) {
        const auto address = addresses[i];
        const auto original = tracked[i];
        if (!installer(address, [this, address, original](CpuContext& context, uint32_t entry) {
            if (entry != 0 && entry != address) {
                if (player_buffers_active_ && player_buffers_active_())
                    GuestPark::Forever("movie_owned_buffer_continuation", entry);
                ++continuations_;
                original(context, entry);
                return;
            }
            if (address == kSeekStream) SeekStream(context); else ReadStream(context);
        })) {
            for (size_t j = 0; j < i; ++j) installer(addresses[j], originals[j]);
            return 0;
        }
    }
    seek_original_ = tracked[0];
    read_original_ = tracked[1];
    stream_ = std::move(services);
    stream_installed_ = true;
    return 2;
}

int MovieDecoder::InstallInput(const Capture& capture, const Installer& installer, DemuxService demux) {
    if (!stream_installed_ || input_installed_ || !capture || !installer || !demux) return 0;
    const Body original = CountOriginal(capture(kDemultiplex), &input_original_calls_);
    if (!original) return 0;
    if (!installer(kDemultiplex, [this, original](CpuContext& context, uint32_t entry) {
        if (entry != 0 && entry != kDemultiplex) {
            if (player_buffers_active_ && player_buffers_active_())
                GuestPark::Forever("movie_owned_buffer_continuation", entry);
            ++continuations_;
            original(context, entry);
            return;
        }
        Demultiplex(context);
    })) return 0;
    demux_original_ = original;
    demux_ = std::move(demux);
    input_installed_ = true;
    return 1;
}

bool MovieDecoder::Owns(uint32_t decoder) {
    return decoder != 0 && memory_.Read32(kDecoderPointer) == decoder &&
           memory_.Translate(decoder, 0x160) != nullptr && memory_.Read32(decoder + 0x148) == 2;
}

void MovieDecoder::Fallback(const Body& body, CpuContext& context, uint32_t address) {
    if (player_buffers_active_ && player_buffers_active_())
        GuestPark::Forever("movie_owned_buffer_fallback", address);
    ++fallbacks_;
    if (!body) GuestPark::Forever("movie_missing_fallback", address);
    body(context, address);
}

bool MovieDecoder::SetPlayerBuffers(std::function<bool()> active,
                                    std::function<void(CpuContext&)> failed_start) {
    if (!lifecycle_installed_ || !audio_installed_ || !audio_playback_selected_ ||
        !stream_installed_ || !input_installed_ || playback_owned_ ||
        (player_buffers_active_ && player_buffers_active_()) ||
        bool(active) != bool(failed_start)) return false;
    player_buffers_active_ = std::move(active);
    player_buffers_failed_ = std::move(failed_start);
    return true;
}

void MovieDecoder::Create(CpuContext& context) {
    const uint32_t decoder = Arg(context, 0);
    if (!Owns(decoder)) {
        Fallback(create_original_, context, kCreate);
        return;
    }
    ++creates_;
    if (hooks_.create) hooks_.create(context);
    const uint32_t work = Arg(context, 1);
    const uint32_t size = Arg(context, 2);
    const uint64_t aligned = (uint64_t{work} + 3u) & ~uint64_t{3};
    const uint64_t table = (aligned + 0x19a0u + 7u) & ~uint64_t{7};
    const uint64_t end = uint64_t{work} + size;
    // The generated stream-callback registration still consumes this software
    // table. Validate it before publishing the new context or touching memory.
    uint8_t* bytes = nullptr;
    if (work == 0 || end > 0x100000000ull ||
        table + 0x800u > end || !(bytes = memory_.Translate(work, size))) {
        ++invalid_work_;
        Return(context, 0);
        return;
    }
    std::memset(bytes, 0, size);
    const uint32_t sys = static_cast<uint32_t>(aligned);
    const auto put = [this, sys](uint32_t offset, uint32_t value) { memory_.Write32(sys + offset, value); };
    memory_.Write32(decoder + 0x40, sys);
    for (uint32_t offset : {0u, 4u, 8u, 0x20u, 0x24u, 0x38u, 0x3cu}) memory_.Write32(decoder + offset, 0);
    for (uint32_t offset : {0x10u, 0x18u, 0x28u, 0x30u}) {
        memory_.Write32(decoder + offset, 0xffffffffu);
        memory_.Write32(decoder + offset + 4, 0xffffffffu);
    }
    // TR: movie/mpeg.h MpegSystem, checked against NTSC sceMpegCreate.
    // Only software descriptors survive: their alignment is four bytes, and
    // the old hardware-buffer alignment check no longer applies. There are no Stop/RestartDMA
    // callbacks, scratchpad macroblock buffers or IPU register writes.
    put(0x60, static_cast<uint32_t>(table));
    for (uint32_t offset : {0xa8u, 0xacu, 0xb0u, 0x100u, 0x104u, 0x860u}) put(offset, 0xffffffffu);
    put(0xc4, 1);
    put(0x118, sys + 0x19a0u);
    put(0x11c, static_cast<uint32_t>(end - (aligned + 0x19a0u)));
    put(0x120, static_cast<uint32_t>(table + 0x800u));
    put(0x124, static_cast<uint32_t>(table + 0x800u));
    put(0x868, decoder);
    // Reference descriptor links are still read by the software ClearRefBuff
    // ABI. These do not allocate or decode hardware reference pictures.
    for (uint32_t group = 0; group < 3; ++group) {
        const uint32_t slots = 0x1c8u + group * 16u;
        for (uint32_t image = 0; image < 3; ++image)
            put(slots + (image == 2 ? 12u : image * 4u),
                sys + 0x1f8u + (group * 3u + image) * 0x68u);
    }
    // Start creates twice around the initial demux. Keep the existing native
    // playback reset at both calls, including its buffered-input policy.
    ResetSoftware(context);
    stopped_decoder_ = 0;
    Return(context, 1);
}

void MovieDecoder::ResetSoftware(CpuContext& context) {
    ++resets_;
    if (hooks_.reset) hooks_.reset(context);
    const uint32_t decoder = Arg(context, 0);
    const uint32_t sys = memory_.Read32(decoder + 0x40);
    for (uint32_t offset : {0u, 4u, 8u, 0xc0u, 0x128u, 0x1c0u, 0x858u, 0x878u})
        memory_.Write32(sys + offset, 0);
    memory_.Write32(sys + 0x98, 0xffffffffu);
    memory_.Write32(sys + 0x828, 1);
    memory_.Write32(decoder + 8, 0);
}

void MovieDecoder::Reset(CpuContext& context) {
    const uint32_t decoder = Arg(context, 0);
    if (!Owns(decoder)) {
        Fallback(reset_original_, context, kReset);
        return;
    }
    const uint32_t sys = memory_.Read32(decoder + 0x40);
    if (sys == 0 || !memory_.Translate(sys, 0x19a0)) {
        ++invalid_work_;
        Return(context, 0);
        return;
    }
    ResetSoftware(context);
    stopped_decoder_ = 0;
    Return(context, 1);
}

void MovieDecoder::DecodePicture(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    if (!Owns(decoder)) {
        Fallback(picture_original_, context, kDecodePicture);
        return;
    }
    const uint32_t sys = memory_.Read32(decoder + 0x40);
    if (decoder == stopped_decoder_ || sys == 0 || !memory_.Translate(sys, 4) || memory_.Read32(sys) != 0) {
        ++no_picture_;
        Return(context, 0);
        return;
    }
    const uint32_t decoded = memory_.Read32(decoder + 0x54);
    const uint32_t plane = (decoded & 1u) && memory_.Read32(decoder + 0x158) != 0 ? 1u : 0u;
    const uint32_t format = memory_.Read32(decoder + 0x154);
    if (format < 0x100u || format > 0x103u) {
        Fallback(picture_original_, context, kDecodePicture);
        return;
    }
    const bool rgb16 = format == 0x102u || format == 0x103u;
    const uint32_t width = memory_.Read32(decoder + 0x58);
    const uint32_t height = memory_.Read32(decoder + 0x5c);
    const uint32_t destination = memory_.Read32(decoder + 0x94u + plane * 4u);
    const uint64_t bytes = uint64_t{width} * height * (rgb16 ? 2u : 4u);
    const bool owned = decoder == picture_decoder_ && format == 0x100u;
    // Both retained picture layouts consist of complete 16x16 blocks. Do not
    // consume the provider's next picture unless its final output is backed.
    if (width == 0 || height == 0 || (width & 15u) != 0 || (height & 15u) != 0 ||
        (!owned && (destination == 0 || bytes > 0xffffffffu ||
        uint64_t{destination} + bytes > 0x100000000ull ||
        !memory_.Translate(destination, static_cast<size_t>(bytes))))) {
        ++invalid_picture_;
        Return(context, 0);
        return;
    }
    context.cpuRegs.GPR.r[4].UD[0] = decoder;
    context.cpuRegs.GPR.r[5].UD[0] = memory_.Read32(decoder + (rgb16 ? 0x9cu : 0x94u + plane * 4u));
    context.cpuRegs.GPR.r[6].UD[0] = rgb16 ? (width >> 4) * (height >> 4) : (width * height) >> 8;
    // The service preserves timer pumping, output conversion, PCM and pacing.
    // No DMA snapshot/restore or IPU command is needed to deliver a picture.
    if (owned) picture_services_.serve(context);
    else if (rgb16) hooks_.picture_rgb16(context);
    else hooks_.picture_rgb32(context);
    if (context.cpuRegs.GPR.r[2].SL[0] < 0) {
        ++no_picture_;
        // The blocking software service drains accepted input before reporting
        // no picture. Once the file refill also ended, a failed decode cannot
        // become ready later. Stop owned playback instead of holding malformed
        // input forever. Picture-only installations retain their retry policy.
        if (decoder == lifecycle_decoder_ && memory_.Read32(decoder + 0x15c) != 0) {
            ++decode_errors_;
            Release(decoder);
        }
        Return(context, 0);
        return;
    }
    ++pictures_;
    if (rgb16) ++rgb16_;
    memory_.Write32(decoder + 0x54, decoded + 1u);
    Return(context, 1);
}

void MovieDecoder::Initialise(CpuContext& context) {
    ++initialized_;
    if (picture_services_.close) picture_services_.close();
    picture_decoder_ = 0;
    hooks_.initialize();
    stopped_decoder_ = memory_.Read32(kDecoderPointer);
    lifecycle_decoder_ = 0;
    playback_owned_ = false;
    audio_output_owned_ = false;
    Return(context, 1);
}

uint32_t MovieDecoder::Allocate(uint64_t size) {
    const uint64_t next = (uint64_t{memory_.Read32(kArenaNext)} + 63u) & ~uint64_t{63};
    const uint64_t end = next + size;
    if (next == 0 || size == 0 || end > 0xffffffffu || end > memory_.Read32(kArenaEnd) ||
        !memory_.Translate(static_cast<uint32_t>(next), static_cast<size_t>(size))) return 0;
    memory_.Write32(kArenaNext, static_cast<uint32_t>(end));
    return static_cast<uint32_t>(next);
}

bool MovieDecoder::Configure(uint32_t decoder, const std::array<uint32_t, 12>& args) {
    const auto put = [this, decoder](uint32_t offset, uint32_t value) { memory_.Write32(decoder + offset, value); };
    if (args[0] != 0) {
        const uint32_t read = Allocate(0x4000);
        if (read == 0) return false;
        put(0x48, read);
        const uint32_t video = Allocate(0x40000);
        if (video == 0) return false;
        put(0xf0, video);
        put(0xf4, 0x40000);
        put(0x60, args[7]);
        put(0x64, args[6]);
        put(0x68, args[8]);
        put(0x6c, args[10]);
        put(0x144, args[2]);
        put(0x148, args[3]);
        put(0x14c, args[4]);
        put(0x150, args[5]);
        put(0x154, args[11]);
        put(0x158, args[9]);
    }
    put(0x4c, 0);
    const uint32_t video = memory_.Read32(decoder + 0xf0);
    if (video == 0 || uint64_t{video} + 0x40u > 0xffffffffu ||
        !memory_.Translate(video, 0x40)) return false;
    put(0xfc, video);
    put(0xf8, video + 0x40);
    put(0x100, video + 0x40);
    if (args[4] != 0) {
        uint32_t size = 0;
        if (args[0] != 0) {
            size = args[4] == 2 ? 0x8000 : 0x19000;
            const uint32_t audio = Allocate(size);
            if (audio == 0) return false;
            put(0x104, audio);
        }
        // NTSC Start's restart arm clears audioSize; preserve that software
        // contract while the caller still owns the audio ring lifecycle.
        put(0x108, size);
        put(0x10c, 0);
        put(0x110, 0);
        put(0x140, 0);
    }
    return true;
}

bool MovieDecoder::AllocatePictures(CpuContext& context, uint32_t decoder) {
    const uint32_t columns = memory_.Read32(decoder + 0x58) >> 4;
    const uint32_t rows = memory_.Read32(decoder + 0x5c) >> 4;
    const uint64_t blocks = uint64_t{columns} * rows;
    const uint32_t format = memory_.Read32(decoder + 0x154);
    if (format == 0x100 && picture_services_.begin) {
        if (!picture_services_.begin(decoder, columns * 16, rows * 16)) return false;
        picture_decoder_ = decoder;
        // No packet/guest-plane identity survives this selection. The player
        // queue carries checked picture IDs supplied by the picture owner.
        for (uint32_t offset : {0x94u, 0x98u, 0x9cu, 0xa8u, 0xc8u})
            memory_.Write32(decoder + offset, 0);
        return true;
    }
    const bool rgb16 = format == 0x102 || format == 0x103;
    if (rgb16) {
        const uint32_t raw = Allocate(blocks * 0x180);
        if (raw == 0) return false;
        memory_.Write32(decoder + 0x9c, raw);
    }
    const uint32_t count = memory_.Read32(decoder + 0x158) != 0 ? 2u : 1u;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t picture = Allocate(blocks * (rgb16 ? 0x200u : 0x400u));
        if (picture == 0) return false;
        memory_.Write32(decoder + 0x94 + index * 4, picture);
        const uint32_t packet = Allocate(((uint64_t{columns} * 6u + 8u) / 4u) * 64u);
        if (packet == 0) return false;
        memory_.Write32(decoder + 0xa8 + index * 0x20, packet);
        // This packet is still consumed by the live presentation path. Its
        // builder only describes the upload; it does not start any DMA.
        Arguments(context, {packet, picture, columns, rows, rgb16 ? 2u : 0u});
        hooks_.call(context, 0x002b1448u);
        if (format == 0x101 || format == 0x103) memory_.Write32(decoder + 0xe8 + index * 4, 0);
    }
    return true;
}

bool MovieDecoder::StartPlayback(CpuContext& context, const std::array<uint32_t, 12>& args) {
    const bool create = args[0] != 0;
    const uint32_t decoder = args[1];
    stopped_decoder_ = 0;
    memory_.Write32(kDecoderPointer, decoder);
    memory_.Write32(decoder + 0x15c, 0);
    if (!Configure(decoder, args)) return false;
    const bool input_owned = input_installed_ && OwnsStream(context) &&
                             memory_.Read32(decoder + 0x14c) <= 1;
    const uint32_t seek = memory_.Read32(decoder + 0x64);
    if (seek != 0) {
        Arguments(context, {args[8]});
        hooks_.call(context, seek);
    }
    memory_.Write32(decoder + 0x54, 0);
    if (create) {
        Arguments(context, {decoder, context.cpuRegs.GPR.r[29].UL[0], 0x2800});
        Create(context);
        if (context.cpuRegs.GPR.r[2].SL[0] == 0) return false;
        if (input_owned) {
            ++registrations_retired_;
        } else {
            Arguments(context, {memory_.Read32(decoder + 0x150)});
            hooks_.call(context, 0x002b1120u);
        }
    }
    Arguments(context, {1});
    if (input_owned) Demultiplex(context); else hooks_.call(context, kDemultiplex);
    if (context.cpuRegs.GPR.r[2].SL[0] == 0) return false;
    if (!create) {
        Arguments(context, {decoder});
        Reset(context);
        if (context.cpuRegs.GPR.r[2].SL[0] == 0) return false;
        // Restart reuses the descriptor/arena, but an owned picture session
        // still needs fresh IDs after Close (its guest plane words are zero).
        if (memory_.Read32(decoder + 0x154) == 0x100 && picture_services_.begin)
            return AllocatePictures(context, decoder);
        return true;
    }
    const uint32_t header = memory_.Read32(decoder + 0x100);
    const uint8_t* sequence = memory_.Translate(header, 7);
    if (!sequence) return false;
    const uint32_t width = (uint32_t{sequence[4]} << 4) | (sequence[5] >> 4);
    const uint32_t height = ((sequence[5] & 15u) << 8) | sequence[6];
    if (sequence[0] != 0 || sequence[1] != 0 || sequence[2] != 1 || sequence[3] != 0xb3 ||
        width < 16 || height < 16 || (width & 15u) != 0 || (height & 15u) != 0) return false;
    memory_.Write32(decoder + 0x58, width);
    memory_.Write32(decoder + 0x5c, height);
    const uint32_t size = ((width * height * 9u) >> 1) + 0x2800u;
    const uint32_t work = Allocate(size);
    if (work == 0) return false;
    memory_.Write32(decoder + 0xa4, work);
    Arguments(context, {decoder, work, size});
    Create(context);
    if (context.cpuRegs.GPR.r[2].SL[0] == 0) return false;
    // Keep the software error callback, but never register FeedIpu or the IPU
    // background poll. Those consumers belong to the retired decoder.
    const uint32_t sys = memory_.Read32(decoder + 0x40);
    memory_.Write32(sys + 0xc, 0x002b1240u);
    memory_.Write32(sys + 0x10, 0);
    memory_.Write32(sys + 0x14, context.cpuRegs.GPR.r[28].UL[0]);
    if (input_owned) {
        ++registrations_retired_;
    } else {
        Arguments(context, {memory_.Read32(decoder + 0x150)});
        hooks_.call(context, 0x002b1120u);
    }
    return AllocatePictures(context, decoder);
}

void MovieDecoder::Start(CpuContext& context) {
    // NTSC Open passes eight register arguments, then path/double buffering/
    // texture width/output format in eight-byte stack slots.
    if (Arg(context, 2) != 1 || Arg(context, 3) != 2 || Arg(context, 4) > 3) {
        Fallback(start_original_, context, kStart);
        return;
    }
    const uint32_t stack = context.cpuRegs.GPR.r[29].UL[0];
    const uint32_t decoder = Arg(context, 1);
    if (decoder == 0 || !memory_.Translate(decoder, 0x160) ||
        stack < 0x28d0 || !memory_.Translate(stack, 0x1c) ||
        !memory_.Translate(stack - 0x28d0, 0x28d0)) {
        ++invalid_work_;
        ++start_failed_;
        if (player_buffers_failed_) player_buffers_failed_(context);
        Return(context, 0);
        return;
    }
    std::array<uint32_t, 12> args{};
    for (uint32_t i = 0; i < 8; ++i) args[i] = Arg(context, i);
    for (uint32_t i = 8; i < 12; ++i) args[i] = memory_.Read32(stack + (i - 8) * 8);
    if (args[11] < 0x100 || args[11] > 0x103 ||
        (args[0] == 0 && (!Owns(decoder) || memory_.Read32(decoder + 0x144) != 1))) {
        Fallback(start_original_, context, kStart);
        return;
    }
    const auto saved = context.cpuRegs.GPR;
    const uint32_t arena = memory_.Read32(kArenaNext);
    std::array<uint8_t, 0x160> descriptor{};
    std::memcpy(descriptor.data(), memory_.Translate(decoder, descriptor.size()), descriptor.size());
    const uint32_t previous = memory_.Read32(kDecoderPointer);
    // Preparation may enter retained services before the software provider
    // sees compressed input. The player already owns this buffer transaction.
    playback_owned_ = true;
    skip_requested_ = false;
    if (args[0] != 0) audio_output_owned_ = false;
    if (previous != decoder && Owns(previous)) Release(previous);
    if (args[0] != 0 && picture_services_.close) {
        picture_services_.close();
        picture_decoder_ = 0;
    }
    context.cpuRegs.GPR.r[29].UD[0] = stack - 0x28d0;
    // A failed open owns no buffers and must not retain the temporary stack
    // context, queued PCM or decoder pictures. The next open may reuse the
    // same arena; writes to its unused bytes are deliberately not replayed.
    const auto fail = [&] {
        Release(decoder);
        if (picture_services_.close) picture_services_.close();
        picture_decoder_ = 0;
        std::memcpy(memory_.Translate(decoder, descriptor.size()), descriptor.data(), descriptor.size());
        memory_.Write32(kDecoderPointer, decoder);
        memory_.Write32(kArenaNext, arena);
        memory_.Write32(decoder + 0x15c, 1);
        playback_owned_ = false;
        audio_output_owned_ = false;
        ++start_failed_;
        if (player_buffers_failed_) player_buffers_failed_(context);
    };
    bool success;
    try {
        success = StartPlayback(context, args);
    } catch (...) {
        fail();
        RestoreCaller(context, saved);
        throw;
    }
    RestoreCaller(context, saved);
    if (success) {
        stopped_decoder_ = 0;
        lifecycle_decoder_ = decoder;
        ++started_;
    } else {
        fail();
    }
    Return(context, success ? 1 : 0);
}

void MovieDecoder::Finished(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    if (!Owns(decoder)) {
        Fallback(finished_original_, context, kFinished);
        return;
    }
    if (decoder == stopped_decoder_) {
        Return(context, 1);
        return;
    }
    if (skip_requested_) {
        // A5: the player asked for the end; the title's Step takes its Stop path.
        Return(context, 1);
        return;
    }
    const uint32_t sys = memory_.Read32(decoder + 0x40);
    const bool refill_ended = memory_.Read32(decoder + 0x15c) != 0;
    const bool decoder_ended = sys != 0 && memory_.Translate(sys, 4) && memory_.Read32(sys) != 0;
    // The old scene abort hold only deferred refill EOF. Preserve the public
    // software decoder's own terminal/error result independently of it.
    if (decoder_ended || !refill_ended) {
        Return(context, decoder_ended ? 1 : 0);
        return;
    }
    // Finished is also the stop condition of the player's initial audio-fill
    // loop. The existing PCM provider queues audio directly and does not fill
    // that loop's old IOP ring. EOF must release the fill loop before sound
    // starts; otherwise two queued pictures keep it waiting forever. NTSC
    // Prepare clears g_SoundStarted and StartSound sets it only after the loop.
    // The no-audio arm has no fill wait, so it needs no EOF exception.
    const bool filling_audio = memory_.Read32(decoder + 0x14c) != 0 &&
                               memory_.Read32(kSoundStarted) == 0;
    if (filling_audio) {
        ++priming_eof_;
        Return(context, 1);
        return;
    }
    // During playback, accepted pictures and compressed backlog survive a
    // front-loaded refill EOF. This replaces the scene abort hold
    // at the Movie service boundary without rewriting the gameplay state.
    const bool pending = hooks_.has_pending(decoder);
    if (pending) ++eof_deferred_;
    Return(context, pending ? 0 : 1);
}

void MovieDecoder::Release(uint32_t decoder) {
    if (decoder != stopped_decoder_) hooks_.destroy(decoder);
    stopped_decoder_ = decoder;
    if (decoder == lifecycle_decoder_) lifecycle_decoder_ = 0;
}

void MovieDecoder::Close(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    if (!Owns(decoder)) {
        Fallback(close_original_, context, kClose);
        return;
    }
    ++closed_;
    Arguments(context, {decoder});
    Release(decoder);
    if (picture_services_.close) picture_services_.close();
    picture_decoder_ = 0;
    playback_owned_ = false;
    skip_requested_ = false;
    audio_output_owned_ = false;
    // Retain the guest context: retail sceMpegDelete leaves it intact.
    // Host playback ownership is released through the software provider.
    Return(context, 1);
}

void MovieDecoder::FeedAudio(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    if (!Owns(decoder)) {
        Fallback(feed_audio_original_, context, kFeedAudio);
        return;
    }
    const uint32_t audio = memory_.Read32(decoder + 0x14c);
    if (audio == 0) return;
    // MPEG's software demux already delivered this movie's PCM to Audio. Its
    // legacy ring stays empty. Calling the player's consumer for zero bytes
    // merely polls the IOP block-transfer model; it cannot deliver more PCM.
    // Only retire that proven path. Foreign consumers or restored/nonempty
    // legacy rings still need their existing handler, rather than losing data.
    const uint32_t size = memory_.Read32(decoder + 0x108);
    const uint32_t read = memory_.Read32(decoder + 0x10c);
    if (audio != 1 || Arg(context, 0) != kPlayerFeedSound || Arg(context, 1) != 0 ||
        memory_.Read32(kSoundMode) != 1 || memory_.Read32(decoder + 0x110) != 0 ||
        size == 0 || size > 0x7fffffffu || read >= size) {
        Fallback(feed_audio_original_, context, kFeedAudio);
        return;
    }
    ++audio_feeds_;
    Return(context, 0);  // the old callback also consumed zero bytes
}

void MovieDecoder::SetVolume(CpuContext& context) {
    if (memory_.Read32(kSoundMode) != 1) return;  // source non-PCM arm is void
    const float volume = context.fpuRegs.fpr[12].f;
    if (!Owns(memory_.Read32(kDecoderPointer)) || !hooks_.volume ||
        !std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
        Fallback(volume_original_, context, kSetVolume);
        return;
    }
    // NTSC 0x2af474: single-precision multiply by 32767, truncate to an integer.
    // Preserve that gain quantization before handing the intent to Audio.
    hooks_.volume(static_cast<uint16_t>(volume * 32767.0f));
    ++volume_changes_;
}

bool MovieDecoder::OwnsStream(CpuContext& context) {
    const uint32_t gp = context.cpuRegs.GPR.r[28].UL[0];
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    return stream_installed_ && Owns(decoder) &&
           memory_.Read32(decoder + 0x144) == 1 &&
           memory_.Read32(decoder + 0x60) == kReadStream &&
           memory_.Read32(decoder + 0x64) == kSeekStream &&
           gp >= kFileLocationGp && memory_.Translate(gp - kFileLocationGp, 12);
}

void MovieDecoder::SeekStream(CpuContext& context) {
    if (!OwnsStream(context)) {
        Fallback(seek_original_, context, kSeekStream);
        return;
    }
    const uint32_t gp = context.cpuRegs.GPR.r[28].UL[0];
    const uint32_t location = memory_.Read32(gp - kFileLocationGp);
    ++stream_seeks_;
    const int32_t result = stream_.seek(location);
    // NTSC clears the title's refill count even when the seek service fails.
    // This is distinct from the provider cursor, which read-ahead also moves.
    memory_.Write32(gp - kReadPositionGp, 0);
    if (result <= 0) ++stream_errors_;
    Return(context, result);
}

void MovieDecoder::ReadStream(CpuContext& context) {
    if (!OwnsStream(context)) {
        Fallback(read_original_, context, kReadStream);
        return;
    }
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    const uint32_t gp = context.cpuRegs.GPR.r[28].UL[0];
    const uint32_t size = Arg(context, 1);
    const uint32_t position = memory_.Read32(gp - kReadPositionGp);
    const uint32_t file_size = memory_.Read32(gp - kFileSizeGp);
    // Match the title's unsigned, wrapping EOF predicate before touching the
    // destination. EOF changes neither accepted decoder input nor queued PCM.
    if (decoder == stopped_decoder_ ||
        (uint32_t{position + size} >= file_size && position >= file_size)) {
        ++stream_eof_;
        Return(context, -1);
        return;
    }
    if (size > 0x7fffffffu) {
        Fallback(read_original_, context, kReadStream);
        return;
    }
    const uint32_t sectors = size / 2048u;
    const uint32_t bytes = sectors * 2048u;
    uint8_t* destination = bytes ? memory_.Translate(Arg(context, 0), bytes) : nullptr;
    if (bytes != 0 && (Arg(context, 0) == 0 ||
        uint64_t{Arg(context, 0)} + bytes > 0x100000000ull || !destination)) {
        ++stream_errors_;
        Return(context, -1);
        return;
    }
    ++stream_reads_;
    const int32_t got = sectors ? stream_.read(destination, sectors) : 0;
    if (got < 0 || static_cast<uint32_t>(got) > sectors) {
        ++stream_errors_;
        Return(context, -1);
        return;
    }
    const uint32_t delivered = static_cast<uint32_t>(got) * 2048u;
    memory_.Write32(gp - kReadPositionGp, position + delivered);
    stream_bytes_ += delivered;
    Return(context, static_cast<int32_t>(delivered));
}

void MovieDecoder::Demultiplex(CpuContext& context) {
    const uint32_t decoder = memory_.Read32(kDecoderPointer);
    if (!input_installed_ || !OwnsStream(context) || memory_.Read32(decoder + 0x14c) > 1) {
        Fallback(demux_original_, context, kDemultiplex);
        return;
    }
    if (decoder == stopped_decoder_) {
        Return(context, 0);
        return;
    }
    constexpr uint32_t capacity = 0x4000;
    const uint32_t buffer = memory_.Read32(decoder + 0x48);
    int32_t pending = static_cast<int32_t>(memory_.Read32(decoder + 0x4c));
    const auto error = [&] {
        ++input_errors_;
        memory_.Write32(decoder + 0x15c, 1);
        Return(context, 0);
    };
    if (buffer == 0 || uint64_t{buffer} + capacity > 0x100000000ull ||
        !memory_.Translate(buffer, capacity) ||
        pending > static_cast<int32_t>(capacity)) {
        error();
        return;
    }
    uint32_t data = buffer;
    if (pending == 0) {
        Arguments(context, {buffer, capacity, memory_.Read32(decoder + 0x68)});
        ReadStream(context);
        pending = context.cpuRegs.GPR.r[2].SL[0];
        memory_.Write32(decoder + 0x4c, static_cast<uint32_t>(pending));
    } else if (pending > 0) {
        // The source keeps a partial window at the end of its read buffer.
        data += capacity - static_cast<uint32_t>(pending);
    }
    if (pending < 0) {
        // Source EOF belongs to the player. Already accepted pictures and
        // PCM belong to the software provider until delivery/Close.
        memory_.Write32(decoder + 0x15c, 1);
        Return(context, 0);
        return;
    }
    if (pending == 0) {
        // This file provider completes synchronously. There is no disc-ring
        // readiness poll to reproduce, including on Start's wait=true arm.
        // Exhausted input must also release the initial audio-fill loop.
        memory_.Write32(decoder + 0x15c, 1);
        Return(context, 0);
        return;
    }
    // NTSC 0x2b0c3c clears audioFull; +0x140 is the header-byte count.
    memory_.Write32(decoder + 0x13c, 0);
    const int32_t used = demux_(decoder, data, pending);
    if (used < 0 || used > pending) {
        error();
        return;
    }
    ++demux_windows_;
    demux_bytes_ += static_cast<uint32_t>(used);
    memory_.Write32(decoder + 0x4c, static_cast<uint32_t>(pending - used));
    Return(context, used);
}

bool MovieDecoder::CanCheckpoint(std::string* reason) const {
    if (!playback_owned_ && !audio_output_owned_ && !picture_decoder_ && !lifecycle_decoder_) return true;
    if (reason) *reason = "Movie player owns playback buffers: wait for Close";
    return false;
}

uint32_t MovieDecoder::Services() const {
    return uint32_t(lifecycle_installed_) | uint32_t(stream_installed_) << 1 |
        uint32_t(input_installed_) << 2 | uint32_t(audio_installed_) << 3 |
        uint32_t(audio_playback_selected_) << 4 | uint32_t(bool(player_buffers_active_)) << 5 |
        uint32_t(bool(picture_services_.serve)) << 6;
}

bool MovieDecoder::ValidateClosed(uint32_t stopped, std::string* reason) const {
    if ((stopped && !memory_.Translate(stopped, 0x160)) ||
        ((lifecycle_installed_ || stopped) && stopped != memory_.Read32(kDecoderPointer))) {
        if (reason) *reason = "Movie stopped decoder disagrees with restored RAM";
        return false;
    }
    return true;
}

bool MovieDecoder::RestoreClosed(uint32_t stopped, std::string* reason) {
    if (!CanCheckpoint(reason) || !ValidateClosed(stopped, reason)) return false;
    stopped_decoder_ = stopped;
    ++restore_resets_;
    return true;
}

void MovieDecoder::ResetAfterRestore() {
    if (!lifecycle_installed_) return;
    if (picture_services_.close) picture_services_.close();
    picture_decoder_ = 0;
    hooks_.initialize();
    stopped_decoder_ = memory_.Read32(kDecoderPointer);
    lifecycle_decoder_ = 0;
    playback_owned_ = false;
    audio_output_owned_ = false;
    ++restore_resets_;
}

void MovieDecoder::RequestSkip() {
    if (!playback_owned_) return;
    skip_requested_ = true;
    ++skips_;
}

std::string MovieDecoder::Census() const {
    return "created=" + std::to_string(creates_) + ",reset=" + std::to_string(resets_) +
           ",pictures=" + std::to_string(pictures_) + ",rgb16=" + std::to_string(rgb16_) +
           ",nopic=" + std::to_string(no_picture_) + ",invalid_work=" + std::to_string(invalid_work_) +
           ",fallback=" + std::to_string(fallbacks_) + ",continuations=" + std::to_string(continuations_) +
           ",initialized=" + std::to_string(initialized_) + ",started=" + std::to_string(started_) +
           ",closed=" + std::to_string(closed_) + ",start_failed=" + std::to_string(start_failed_) +
           ",eof_deferred=" + std::to_string(eof_deferred_) +
           ",invalid_picture=" + std::to_string(invalid_picture_) +
           ",priming_eof=" + std::to_string(priming_eof_) +
           ",decode_errors=" + std::to_string(decode_errors_) +
           ",audio_feeds=" + std::to_string(audio_feeds_) +
           ",volume_changes=" + std::to_string(volume_changes_) +
           ",stream_seeks=" + std::to_string(stream_seeks_) +
           ",stream_reads=" + std::to_string(stream_reads_) +
           ",stream_bytes=" + std::to_string(stream_bytes_) +
           ",stream_eof=" + std::to_string(stream_eof_) +
           ",stream_errors=" + std::to_string(stream_errors_) +
           ",restore_resets=" + std::to_string(restore_resets_) +
           ",demux_windows=" + std::to_string(demux_windows_) +
           ",skips=" + std::to_string(skips_) +
           ",demux_bytes=" + std::to_string(demux_bytes_) +
           ",input_errors=" + std::to_string(input_errors_) +
           ",registrations_retired=" + std::to_string(registrations_retired_) +
           ",audio_original_calls=" + std::to_string(audio_original_calls_) +
           ",stream_original_calls=" + std::to_string(stream_original_calls_) +
           ",input_original_calls=" + std::to_string(input_original_calls_) +
           ",audio_playback_original_calls=" + std::to_string(audio_playback_original_calls_) +
           ",audio_started=" + std::to_string(audio_started_) +
           ",audio_stopped=" + std::to_string(audio_stopped_);
}

}  // namespace title
