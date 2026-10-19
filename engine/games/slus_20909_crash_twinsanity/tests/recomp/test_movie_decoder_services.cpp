// MovieDecoder over the production MPEG/PCM services, with file inputs supplied
// by the caller. No game run or emulator: file/demux providers are synchronous;
// upload construction/presentation is deliberately outside scope.
// OLD pins: BUG-045 refill EOF, S45-FMVAUD skip/reopen flush, R136/R156/R181
// two-picture priming and the tick grid carried between movies on one context.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_sound.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/libipu_api.h"
#include "engine/ipu/hle/libmpeg_bindings.h"
#include "engine/ipu/hle/mpeg_api.h"
#include "engine/ipu/ipu_device.h"
#include "engine/sound/fmv_pcm_input.h"
#include "engine/sound/hle/sd_block_trans.h"
#include "engine/sound/hle/sdr_remote.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "tests/ipu/file_fmv_sector_source.h"
#include "tests/ipu/recording_dmac_channel_face.h"
#include "tests/ipu/vector_guest_memory.h"

namespace {

using Movie = title::MovieDecoder;
using namespace ps2r::ipu;
unsigned checks = 0;

void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        std::exit(1);
    }
}

uint64_t Hash(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

class MovieFileSource : public FmvSectorSource {
public:
    title::MovieStream* owner = nullptr;
    bool Open(const std::string& path) { return retained_.Open(path); }
    uint32_t PumpSectors(uint8_t* destination, uint32_t sectors) override {
        if (!owner || !owner->OwnsCursor()) return retained_.PumpSectors(destination, sectors);
        const int32_t got = owner->Read(destination, sectors);
        return got > 0 ? uint32_t(got) : 0;
    }
private:
    FileFmvSectorSource retained_;
};

class TestMovieDecoderServices {
public:
    enum class CloseMode { Playback, ResetOracle, Destroy, NewSession, RestoredSession };
    struct Delivery {
        int64_t arrival, wait;
        bool no_park;
        uint64_t picture;
        bool operator==(const Delivery& other) const {
            return arrival == other.arrival && wait == other.wait &&
                   no_park == other.no_park && picture == other.picture;
        }
    };
    struct ClockTrace {
        std::vector<Delivery> pictures;
        uint64_t pcm_hash = 0;
        uint32_t pcm_frames = 0;
    };
    struct Field {
        int64_t tick;
        uint64_t audio, consumed;
        bool audible;
        bool operator==(const Field& other) const {
            return tick == other.tick && audio == other.audio &&
                   consumed == other.consumed && audible == other.audible;
        }
    };
    struct ProviderTrace {
        std::vector<Delivery> pictures;
        std::vector<Field> fields;
        uint64_t consumed = 0, reanchors = 0;
    };
    static constexpr uint32_t kDecoder = 0x10000, kStack = 0x1f00000, kArena = 0x400000;
    static constexpr uint32_t kSeek = Movie::kSeekStream, kRead = Movie::kReadStream, kGp = 0x320000;
    VectorGuestMemory memory{32u << 20};
    MovieFileSource source;
    std::vector<uint8_t> file_bytes;
    title::MovieStream file_owner{memory, [this](uint64_t offset, size_t bytes, uint8_t* destination) -> int64_t {
        constexpr uint64_t base = 17 * 2048;
        if (offset < base) return -1;
        offset -= base;
        if (offset >= file_bytes.size()) return 0;
        const size_t count = std::min(bytes, file_bytes.size() - size_t(offset));
        std::copy_n(file_bytes.data() + offset, count, destination);
        return int64_t(count);
    }};
    bool file_owner_selected = false;
    ps2r::sound::FmvPcmInput pcm;
    ps2r::sound::SpuCore sound;
    ps2r::sound::Mixer mixer{sound};
    ps2r::app::MovieSound sound_owner;
    ps2r::sound::hle::SdBlockTrans retired_block{8, [] { return uint64_t{0}; }};
    ps2r::sound::hle::SdrRemote retired_audio{sound, retired_block, {}, {}};
    RecordingDmacChannelFace dmac;
    IpuDevice ipu;
    LibipuApi libipu{ipu, dmac};
    FmvContextLayout layout = FmvContextLayoutData::CrashTwinsanityUS();
    IpuSettings settings;
    MpegApi api{memory, layout, source, pcm,
                [] { return std::make_unique<FfmpegVideoDecoder>(); }, settings};
    int64_t tick = 1000;
    int64_t mixed_tick = tick;
    bool field_audio = false;
    std::vector<Field> fields;
    unsigned pumps = 0, presents = 0;
    bool last_no_park = false;
    std::vector<int64_t> parks;
    LibmpegBindings bindings{api, memory, layout, {
        [this](CpuContext&) { ++pumps; },
        [this] { return tick; },
        [this](int64_t count) { parks.push_back(count); AdvanceFields(count); },
        [this](bool no_park) {
            ++presents;
            last_no_park = no_park;
            if (no_park) PumpFields();
        },
    }};
    std::string root, path;
    CloseMode close_mode;
    bool stream_selected;
    unsigned retained_file_calls = 0;
    unsigned retained_demux_calls = 0, retained_registration_calls = 0;
    Movie movie{memory, {
        [this](CpuContext& c) { bindings.SceMpegCreate(c); },
        [this](CpuContext& c) { bindings.SceMpegReset(c); },
        [this](CpuContext& c) { bindings.SceMpegGetPicture(c); },
        [this](CpuContext& c) { bindings.SceMpegGetPictureRaw8(c); },
        [this] { bindings.InitializePlayback(); },
        [this](uint32_t decoder) {
            if (close_mode == CloseMode::ResetOracle) api.Reset(decoder);
            else if (close_mode == CloseMode::Destroy) bindings.DestroyPlayback(decoder);
            else bindings.ClosePlayback(decoder);
        },
        [this](CpuContext& c, uint32_t address) { RetainedService(c, address); },
        [this](uint32_t decoder) { return bindings.HasPendingPlayback(decoder); },
        [this](uint16_t gain) { sound_owner.SetGain(gain); },
    }};
    CpuContext context{};
    std::map<uint32_t, Movie::Body> installed;

    explicit TestMovieDecoderServices(std::string game_root, CloseMode mode = CloseMode::Playback,
                                      bool select_stream = true, int select_audio_playback = -1,
                                      bool select_file_owner = false, bool select_pcm = true)
        : sound_owner(select_pcm, pcm, sound, mixer),
          root(std::move(game_root)), close_mode(mode), stream_selected(select_stream) {
        const Movie::Capture capture = [this](uint32_t address) -> Movie::Body {
            if (address == Movie::kSoundRemote) {
                return [this](CpuContext& c, uint32_t) {
                    c.cpuRegs.GPR.r[2].UD[0] = retired_audio.Dispatch(
                        c.cpuRegs.GPR.r[5].UL[0], c.cpuRegs.GPR.r[6].UL[0], c.cpuRegs.GPR.r[7].UL[0],
                        c.cpuRegs.GPR.r[8].UL[0], c.cpuRegs.GPR.r[9].UL[0]);
                };
            }
            if (address == Movie::kSetVolume) {
                // Keep a working replaced-model arm: a mistaken fallback
                // could produce correct sound, but must fail its zero counter.
                return [this](CpuContext& c, uint32_t) {
                    const uint32_t gain = static_cast<uint32_t>(c.fpuRegs.fpr[12].f * 32767.0f);
                    retired_audio.Dispatch(ps2r::sound::hle::SdrRemote::kSetParam, 0xf80, gain, 0, 0);
                    retired_audio.Dispatch(ps2r::sound::hle::SdrRemote::kSetParam, 0x1080, gain, 0, 0);
                };
            }
            return [](CpuContext&, uint32_t) { Check(false, "production fixture must not reach retired bodies"); };
        };
        const Movie::Installer install = [this](uint32_t address, Movie::Body body) {
            installed[address] = std::move(body);
            return true;
        };
        Check(movie.Install(capture, install, true) == 7, "production lifecycle boundaries installed");
        Check(movie.InstallAudio(capture, install) == 2, "production audio boundaries installed");
        if (select_file_owner) {
            Check(stream_selected, "owned file cursor requires the selected Movie input callbacks");
            Check(file_owner.Install(capture, install, true) == 3, "owned file-control boundary installed");
            source.owner = &file_owner;
            file_owner_selected = true;
        }
        if (select_audio_playback >= 0)
            Check(movie.InstallAudioPlayback(capture, install, select_audio_playback != 0) == 1,
                  "checked movie start/stop service boundary installed");
        if (stream_selected) {
            Check(movie.InstallStream(capture, install, {
                [this](uint32_t location) {
                    Check(location == 17, "file provider receives the location published by stream open");
                    if (file_owner.OwnsCursor()) return file_owner.Seek(location);
                    return source.Open(path) ? 1 : 0;
                },
                [this](uint8_t* destination, uint32_t sectors) {
                    if (file_owner.OwnsCursor()) return file_owner.Read(destination, sectors);
                    return static_cast<int32_t>(source.PumpSectors(destination, sectors));
                },
            }) == 2, "production file callbacks installed over the shared sector cursor");
            Check(movie.InstallInput(capture, install,
                [this](uint32_t decoder, uint32_t data, int32_t size) {
                    return bindings.DemuxPlayback(decoder, data, size);
                }) == 1, "production input goes directly to the reusable software demux/PCM service");
        }
        sound.SdInit();
        movie.Initialise(context);
    }

    int32_t Result() const { return context.cpuRegs.GPR.r[2].SL[0]; }
    void PumpFields() {
        if (!field_audio) return;
        // The root's play-mode contract: catch up to fields actually elapsed.
        // A late picture callback at the same tick must not mix twice.
        while (mixed_tick < tick) {
            const uint64_t before = pcm.Consumed();
            mixer.Pump();
            const float* output = mixer.Output();
            const size_t count = static_cast<size_t>(mixer.OutputFrames()) * 2;
            fields.push_back({++mixed_tick,
                Hash(reinterpret_cast<const uint8_t*>(output), count * sizeof(float)),
                pcm.Consumed() - before,
                std::any_of(output, output + count, [](float v) { return v != 0.0f; })});
        }
    }
    void AdvanceFields(int64_t count) {
        for (int64_t i = 0; i < count; ++i) {
            ++tick;
            PumpFields();
        }
    }
    void RetainedService(CpuContext& c, uint32_t address) {
        if (address == kSeek) {
            if (stream_selected) {
                installed.at(kSeek)(c, kSeek);
                Check(c.cpuRegs.GPR.r[2].SL[0] == 1, "movie file opens through the title callback");
            } else {
                ++retained_file_calls;
                Check(source.Open(path), "movie file opens through the previous direct file adapter");
                c.cpuRegs.GPR.r[2].SD[0] = 1;
            }
        } else if (address == 0x2b1120) {
            ++retained_registration_calls;
            // MpegApi performs ring/PCM publication; no guest stream callbacks.
        } else if (address == 0x2b0ae0) {
            ++retained_demux_calls;
            const uint32_t destination = memory.Read32(kDecoder + 0x48);
            const uint32_t bytes = ReadWindow(c, destination, 0x4000);
            if (bytes == 0) {
                memory.Write32(kDecoder + 0x15c, 1);
                api.NotifyCdStreamEof();
            }
            c.cpuRegs.GPR.r[2].SD[0] = api.DemuxPss(kDecoder, destination, bytes);
            memory.Write32(kDecoder + 0x4c, 0);
        } else if (address == 0x2b1448) {
            // Output ownership is real; this upload marker is not a rendering
            // or packet-layout oracle. The graphics owner owns that separate boundary.
            memory.Write32(c.cpuRegs.GPR.r[4].UL[0], 0x12345678);
        } else {
            Check(false, "only file/demux/registration/upload adapters are retained");
        }
    }

    uint32_t ReadWindow(CpuContext& c, uint32_t destination, uint32_t capacity) {
        if (!stream_selected) {
            ++retained_file_calls;
            return source.PumpSectors(memory.Translate(destination, capacity), capacity / 2048) * 2048;
        }
        c.cpuRegs.GPR.r[4].UD[0] = destination;
        c.cpuRegs.GPR.r[5].UD[0] = capacity;
        installed.at(kRead)(c, kRead);
        const int32_t result = c.cpuRegs.GPR.r[2].SL[0];
        return result < 0 ? 0u : static_cast<uint32_t>(result);
    }

    void Start(const char* name) {
        path = root + "/FMV/" + name;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        Check(input.good() && input.tellg() > 0, "stream open has a real nonempty movie size");
        memory.Write32(kGp - Movie::kFileLocationGp, 17);
        memory.Write32(kGp - Movie::kFileSizeGp, static_cast<uint32_t>(input.tellg()));
        memory.Write32(kGp - Movie::kReadPositionGp, 0);
        memory.Write32(Movie::kArenaNext, kArena);
        memory.Write32(Movie::kArenaEnd, 0xb00000);
        memory.Write32(Movie::kSoundStarted, 0);  // source Prepare
        memory.Write32(Movie::kSoundMode, 1);
        context.cpuRegs.GPR.r[29].UD[0] = kStack;
        context.cpuRegs.GPR.r[28].UD[0] = kGp;
        if (file_owner_selected) {
            const size_t size = size_t(input.tellg());
            file_bytes.assign((size + 2047) & ~size_t(2047), 0);
            input.seekg(0);
            input.read(reinterpret_cast<char*>(file_bytes.data()), std::streamsize(size));
            Check(bool(input), "owned file input contains the same complete PSS and final-sector padding");
            constexpr uint32_t player = 0x1a0000, buffer = 0x150031;
            memory.Write32(player + 0x41f8, buffer);
            context.cpuRegs.GPR.r[17].UD[0] = player;
            context.cpuRegs.GPR.r[4].UD[0] = 0x50;
            context.cpuRegs.GPR.r[5].UD[0] = 5;
            context.cpuRegs.GPR.r[6].UD[0] = (buffer + 63) & ~63u;
            context.cpuRegs.GPR.r[31].UD[0] = 0x2ae9c4;
            installed.at(title::MovieStream::kPrepare)(context, title::MovieStream::kPrepare);
            Check(Result() == 1, "source Prepare acknowledged by owned file service");
            context.cpuRegs.GPR.r[4].UD[0] = 17;
            context.cpuRegs.GPR.r[5].UD[0] = kGp - 0x74b8;
            context.cpuRegs.GPR.r[31].UD[0] = 0x2ae9ec;
            installed.at(title::MovieStream::kOpen)(context, title::MovieStream::kOpen);
            Check(Result() == 1 && file_owner.Active(), "source file opens before software decoder Start");
        }
        const std::array<uint32_t, 8> args = {1, kDecoder, 1, 2, 1, 0, kSeek, kRead};
        for (size_t i = 0; i < args.size(); ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        memory.Write32(kStack, 0);
        memory.Write32(kStack + 8, 1);
        memory.Write32(kStack + 16, 10);
        memory.Write32(kStack + 24, 0x100);
        movie.Start(context);
        Check(Result() == 1, "production MPEG services start the movie");
        Check(!movie.CanCheckpoint(), "player owns buffers immediately after Start");
        const bool vivendi = name[0] == 'V';
        Check(memory.Read32(kDecoder + 0x58) == (vivendi ? 640u : 512u) &&
              memory.Read32(kDecoder + 0x5c) == (vivendi ? 480u : 448u),
              "sequence dimensions discovered before allocating output");
        Check(memory.Read32(kDecoder + 0x54) == 0, "Start leaves two-picture priming to player");
    }

    void Prime() {
        const size_t before = parks.size();
        for (unsigned i = 0; i < 2; ++i) {
            movie.DecodePicture(context);
            Check(Result() == 1, "priming picture serves");
            Check(parks.size() == before, "two priming pictures never pace-park");
        }
    }

    void RefillToEof() {
        if (stream_selected) {
            // The player's initial audio fill owns this wait. The real input
            // boundary must release it on source EOF without flushing PCM.
            unsigned attempts = 0;
            while (memory.Read32(kDecoder + 0x15c) == 0 && attempts++ < 10000) {
                context.cpuRegs.GPR.r[4].UD[0] = 0;
                installed.at(Movie::kDemultiplex)(context, Movie::kDemultiplex);
            }
            Check(memory.Read32(kDecoder + 0x15c) == 1 && attempts < 10000,
                  "selected input reaches finite EOF at the player boundary");
        } else {
            constexpr uint32_t window = 0x1000000;
            while (const uint32_t bytes = ReadWindow(context, window, 32768)) {
                api.DemuxPss(kDecoder, window, bytes);
            }
            memory.Write32(kDecoder + 0x15c, 1);
        }
        api.NotifyCdStreamEof();
    }

    bool Finished() {
        movie.Finished(context);
        return Result() != 0;
    }

    void FeedAudio() {
        const uint64_t pushed = pcm.Pushed(), consumed = pcm.Consumed(), flushed = pcm.Flushes();
        const uint32_t queued = pcm.Queued();
        context.cpuRegs.GPR.r[4].UD[0] = Movie::kPlayerFeedSound;
        context.cpuRegs.GPR.r[5].UD[0] = 0;
        installed.at(Movie::kFeedAudio)(context, Movie::kFeedAudio);
        Check(Result() == 0 && pcm.Pushed() == pushed && pcm.Consumed() == consumed &&
              pcm.Queued() == queued && pcm.Flushes() == flushed,
              "already queued software PCM is neither duplicated nor discarded by FeedAudio");
    }

    void Volume(float value) {
        context.fpuRegs.fpr[12].f = value;
        installed.at(Movie::kSetVolume)(context, Movie::kSetVolume);
    }
    void SoundCommand(bool start, uint32_t caller = 0) {
        constexpr uint32_t ring = 0x180000;
        memory.Write32(Movie::kSoundRing, ring);
        memory.Write32(Movie::kSoundRingSize, 0x6000);
        const std::array<uint32_t, 7> args{
            1, 0x80e0, 0, start ? 0x13u : 2u, start ? ring : 0u, start ? 0x6000u : 0u, ring};
        for (size_t i = 0; i < args.size(); ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        context.cpuRegs.GPR.r[31].UD[0] = caller ? caller : (start ? 0x2ae620 : 0x2ae6e4);
        installed.at(Movie::kSoundRemote)(context, Movie::kSoundRemote);
        Check(Result() == 0, "movie sound command preserves the original acknowledgement");
    }

    Delivery RecordPicture() {
        const uint32_t number = memory.Read32(kDecoder + 0x54);
        const uint32_t plane = memory.Read32(kDecoder + ((number & 1) ? 0x98 : 0x94));
        const int64_t arrival = tick;
        movie.DecodePicture(context);
        Check(Result() == 1, "clock comparison serves a real picture");
        const size_t size = size_t{memory.Read32(kDecoder + 0x58)} *
                            memory.Read32(kDecoder + 0x5c) * 4;
        return {arrival, tick - arrival, last_no_park, Hash(memory.Translate(plane, size), size)};
    }

    void Close() {
        if (file_owner_selected) {
            const auto flushed = pcm.Flushes(), consumed = pcm.Consumed();
            context.cpuRegs.GPR.r[31].UD[0] = 0x2af608;
            installed.at(title::MovieStream::kClose)(context, title::MovieStream::kClose);
            Check(Result() == 1 && !file_owner.Active() &&
                  pcm.Flushes() == flushed && pcm.Consumed() == consumed,
                  "source file Close does not flush or drain PCM before decoder Close");
        }
        movie.Close(context);
        Check(movie.CanCheckpoint(), "player Close releases movie quiescence");
        Check(!pcm.GateIsOpen() && pcm.Queued() == 0, "close cuts queued PCM immediately");
        Check(!api.FmvActive() && !bindings.HasPendingPlayback(kDecoder),
              "close discards accepted input and pictures, retaining no active playback");
        if (close_mode == CloseMode::NewSession) movie.Initialise(context);
        if (close_mode == CloseMode::RestoredSession) movie.ResetAfterRestore();
    }

    void CheckNoHardware() {
        Check(bindings.RetainedInitCalls() == 0, "software-only constructor never invokes retained initialization");
        const auto actual = libipu.Count();
        Check(actual.init == 0 && actual.sync == 0 && actual.stop == 0 && actual.restart == 0,
              "software services execute no actual libipu operation, including snapshot-only calls");
        Check(dmac.Log().empty(), "movie services make no DMA register access");
        Check(retired_audio.Calls() == 0, "movie Audio intent never falls back into the sound command model");
        if (sound_owner.StateOwner())
            Check(sound.BvolSets() == 0 && sound.FmvModelPops() == 0,
                  "owned Movie gain and PCM never execute retained register/input operations");
        Check(retained_file_calls == 0, "selected file callbacks never fall back to the previous direct file adapter");
        Check(retained_demux_calls == 0 && retained_registration_calls == 0,
              "selected input skips the retained demux and registration adapters");
        const std::string census = "," + movie.Census() + ",";
        for (const char* field : {"audio_original_calls", "stream_original_calls", "input_original_calls"}) {
            Check(census.find(std::string(",") + field + "=0,") != std::string::npos,
                  "selected service counter records zero actual captured-body invocations");
        }
        for (uint8_t command = 0; command < 16; ++command)
            Check(ipu.CommandCount(command) == 0, "movie services issue no IPU command");
    }
};

void TestLifecycle(const std::string& root) {
    TestMovieDecoderServices r(root);
    for (const char* name : {"VIVENDI.PSS", "TTIDENT.PSS"}) {
        r.Start(name);
        const uint64_t before_pcm = r.pcm.Pushed();
        const size_t before_parks = r.parks.size();
        const unsigned before_presents = r.presents, before_pumps = r.pumps;
        Check(r.memory.Read32(r.kDecoder + 0x94) != r.memory.Read32(r.kDecoder + 0x98),
              "two distinct owned picture planes");
        r.Prime();
        r.FeedAudio();
        r.RefillToEof();
        r.FeedAudio();
        Check(r.Finished(), "initial audio-fill exits at file EOF");
        Check(r.bindings.HasPendingPlayback(r.kDecoder) && r.pcm.Queued() != 0,
              "initial-fill EOF retains real PCM and accepted pictures");
        r.memory.Write32(Movie::kSoundStarted, 1);
        r.Volume(1.0f);
        Check(!r.Finished(), "same EOF defers to picture drain after sound starts");
        Check(r.pcm.Queued() != 0 && r.pcm.GateIsOpen(), "real PCM populated and presentation-gated open");
        std::array<int16_t, 1600> samples{};
        Check(r.pcm.Pop(samples.data(), 800) > 0, "field-clocked mixer can consume audio during movie");
        unsigned served = 2;
        for (; served < 1000 && !r.Finished(); ++served) {
            r.movie.DecodePicture(r.context);
            Check(r.Result() == 1, "accepted video backlog serves continuously to true EOF");
            r.FeedAudio();
        }
        Check(served == (name[0] == 'V' ? 201u : 346u), "OLD intro picture windows remain 201/346");
        Check(!r.api.FmvActive() && !r.movie.CanCheckpoint(),
              "provider EOF alone cannot permit a save while the player still owns buffers");
        Check(r.presents - before_presents == served && r.pumps - before_pumps == served,
              "one timer pump and presentation callback per delivered picture");
        Check(r.parks.size() > before_parks, "post-prime serves use production pacer");
        Check(r.pcm.PushedNonZero() != 0 && r.pcm.Dropped() == 0, "PCM has nonzero samples without overflow");
        std::printf("%s pictures=%u pcm_frames=%llu queued_before_close=%u parks=%zu\n", name, served,
                    static_cast<unsigned long long>(r.pcm.Pushed() - before_pcm), r.pcm.Queued(),
                    r.parks.size() - before_parks);
        Check(r.pcm.Queued() > 0, "fixture retains PCM to prove close cancellation");
        r.Close();
        const uint64_t flushed = r.pcm.Flushes();
        r.movie.Close(r.context);
        Check(r.pcm.Flushes() == flushed, "duplicate close is idempotent");
        r.movie.DecodePicture(r.context);
        Check(r.Result() == 0, "closed descriptor cannot serve");
    }
    r.Start("VIVENDI.PSS");
    r.Prime();
    Check(r.pcm.GateIsOpen() && r.pcm.Queued() != 0, "reopened movie has fresh PCM");
    r.Close();
    r.Start("VIVENDI.PSS");
    r.Prime();
    r.Close();
    r.CheckNoHardware();
    Check(r.bindings.ServedPerPlayback() == "201,346,2,2", "playback census closes each movie once");
    std::printf("Lifecycle: %s\n", r.movie.Census().c_str());
}

TestMovieDecoderServices::ClockTrace RecordClock(const std::string& root, TestMovieDecoderServices::CloseMode mode) {
    TestMovieDecoderServices r(root, mode);
    r.Start("VIVENDI.PSS");
    for (unsigned i = 0; i < 8; ++i) (void)r.RecordPicture();
    r.Close();
    // The input pause is identical on every arm. A carried deadline now lies
    // behind the caller: replay must catch up, not silently arm a new grid.
    r.tick += 24;
    r.Start("TTIDENT.PSS");
    TestMovieDecoderServices::ClockTrace trace;
    for (unsigned i = 0; i < 20; ++i) trace.pictures.push_back(r.RecordPicture());
    trace.pcm_frames = r.pcm.Queued();
    std::vector<int16_t> samples(size_t{trace.pcm_frames} * 2);
    Check(r.pcm.Pop(samples.data(), static_cast<int>(trace.pcm_frames)) ==
              static_cast<int>(trace.pcm_frames), "clock oracle captures the same accepted PCM");
    trace.pcm_hash = Hash(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(int16_t));
    r.Close();
    r.CheckNoHardware();
    return trace;
}

void TestClockOwnership(const std::string& root) {
    using Mode = TestMovieDecoderServices::CloseMode;
    const auto baseline = RecordClock(root, Mode::ResetOracle);
    const auto closed = RecordClock(root, Mode::Playback);
    const auto destroyed = RecordClock(root, Mode::Destroy);
    const auto initialized = RecordClock(root, Mode::NewSession);
    const auto restored = RecordClock(root, Mode::RestoredSession);
    Check(closed.pictures == baseline.pictures, "Close/reopen matches Reset/reopen at every picture, tick and park");
    Check(closed.pcm_frames == baseline.pcm_frames && closed.pcm_hash == baseline.pcm_hash,
          "same-input Close and Reset deliver identical PCM");
    Check(baseline.pictures[0].wait == 0 && baseline.pictures[1].wait == 0 &&
          baseline.pictures[2].wait == 0 && baseline.pictures[3].wait == 0,
          "two-picture prime then late catch-up preserves the carried clock");
    Check(destroyed.pictures != baseline.pictures && destroyed.pictures[3].wait > 0,
          "regression control: Destroy/reopen would incorrectly re-arm movie delivery");
    Check(initialized.pictures == destroyed.pictures,
          "explicit new session resets the delivery clock, unlike movie Close");
    Check(restored.pictures == initialized.pictures && restored.pcm_hash == initialized.pcm_hash &&
          restored.pcm_frames == initialized.pcm_frames,
          "non-movie restore discards prior software session and clock exactly like fresh initialization");
    Check(destroyed.pcm_frames == baseline.pcm_frames && destroyed.pcm_hash == baseline.pcm_hash,
          "clock regression control has identical source audio");
    for (size_t i = 0; i < baseline.pictures.size(); ++i)
        Check(destroyed.pictures[i].picture == baseline.pictures[i].picture,
              "clock regression control has identical decoded pictures");
    std::printf("Clock oracle: 20 picture/clock keys and PCM identical; Destroy control diverges at serve 4.\n");
}

void TestClosedClockMigration(const std::string& root) {
    // OLD R136/R156 and the source Close-vs-Reset regression: compare a
    // continued session with capture/restore on the same and a fresh origin.
    // Input, decoded bytes and audio remain real production providers.
    const auto record = [&](bool restore, int64_t origin) {
        TestMovieDecoderServices r(root, TestMovieDecoderServices::CloseMode::Playback, true, -1, true);
        ps2r::app::MovieCheckpoint owner(r.movie, r.file_owner, r.api, [&r] { return r.tick; },
                                        r.sound_owner.StateOwner());
        r.Start("VIVENDI.PSS");
        Check(!r.movie.CanCheckpoint(), "player refuses capture before the first software picture");
        r.Prime();
        std::vector<MpegApi::IdleContext> saved;
        std::string error;
        const auto pcm_before = r.pcm.Queued();
        Check(!r.api.CaptureIdle(r.tick, saved, &error) && saved.empty() &&
              r.pcm.Queued() == pcm_before, "active software capture refuses without discarding PCM");
        for (unsigned i = 0; i < 6; ++i) (void)r.RecordPicture();
        r.Close();
        r.tick += 24;
        Check(r.api.CaptureIdle(r.tick, saved, &error) && saved.size() == 1 &&
              saved[0].clock.armed && saved[0].clock.ticks_to_next < 0,
              "closed movie captures its carried late clock");
        ps2r::app::MovieCheckpoint::Snapshot snapshot;
        ps2r::savestate::Blob blob{ps2r::app::MovieCheckpoint::kTag, ps2r::app::MovieCheckpoint::kVersion,
                                  ps2r::savestate::kSectionRequired, {}};
        Check(owner.Capture(snapshot, &error) && snapshot.stream.owned &&
              ps2r::app::MovieCheckpoint::Encode(snapshot, blob.payload, &error),
              "production checkpoint captures closed decoder AND the selected file cursor");
        const auto before = r.memory.Bytes();
        if (restore) {
            r.movie.ResetAfterRestore(); // existing host reset, followed by owned migration
            r.file_owner.ResetAfterRestore();
            r.tick = origin;
            const auto closed_flushes = r.pcm.Flushes();
            Check(ps2r::app::MovieCheckpoint::Decode(blob, snapshot, &error) && owner.Restore(snapshot, &error),
                  "closed owner state restores on chosen origin");
            Check(r.pcm.Flushes() == closed_flushes,
                  "reconstructing a closed software decoder never executes another PCM Close");
            Check(r.file_owner.OwnsCursor() && !r.file_owner.Active() && owner.Restored() == 1,
                  "restored closed cursor still refuses stale retained reads");
            Check(r.memory.Bytes() == before && !r.pcm.Queued() && !r.pcm.GateIsOpen(),
                  "software clock migration changes no guest bytes and leaves PCM closed");
        }
        const int64_t start = r.tick;
        r.Start("TTIDENT.PSS");
        TestMovieDecoderServices::ClockTrace trace;
        for (unsigned i = 0; i < 24; ++i) {
            auto picture = r.RecordPicture();
            picture.arrival -= start;
            trace.pictures.push_back(picture);
        }
        trace.pcm_frames = r.pcm.Queued();
        std::vector<int16_t> pcm(size_t(trace.pcm_frames) * 2);
        Check(r.pcm.Pop(pcm.data(), int(trace.pcm_frames)) == int(trace.pcm_frames),
              "restored movie delivers real source PCM");
        trace.pcm_hash = Hash(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
        r.Close();
        r.CheckNoHardware();
        return trace;
    };
    const auto continuous = record(false, 0);
    for (const int64_t origin : {int64_t{0}, int64_t{100}, int64_t{10000}}) {
        const auto resumed = record(true, origin);
        Check(resumed.pictures == continuous.pictures &&
              resumed.pcm_frames == continuous.pcm_frames && resumed.pcm_hash == continuous.pcm_hash,
              "capture/restore preserves every subsequent picture/clock/PCM key across reopen");
    }
    TestMovieDecoderServices r(root);
    r.Start("VIVENDI.PSS");
    r.Prime();
    r.Close();
    std::vector<MpegApi::IdleContext> saved;
    std::string error;
    Check(r.api.CaptureIdle(r.tick, saved, &error), "closed decoder capture is reusable");
    const auto memory = r.memory.Bytes();
    const auto flushes = r.pcm.Flushes();
    for (unsigned invalid = 0; invalid != 8; ++invalid) {
        auto corrupt = saved;
        if (invalid == 0) corrupt.push_back(corrupt.front());
        if (invalid == 1) corrupt[0].address = 0;
        if (invalid == 2) corrupt[0].address = UINT32_MAX;
        if (invalid == 3) corrupt[0].context_address++;
        if (invalid == 4) corrupt[0].width = 0;
        if (invalid == 5) corrupt[0].authored_fps = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 6) corrupt[0].clock.reanchor_periods = -1;
        if (invalid == 7) corrupt[0].clock.period = 2;
        Check(!r.api.RestoreIdle(corrupt, r.tick, &error) && r.pcm.Flushes() == flushes &&
              r.memory.Bytes() == memory, "malformed software state rejects before mutation");
    }
    r.Start("VIVENDI.PSS");
    r.Prime();
    const auto queued = r.pcm.Queued();
    Check(!r.api.RestoreIdle(saved, r.tick, &error) && r.pcm.Queued() == queued,
          "live software input cannot be overwritten by a closed checkpoint");
    r.Close();
    std::printf("Closed clock migration: 24 subsequent picture/clock keys and PCM exact at three restored origins.\n");
}

void TestAudioIntent(const std::string& root) {
    TestMovieDecoderServices r(root);
    r.Start("VIVENDI.PSS");
    r.Prime();
    r.RefillToEof();
    const uint32_t count = r.pcm.Queued();
    std::vector<int16_t> decoded(size_t{count} * 2);
    Check(r.pcm.Pop(decoded.data(), static_cast<int>(count)) == static_cast<int>(count),
          "gain oracle uses PCM decoded from the same supplied movie");
    r.Close();
    const auto first = std::find_if(decoded.begin(), decoded.end(), [](int16_t sample) { return sample != 0; });
    const size_t offset = static_cast<size_t>(first - decoded.begin()) & ~size_t{1};
    Check(first != decoded.end() && offset + 512 <= decoded.size(), "gain oracle has a nonzero stereo input window");

    using namespace ps2r::sound;
    using namespace ps2r::sound::hle;
    SpuCore oracle;
    oracle.SdInit();
    FmvPcmInput oracle_pcm(1024);
    oracle.SetFmvInput(&oracle_pcm);
    SdBlockTrans block(8, [] { return uint64_t{0}; });
    SdrRemote original(oracle, block, {}, {});
    constexpr std::array<float, 4> volumes = {0.0f, 0.5f, 1.0f, 0.25f};
    constexpr std::array<uint32_t, 4> gains = {0, 16383, 32767, 8191};
    for (size_t i = 0; i < volumes.size(); ++i) {
        original.Dispatch(SdrRemote::kSetParam, 0xf80, gains[i], 0, 0);
        original.Dispatch(SdrRemote::kSetParam, 0x1080, gains[i], 0, 0);
        r.Volume(volumes[i]);
        r.pcm.Push(decoded.data() + offset, 256);
        oracle_pcm.Push(decoded.data() + offset, 256);
        r.pcm.GateOpen();
        oracle_pcm.GateOpen();
        const uint64_t before = r.pcm.Consumed();
        std::array<float, 512> expected{}, actual{};
        oracle.MixInto(expected.data(), 256);
        r.mixer.Pump(256);
        std::copy_n(r.mixer.Output(), actual.size(), actual.begin());
        Check(actual == expected, "movie volume adapter and replaced sound command give identical mixed samples");
        Check(r.pcm.Consumed() == before + 256 && r.pcm.Queued() == 0,
              "muting still consumes PCM at mixer time, without IOP status polling");
        if (gains[i] == 0)
            Check(std::all_of(actual.begin(), actual.end(), [](float s) { return s == 0.0f; }), "zero gain is silent");
        else
            Check(std::any_of(actual.begin(), actual.end(), [](float s) { return s != 0.0f; }), "gain oracle is nonvacuous");
    }
    Check(original.Calls() == 8 && r.retired_audio.Calls() == 0, "Audio intent bypasses the retired libsdr command model");
    r.CheckNoHardware();
}

TestMovieDecoderServices::ProviderTrace RecordProvider(
    const std::string& root, bool selected, bool field_audio = true, bool owned_file = false) {
    using Mode = TestMovieDecoderServices::CloseMode;
    TestMovieDecoderServices r(root, selected ? Mode::Playback : Mode::ResetOracle, selected, -1, owned_file, selected);
    r.field_audio = field_audio;
    r.api.SetPacerReanchorPeriods(4); // root's play-mode provider policy
    const auto volume = [&r, selected](float value) {
        if (selected) {
            r.Volume(value);
        } else {
            const auto gain = static_cast<uint32_t>(value * 32767.0f);
            using Remote = ps2r::sound::hle::SdrRemote;
            r.retired_audio.Dispatch(Remote::kSetParam, 0xf80, gain, 0, 0);
            r.retired_audio.Dispatch(Remote::kSetParam, 0x1080, gain, 0, 0);
        }
    };
    TestMovieDecoderServices::ProviderTrace trace;
    for (const char* name : {"VIVENDI.PSS", "TTIDENT.PSS"}) {
        r.Start(name);
        r.Prime();
        r.RefillToEof();
        Check(r.Finished() && r.pcm.Queued() > 0, "provider audio fill reaches EOF with PCM retained");
        r.memory.Write32(Movie::kSoundStarted, 1);
        volume(1.0f);
        const uint64_t consumed_at_start = r.pcm.Consumed();
        const uint64_t flushed_at_start = r.pcm.Flushes();
        unsigned pictures = 2;
        for (; pictures < 1000 && !r.Finished(); ++pictures) {
            // Same NEW input schedule in both arms: a long decode/provider
            // stall, then mute and unmute while delivery remains active.
            if (pictures == 80) r.AdvanceFields(24);
            if (pictures == 100) volume(0.0f);
            if (pictures == 108) volume(0.5f);
            trace.pictures.push_back(r.RecordPicture());
            r.FeedAudio();
        }
        Check(pictures == (name[0] == 'V' ? 201u : 346u), "provider reaches complete movie EOF");
        Check(r.pcm.Flushes() == flushed_at_start, "refill and picture EOF never flush playing audio");
        if (field_audio) {
            Check(r.pcm.Consumed() > consumed_at_start, "field provider consumes PCM before Close");
            const auto muted = std::find_if(r.fields.begin(), r.fields.end(),
                [](const auto& f) { return f.consumed != 0 && !f.audible; });
            Check(muted != r.fields.end(), "muted field consumption remains observable");
        } else {
            Check(r.pcm.Consumed() == consumed_at_start,
                  "BUG-079 control defers every PCM sample until after the movie");
        }
        volume(0.0f); // actual player's stop order, before decoder Close
        r.Close();
        const uint64_t consumed_at_close = r.pcm.Consumed();
        const size_t before = r.fields.size();
        r.AdvanceFields(24);
        Check(r.pcm.Consumed() == consumed_at_close, "closed playback has no deferred PCM tail");
        for (size_t i = before; i < r.fields.size(); ++i)
            Check(!r.fields[i].audible && r.fields[i].consumed == 0, "inter-movie fields contain no stale sound");
    }
    // Skip after the next movie's two primes, while real audio is queued.
    r.Start("VIVENDI.PSS");
    r.Prime();
    Check(r.pcm.Queued() > 0, "skip fixture owns accepted PCM");
    volume(1.0f);
    r.AdvanceFields(1);
    volume(0.0f);
    r.Close();
    const uint64_t consumed_at_skip = r.pcm.Consumed();
    r.AdvanceFields(4);
    Check(r.pcm.Consumed() == consumed_at_skip, "skip cancels PCM before another provider field");
    trace.fields = r.fields;
    trace.consumed = r.pcm.Consumed();
    trace.reanchors = r.api.PacerReanchors();
    if (selected) {
        r.CheckNoHardware();
        if (owned_file) {
            const auto c = r.file_owner.Counts();
            Check(c.opened == 3 && c.closed == 3 && c.sectors > 0 &&
                  c.original_calls == std::array<uint64_t, 3>{},
                  "actual provider uses the owned file cursor throughout complete movies and abort/reopen");
        }
    } else {
        Check(r.retired_audio.Calls() == 20, "oracle actually uses the replaced gain-command path");
        Check(r.retained_file_calls > 0, "oracle actually uses the previous direct file adapter");
        Check(r.retained_demux_calls == 3 && r.retained_registration_calls == 6,
              "oracle reaches the retained initial demux and both registrations for every Start");
    }
    return trace;
}

void TestProviderClock(const std::string& root) {
    const auto baseline = RecordProvider(root, false);
    const auto selected = RecordProvider(root, true);
    const auto boundary_only = RecordProvider(root, true, false);
    const auto owned_file = RecordProvider(root, true, true, true);
    Check(selected.pictures == baseline.pictures, "play-provider pictures, arrivals and parks match Reset oracle");
    Check(selected.fields == baseline.fields && selected.consumed == baseline.consumed,
          "each field's mixed audio and consumption matches the replaced-model gain oracle");
    Check(selected.reanchors > 0 && selected.reanchors == baseline.reanchors,
          "provider lag exercises the root's four-period reanchor policy identically");
    const auto audible = std::count_if(selected.fields.begin(), selected.fields.end(),
        [](const auto& f) { return f.audible; });
    Check(audible > 100 && selected.consumed > 0, "live-provider audio comparison is nonvacuously audible");
    Check(boundary_only.fields.empty() && boundary_only.consumed == 0,
          "BUG-079 control detects disabled field mixing even when picture delivery matches");
    Check(boundary_only.pictures == selected.pictures, "field-audio control keeps video inputs and clock unchanged");
    Check(owned_file.pictures == selected.pictures && owned_file.fields == selected.fields &&
          owned_file.consumed == selected.consumed && owned_file.reanchors == selected.reanchors,
          "owned file cursor preserves real provider picture/PCM/clock keys through EOF, abort and reopen");
    std::printf("Provider oracle: %zu picture keys, %zu field audio keys, %lld audible fields, "
                "%llu PCM consumed; boundary-only control consumes zero.\n",
                selected.pictures.size(), selected.fields.size(), static_cast<long long>(audible),
                static_cast<unsigned long long>(selected.consumed));
    std::printf("Owned file provider: all %zu picture and %zu audio field keys exact; EOF and abort/reopen passed.\n",
                owned_file.pictures.size(), owned_file.fields.size());
}

void TestAudioPlayback(const std::string& root) {
    struct Trace {
        std::vector<TestMovieDecoderServices::Delivery> pictures;
        std::vector<TestMovieDecoderServices::Field> fields;
        uint64_t consumed = 0, commands = 0, flushes = 0;
    };
    const auto run = [&](bool selected) {
        TestMovieDecoderServices r(root, TestMovieDecoderServices::CloseMode::Playback, true, selected ? 1 : 0);
        r.field_audio = true;
        Trace trace;
        for (const char* file : {"VIVENDI.PSS", "TTIDENT.PSS", "VIVENDI.PSS"}) {
            r.Start(file);
            r.Prime();
            r.RefillToEof();
            const uint64_t flushes = r.pcm.Flushes(), pushed = r.pcm.Pushed(), consumed = r.pcm.Consumed();
            // Exact source start order: acknowledge loop playback, then gain,
            // then publish started. Neither command delivers PCM a second time.
            r.SoundCommand(true);
            Check(r.pcm.Flushes() == flushes && r.pcm.Pushed() == pushed && r.pcm.Consumed() == consumed,
                  "starting output neither flushes, duplicates nor consumes movie PCM");
            r.Volume(1.0f);
            r.memory.Write32(Movie::kSoundStarted, 1);
            bool active = false; uint32_t half = 0; uint64_t elapsed = 0;
            r.retired_block.Export(0, active, half, elapsed);
            Check(active == !selected, "retained arm actually started the block model; selected did not");
            for (unsigned i = 0; i < 12; ++i) {
                if (i == 5) r.AdvanceFields(7);
                trace.pictures.push_back(r.RecordPicture());
                r.FeedAudio();
            }
            r.Volume(0.0f); // source mute before ring stop, decoder Close later
            const uint64_t before_stop = r.pcm.Flushes();
            r.SoundCommand(false);
            Check(r.pcm.Flushes() == before_stop, "stop acknowledges intent without an early PCM flush");
            r.retired_block.Export(0, active, half, elapsed);
            Check(!active, "old control stopped its block; selection never armed it");
            r.AdvanceFields(3);
            r.Close();
            Check(r.movie.CanCheckpoint(), "Close releases audio and movie ownership for checkpoint");
        }
        trace.fields = r.fields; trace.consumed = r.pcm.Consumed(); trace.flushes = r.pcm.Flushes();
        trace.commands = r.retired_audio.Commands(ps2r::sound::hle::SdrRemote::CmdIndex(0x80e0));
        Check(trace.commands == (selected ? 0u : 6u),
              "actual SdrRemote command increment proves nonvacuous old block dispatch vs zero");
        if (selected) {
            const auto census = r.movie.Census();
            Check(census.find("audio_playback_original_calls=0") != std::string::npos &&
                  census.find("audio_started=3") != std::string::npos &&
                  census.find("audio_stopped=3") != std::string::npos,
                  "movie owner acknowledged all three starts/stops without the captured SDK service");
        }
        return trace;
    };
    const auto model = run(false), selected = run(true);
    Check(model.pictures == selected.pictures && model.fields == selected.fields &&
          model.consumed == selected.consumed && model.flushes == selected.flushes,
          "ring retirement preserves actual software picture/field PCM bytes, clock and abort/reopen lifetime");
    const auto audible = std::count_if(selected.fields.begin(), selected.fields.end(),
                                      [](const auto& f) { return f.audible; });
    Check(audible > 10 && selected.consumed > 0, "ring retirement oracle has real audible PCM work");

    TestMovieDecoderServices foreign(root, TestMovieDecoderServices::CloseMode::Playback, true, 1);
    foreign.Start("VIVENDI.PSS");
    foreign.Prime();
    // A foreign caller's start must not be swallowed; its later stop must
    // still clean up that actual old block even at the known movie stop site.
    foreign.SoundCommand(true, 0x1234);
    foreign.SoundCommand(false);
    Check(foreign.retired_audio.Commands(ps2r::sound::hle::SdrRemote::CmdIndex(0x80e0)) == 2,
          "caller guard preserves both foreign start and its real-model stop");
    foreign.memory.Write32(TestMovieDecoderServices::kDecoder + 0x110, 1024);
    foreign.SoundCommand(true);
    Check(foreign.retired_audio.Commands(ps2r::sound::hle::SdrRemote::CmdIndex(0x80e0)) == 3,
          "nonempty legacy audio ring retains its original consumer");
    foreign.SoundCommand(false);
    foreign.memory.Write32(TestMovieDecoderServices::kDecoder + 0x110, 0);
    const auto before_takeover = foreign.retired_audio.Commands(ps2r::sound::hle::SdrRemote::CmdIndex(0x80e0));
    foreign.SoundCommand(true); // selected playback first
    foreign.SoundCommand(true, 0x1234); // a real consumer takes over
    foreign.SoundCommand(false); // movie stop must stop that real consumer
    bool active = false; uint32_t half = 0; uint64_t elapsed = 0;
    foreign.retired_block.Export(0, active, half, elapsed);
    Check(!active &&
          foreign.retired_audio.Commands(ps2r::sound::hle::SdrRemote::CmdIndex(0x80e0)) == before_takeover + 2,
          "foreign takeover invalidates output ownership and preserves the real-model stop");
    foreign.Close();
    std::printf("Audio playback oracle: %zu pictures, %zu field keys, %lld audible fields; actual block commands 6/0\n",
                selected.pictures.size(), selected.fields.size(), static_cast<long long>(audible));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: test_movie_decoder_services <game-files-directory>\n");
        return 2;
    }
    const std::string root = argc == 2 ? argv[1] : "";
    for (const char* name : {"VIVENDI.PSS", "TTIDENT.PSS"}) {
        if (root.empty() || !std::ifstream(root + "/FMV/" + name, std::ios::binary)) {
            std::printf("SKIP: MovieDecoderServices needs FMV/VIVENDI.PSS and FMV/TTIDENT.PSS under its input directory\n");
            return 77;
        }
    }
    TestLifecycle(root);
    TestClockOwnership(root);
    TestClosedClockMigration(root);
    TestAudioIntent(root);
    TestProviderClock(root);
    TestAudioPlayback(root);
    std::printf("MovieDecoderServices: %u checks, all passed\n", checks);
    return 0;
}
