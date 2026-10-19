// Actual retained NEW lifecycle vs MovieDecoder's seven-entry selection.
// The same public three picture bindings, software MPEG/PCM and file cursor
// serve both arms. Generated Init reaches real SDK IPU dispatch (four BCLR).
// No game process, console capture, or newly implemented hardware behavior.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_oracle_args.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_sound.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/libipu_api.h"
#include "engine/ipu/hle/libipu_bindings.h"
#include "engine/ipu/hle/libmpeg_bindings.h"
#include "engine/ipu/hle/mpeg_api.h"
#include "engine/sound/fmv_pcm_input.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/fmv_abort_hold.h"
#include "tests/ipu/recording_dmac_channel_face.h"

namespace {
using Movie = title::MovieDecoder;
using namespace ps2r::ipu;
using Bytes = std::vector<uint8_t>;
unsigned checks = 0;
void Check(bool value, const char* what) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL MovieLifecycleOracle: %s\n", what); std::exit(1); }
}
Bytes Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(file.good() && file.tellg() > 0, "owned input file is readable and nonempty");
    Bytes data(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    Check(file.good(), "owned input read is complete");
    return data;
}
void Append(Bytes& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
void Word(Bytes& out, uint64_t value) {
    for (unsigned n = 0; n < 8; ++n) out.push_back(static_cast<uint8_t>(value >> (8 * n)));
}
uint32_t At(const Bytes& bytes, size_t offset) {
    Check(offset <= bytes.size() && 4 <= bytes.size() - offset, "ELF field is inside owned input");
    return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
           (uint32_t{bytes[offset + 2]} << 16) | (uint32_t{bytes[offset + 3]} << 24);
}
struct Inputs { Bytes elf, vivendi, ttident; };
struct Trace {
    Bytes input, picture, pcm, clock;
    std::array<uint64_t, 16> ipu{};
    LibipuApi::Operations operations;
    std::map<uint32_t, uint64_t> generated;
    std::string movie;
    uint64_t audible = 0, consumed = 0, abort_ticks = 0;
    uint64_t model_pcm_pops = 0, model_gain_sets = 0, owned_pcm_frames = 0, owned_pcm_pops = 0, owned_gain_changes = 0;
};

class TestMovieDecoderOracle final : public GuestMemory, public FmvSectorSource {
public:
    static constexpr uint32_t kDecoder = 0x10000, kStack = 0x1f00000, kArena = 0x400000;
    static constexpr uint32_t kGp = 0x3114f0;
    const Inputs& inputs;
    bool selected;
    Bytes ram = Bytes(32u << 20);
    const Bytes* file = nullptr;
    size_t cursor = 0;
    Trace trace;
    std::map<uint32_t, uint32_t> transport_words;
    RecordingDmacChannelFace dmac;
    IpuDevice ipu;
    LibipuApi libipu{ipu, dmac};
    LibipuBindings ipu_bindings{libipu, *this};
    ps2r::sound::FmvPcmInput pcm;
    ps2r::sound::SpuCore sound;
    ps2r::sound::Mixer mixer{sound};
    ps2r::app::MovieSound sound_owner{selected, pcm, sound, mixer};
    FmvContextLayout layout = FmvContextLayoutData::CrashTwinsanityUS();
    IpuSettings settings;
    MpegApi api{*this, layout, *this, pcm,
                [] { return std::make_unique<FfmpegVideoDecoder>(); }, settings};
    int64_t tick = 1000, mixed_tick = 1000;
    LibmpegBindings bindings{api, *this, layout, {
        [](CpuContext&) {}, [this] { return tick; },
        [this](int64_t count) { Fields(count); },
        [this](bool no_park) { if (no_park) Fields(0); },
    }, selected ? LibmpegBindings::RetainedInit{} : LibmpegBindings::RetainedInitialization(libipu, dmac)};
    FmvAbortHold abort_hold{*this, layout, [this](uint32_t decoder) { return api.ShouldHoldAbort(decoder); }};
    Movie movie{*this, {
        [this](CpuContext& c) { bindings.SceMpegCreate(c); },
        [this](CpuContext& c) { bindings.SceMpegReset(c); },
        [this](CpuContext& c) { bindings.SceMpegGetPicture(c); },
        [this](CpuContext& c) { bindings.SceMpegGetPictureRaw8(c); },
        [this] { bindings.InitializePlayback(); },
        [this](uint32_t decoder) { bindings.ClosePlayback(decoder); },
        [this](CpuContext& c, uint32_t pc) { Dispatch(pc, c); },
        [this](uint32_t decoder) { return bindings.HasPendingPlayback(decoder); },
    }};
    CpuContext context{};
    std::map<uint32_t, Movie::Body> installed;

    TestMovieDecoderOracle(const Inputs& data, bool replacement) : inputs(data), selected(replacement) {
        Check(inputs.elf.size() > 52 && At(inputs.elf, 0) == 0x464c457f, "owned NTSC input is ELF");
        const uint32_t ph = At(inputs.elf, 28), sizes = At(inputs.elf, 40), counts = At(inputs.elf, 44);
        const uint32_t stride = sizes >> 16, count = counts & 0xffff;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t row = ph + size_t{i} * stride;
            if (At(inputs.elf, row) != 1) continue;
            const uint32_t offset = At(inputs.elf, row + 4), addr = At(inputs.elf, row + 8);
            const uint32_t size = At(inputs.elf, row + 16);
            Check(uint64_t{offset} + size <= inputs.elf.size() && Translate(addr, size), "ELF segment is backed");
            std::memcpy(Translate(addr, size), inputs.elf.data() + offset, size);
        }
        // Owned bundle includes the exact ELF/table bytes and both file
        // providers. Per-read byte spans are appended as they are consumed.
        for (const Bytes* bytes : {&inputs.elf, &inputs.vivendi, &inputs.ttident}) {
            Word(trace.input, bytes->size());
            Append(trace.input, bytes->data(), bytes->size());
        }
        context.cpuRegs.GPR.r[28].UD[0] = kGp;
        context.cpuRegs.GPR.r[29].UD[0] = kStack;
        const auto capture = [](uint32_t address) -> Movie::Body {
            return [address](CpuContext& c, uint32_t entry) {
                Check(entry == 0 || entry == address, "oracle uses complete lifecycle entries");
                Check(movie_oracle::RunGenerated(address, c), "captured original exists in NEW source");
            };
        };
        const auto install = [this](uint32_t address, Movie::Body body) {
            installed[address] = std::move(body);
            return true;
        };
        // The sole replacement selection. Stream/Audio/Input opt-ins are off
        // on both sides: this fixture proves the live seven-entry lifecycle.
        Check(movie.Install(capture, install, selected) == (selected ? 7 : 3),
              "one selection chooses retained NEW lifecycle or the seven-entry owner");
        sound.SdInit();
    }

    uint8_t* Translate(uint32_t address, size_t size) override {
        return address <= ram.size() && size <= ram.size() - address ? ram.data() + address : nullptr;
    }
    uint32_t Read32(uint32_t address) override {
        uint32_t value;
        Access(address, &value, sizeof(value), false);
        return value;
    }
    void Write32(uint32_t address, uint32_t value) override { Access(address, &value, sizeof(value), true); }
    void Access(uint32_t address, void* data, size_t size, bool write) {
        address &= 0x1fffffffu;
        if (address >= 0x10002000 && address < 0x10002040) {
            Check(size == 4, "IPU oracle MMIO is a word");
            uint32_t value = 0;
            if (write) { std::memcpy(&value, data, 4); ipu.Write(address, 4, value); }
            else { value = static_cast<uint32_t>(ipu.Read(address, 4)); std::memcpy(data, &value, 4); }
            return;
        }
        if (address >= 0x10000000) {
            // The source brackets Start with DMA disable/reset and initializes
            // its software channel descriptors. These zero/reset stores are
            // recorded, not claimed as a DMAC retirement oracle.
            Check(size == 4 && (address == 0x1000f590 || address == 0x1000b000 ||
                  address == 0x1000b020 || address == 0x1000b400 || address == 0x1000b420 ||
                  address == 0x1000e030), "only known inert transport initialization words are present");
            if (write) std::memcpy(&transport_words[address], data, 4);
            else std::memcpy(data, &transport_words[address], 4);
            return;
        }
        uint8_t* p = Translate(address, size);
        Check(p != nullptr, "generated movie body remains inside mapped RAM");
        if (write) std::memcpy(p, data, size); else std::memcpy(data, p, size);
    }
    uint32_t PumpSectors(uint8_t* out, uint32_t sectors) override {
        Check(file != nullptr, "read-ahead owns an open file");
        const size_t bytes = std::min(size_t{sectors} * 2048, file->size() - cursor);
        if (!bytes) return 0;
        const uint32_t got = static_cast<uint32_t>((bytes + 2047) / 2048);
        std::fill_n(out, size_t{got} * 2048, 0);
        std::memcpy(out, file->data() + cursor, bytes);
        // Record bytes actually supplied, including the shared final-sector
        // padding, without imposing the caller's read chunk boundaries.
        Append(trace.input, out, size_t{got} * 2048);
        cursor += bytes;
        return got;
    }
    void Dispatch(uint32_t address, CpuContext& c) {
        if (const auto it = installed.find(address); it != installed.end()) {
            it->second(c, address);
            return;
        }
        if (movie_oracle::RunGenerated(address, c)) return;
        const auto arg = [&c](unsigned i) { return c.cpuRegs.GPR.r[4 + i].UL[0]; };
        switch (address) {
        case 0x2c13c0: ipu_bindings.SceIpuInit(c); return;
        case 0x2b7b50: bindings.SceMpegInit(c); return;
        case 0x2bbe90: bindings.SceMpegDemuxPss(c); return;
        case 0x2c10a8: ipu_bindings.SceIpuStopDma(c); return;
        case Movie::kSeekStream: cursor = 0; c.cpuRegs.GPR.r[2].SD[0] = 1; return;
        case Movie::kReadStream: {
            uint8_t* out = Translate(arg(0), arg(1));
            Check(out != nullptr && arg(1) == 0x4000, "retained refill requests its exact input window");
            const uint32_t got = PumpSectors(out, arg(1) / 2048);
            c.cpuRegs.GPR.r[2].SD[0] = got ? static_cast<int32_t>(got * 2048) : -1;
            return;
        }
        case 0x2b1448:
            // Shared picture upload consumer is deliberately outside this
            // decoder oracle; observable planes are read before presentation.
            Check(Translate(arg(0), 8) != nullptr, "upload descriptor is backed");
            Write32(arg(0), 0x12345678);
            return;
        case 0x2d4dc0: // DIntr/EIntr and SetCP state/cache services have no
        case 0x2d4e10: // concurrent guest executor in this composition fixture.
        case 0x2cf560:
            c.cpuRegs.GPR.r[2].SD[0] = 0;
            return;
        default:
            std::fprintf(stderr, "unhandled NEW movie callee %08x\n", address);
            Check(false, "generated call closure is explicit");
        }
    }
    void Call(uint32_t address) { Dispatch(address, context); }
    int32_t Result() const { return context.cpuRegs.GPR.r[2].SL[0]; }
    void Volume(uint16_t gain) {
        Word(trace.input, 0x4d47564c); Word(trace.input, gain); // same source gain intents in both arms
        sound_owner.SetGain(gain);
    }
    void Fields(int64_t count) {
        tick += count;
        while (mixed_tick < tick) {
            ++mixed_tick;
            mixer.Pump();
            const size_t size = static_cast<size_t>(mixer.OutputFrames()) * 2 * sizeof(float);
            Append(trace.pcm, mixer.Output(), size);
            if (std::any_of(mixer.Output(), mixer.Output() + mixer.OutputFrames() * 2,
                            [](float sample) { return sample != 0; })) ++trace.audible;
        }
    }
    void Start(unsigned index) {
        file = index == 0 ? &inputs.vivendi : &inputs.ttident;
        Word(trace.clock, index);
        Write32(Movie::kArenaNext, kArena);
        Write32(Movie::kArenaEnd, 0xb00000);
        Write32(Movie::kSoundStarted, 0);
        Write32(Movie::kSoundMode, 1);
        const std::array<uint32_t, 8> args{1, kDecoder, 1, 2, 1, 0, Movie::kSeekStream, Movie::kReadStream};
        for (size_t i = 0; i < args.size(); ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        Write32(kStack, 0); Write32(kStack + 8, 1); Write32(kStack + 16, 10); Write32(kStack + 24, 0x100);
        Call(Movie::kStart);
        Check(Result() == 1, "actual lifecycle Start succeeds");
        Check(Read32(kDecoder + 0x54) == 0 && Read32(kDecoder + 0x58) == (index ? 512u : 640u),
              "actual Start publishes the expected picture contract");
    }
    void Picture() {
        const uint32_t number = Read32(kDecoder + 0x54);
        const uint32_t plane = Read32(kDecoder + ((number & 1) ? 0x98 : 0x94));
        const int64_t before = tick;
        Call(Movie::kDecodePicture);
        Check(Result() == 1, "shared public picture binding delivers real MPEG output");
        if (number < 2) Check(before == tick, "two-picture priming is unpaced on both arms");
        const uint32_t width = Read32(kDecoder + 0x58), height = Read32(kDecoder + 0x5c);
        Word(trace.picture, width); Word(trace.picture, height);
        Append(trace.picture, Translate(plane, size_t{width} * height * 4), size_t{width} * height * 4);
        Word(trace.clock, number); Word(trace.clock, static_cast<uint64_t>(before));
        Word(trace.clock, static_cast<uint64_t>(tick - before));
    }
    void Fill() {
        const auto flushes = pcm.Flushes();
        unsigned fills = 0;
        while (Read32(kDecoder + 0x15c) == 0 && fills++ < 10000) {
            context.cpuRegs.GPR.r[4].UD[0] = 0;
            Call(Movie::kDemultiplex);
        }
        Check(fills < 10000, "source EOF releases the initial sound-fill loop");
        api.NotifyCdStreamEof();
        Call(Movie::kFinished);
        Check(Result() == 1 && pcm.Queued() > 0 && pcm.Flushes() == flushes &&
              bindings.HasPendingPlayback(kDecoder), "initial EOF preserves accepted pictures and PCM");
        Write32(Movie::kSoundStarted, 1);
        if (!selected) abort_hold.Tick(); // Actual replaced NEW bug workaround.
        Call(Movie::kFinished);
        Check(Result() == 0, "same file EOF does not tear down playing video");
    }
    void Close() {
        Volume(0); // actual player's order, before lifecycle Close
        Call(Movie::kClose);
        if (selected) Check(pcm.Queued() == 0 && !pcm.GateIsOpen(), "selected Close cancels PCM immediately");
    }
    Trace Run() {
        Call(Movie::kInitialise);
        for (unsigned index = 0; index < 2; ++index) {
            Start(index);
            Picture(); Picture(); Fill(); Volume(32767);
            for (unsigned n = 2; n < (index ? 22u : 14u); ++n) {
                if (n == 8) { Volume(0); Fields(8); Volume(16383); }
                Picture();
            }
            Check(pcm.Consumed() > 0 && trace.audible > 0, "real provider fields mix audible movie PCM before Close");
            Close();
            Fields(24); // Same delayed/reopen input; retained PCM tail is muted.
        }
        Start(0); Picture(); Picture(); Volume(32767); Fields(1); Close();
        for (uint8_t cmd = 0; cmd < trace.ipu.size(); ++cmd) trace.ipu[cmd] = ipu.CommandCount(cmd);
        trace.operations = libipu.Count();
        trace.movie = movie.Census();
        trace.consumed = pcm.Consumed();
        trace.abort_ticks = abort_hold.Ticks();
        Check(trace.ipu[0] == (selected ? 0u : 4u), "actual retained initialization dispatches four BCLR; selection dispatches zero");
        for (uint8_t cmd = 1; cmd < trace.ipu.size(); ++cmd)
            Check(trace.ipu[cmd] == 0, "no unclaimed IPU command executes");
        Check(trace.operations.init == (selected ? 0u : 2u) &&
              trace.operations.stop == (selected ? 0u : 3u) &&
              trace.operations.sync == 0 && trace.operations.restart == 0,
              "actual model initialization/snapshot positive controls versus all selected operations zero");
        Check(trace.generated[Movie::kInitialise] == (selected ? 0u : 1u) &&
              trace.generated[Movie::kStart] == (selected ? 0u : 3u) &&
              trace.generated[Movie::kClose] == (selected ? 0u : 3u),
              "retained control executes the actual generated lifecycle bodies");
        trace.model_pcm_pops = sound.FmvModelPops();
        trace.model_gain_sets = sound.BvolSets();
        if (auto* owned = sound_owner.StateOwner()) {
            trace.owned_pcm_frames = owned->Count().frames;
            trace.owned_pcm_pops = owned->Count().pops;
            trace.owned_gain_changes = owned->Count().gain_changes;
        }
        Check(selected ? (trace.model_pcm_pops == 0 && trace.model_gain_sets == 0 &&
                          trace.owned_pcm_frames > 0 && trace.owned_gain_changes > 0) :
                         (trace.model_pcm_pops > 0 && trace.model_gain_sets > 0 && !trace.owned_pcm_frames),
              "actual retained PCM/gain positive controls versus owned software output");
        return std::move(trace);
    }
};
TestMovieDecoderOracle* active = nullptr;

std::string Digest(const Bytes& bytes) {
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    Check(sha && av_sha_init(sha.get(), 256) == 0, "SHA256 artifact digest initializes");
    av_sha_update(sha.get(), bytes.data(), bytes.size());
    std::array<uint8_t, 32> result{};
    av_sha_final(sha.get(), result.data());
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (uint8_t byte : result) text << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}
std::string Quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') out << '\\' << byte;
        else if (byte < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned{byte};
        else out << byte;
    }
    out << '"';
    return out.str();
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    Check(!std::filesystem::exists(path), "export cannot overwrite an earlier artifact or link");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Check(output.good() && std::filesystem::is_regular_file(path), "exported artifact is a complete regular file");
}
std::string IpuCensus(const Trace& trace) {
    constexpr std::array<const char*, 10> names{
        "bclr", "idec", "bdec", "vdec", "fdec", "setiq", "setvq", "csc", "pack", "setth"};
    std::ostringstream text;
    uint64_t unknown = 0;
    for (size_t cmd = 0; cmd < trace.ipu.size(); ++cmd) {
        if (cmd < names.size()) text << (cmd ? "," : "") << names[cmd] << '=' << trace.ipu[cmd];
        else unknown += trace.ipu[cmd];
    }
    text << ",unknown=" << unknown;
    return text.str();
}
void Export(const Trace& model, const Trace& selected, int argc, char** argv,
            const movie_oracle::ExportArgs& args) {
    if (args.directory.empty()) return;
    const auto binary = std::filesystem::canonical(argv[0]);
    const auto directory = std::filesystem::absolute(args.directory);
    const std::string binary_sha = Digest(Load(binary)),
                      source_sha = Digest(Load(args.source_manifest));
    Check(!std::filesystem::exists(directory), "use a fresh artifact directory for every oracle run");
    Check(std::filesystem::create_directories(directory), "fresh artifact directory created");
    std::ostringstream command;
    command << '[' << Quote(binary.string());
    for (int i = 1; i < argc; ++i) command << ',' << Quote(argv[i]);
    command << ']';
    std::ostringstream index;
    index << "{\n  \"schema\":\"movie-lifecycle-artifacts-v1\",\n  \"test\":\"MovieLifecycleOracle\",\n"
          << "  \"scope\":\"actual retained NEW lifecycle and PCM input; IPU.command.bclr, IPU.operation.init/stop, SPU.movie_input.pop exercised\",\n"
          << "  \"attributions\":{},\n  \"arms\":{\n";
    for (bool native : {false, true}) {
        const auto& trace = native ? selected : model;
        const std::string mode = native ? "native" : "model";
        const auto arm = directory / mode;
        Check(std::filesystem::create_directory(arm), "each arm owns independent output files");
        const std::array<std::pair<const char*, const Bytes*>, 4> channels{{
            {"input", &trace.input}, {"picture", &trace.picture}, {"pcm", &trace.pcm}, {"clock", &trace.clock}}};
        std::map<std::string, std::string> digests;
        for (const auto& channel : channels) {
            Save(arm / (std::string(channel.first) + ".bin"), *channel.second);
            digests[channel.first] = Digest(*channel.second);
        }
        const auto outputs = [&digests] {
            std::ostringstream text;
            text << '{';
            bool first = true;
            for (const char* channel : {"picture", "pcm", "clock"}) {
                text << (first ? "" : ",") << Quote(channel) << ':' << Quote(digests.at(channel));
                first = false;
            }
            text << '}';
            return text.str();
        }();
        std::ostringstream receipt;
        receipt << "{\n  \"schema\":\"retirement-oracle-run-v1\",\n  \"mode\":" << Quote(mode)
                << ",\n  \"test\":\"MovieLifecycleOracle\",\n  \"command\":" << command.str()
                << ",\n  \"binary_sha256\":" << Quote(binary_sha) << ",\n  \"source_sha256\":" << Quote(source_sha)
                << ",\n  \"input_sha256\":" << Quote(digests.at("input"))
                << ",\n  \"exit_code\":0,\n  \"completed\":true,\n"
                << "  \"settings\":{\"hle.movie_lifecycle\":" << Quote(native ? "on" : "off")
                << ",\"providers\":\"shared-software-file-mpeg-pcm\",\"optional_movie_services\":\"off\","
                << "\"clock\":\"field\",\"pcm_provider\":\"selected-by-hle.movie_lifecycle\"},\n  \"outputs\":" << outputs
                << ",\n  \"counters\":{\"ipu_commands\":" << Quote(IpuCensus(trace))
                << ",\"ipu_operations\":" << Quote("init=" + std::to_string(trace.operations.init) +
                    ",sync=" + std::to_string(trace.operations.sync) +
                    ",stop=" + std::to_string(trace.operations.stop) +
                    ",restart=" + std::to_string(trace.operations.restart))
                << ",\"movie_decoder\":" << Quote(trace.movie)
                << ",\"fmv_abort_wrapper_ticks\":" << Quote(std::to_string(trace.abort_ticks))
                << ",\"fmv_pcm\":" << Quote("audible_fields=" + std::to_string(trace.audible) +
                                          ",consumed=" + std::to_string(trace.consumed) +
                                          ",bvol_sets=" + std::to_string(trace.model_gain_sets))
                << ",\"movie_audio\":" << Quote("model_pops=" + std::to_string(trace.model_pcm_pops) +
                    ",frames=" + std::to_string(trace.owned_pcm_frames) +
                    ",pops=" + std::to_string(trace.owned_pcm_pops) +
                    ",gain_changes=" + std::to_string(trace.owned_gain_changes))
                << "},\n  \"generated_entries\":{";
        bool first = true;
        for (const auto& entry : trace.generated) {
            std::ostringstream name;
            name << "0x" << std::hex << entry.first;
            receipt << (first ? "" : ",") << Quote(name.str()) << ':' << entry.second;
            first = false;
        }
        receipt << "}\n}\n";
        const std::string json = receipt.str();
        const Bytes receipt_bytes(json.begin(), json.end());
        Save(arm / "receipt.json", receipt_bytes);
        const auto ref = [&mode](const std::string& filename, const std::string& hash) {
            return "{\"path\":" + Quote(mode + "/" + filename) + ",\"sha256\":" + Quote(hash) + "}";
        };
        index << (native ? ",\n" : "") << "    " << Quote(mode) << ":{\"input\":"
              << ref("input.bin", digests.at("input")) << ",\"receipt\":" << ref("receipt.json", Digest(receipt_bytes))
              << ",\"outputs\":{";
        first = true;
        for (const char* channel : {"picture", "pcm", "clock"}) {
            index << (first ? "" : ",") << Quote(channel) << ':'
                  << ref(std::string(channel) + ".bin", digests.at(channel));
            first = false;
        }
        index << "}}";
    }
    index << "\n  }\n}\n";
    const std::string text = index.str();
    Save(directory / "artifacts.json", Bytes(text.begin(), text.end()));
    std::printf("Artifact export: %s\n", directory.string().c_str());
}
}  // namespace

namespace movie_oracle {
std::ostringstream g_logFile;
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) {
    static const std::function<void(CpuContext&, uint32_t)> call =
        [](CpuContext& context, uint32_t address) { Dispatch(address, context); };
    return &call;
}
void Read(uint32_t address, void* destination, size_t size) {
    Check(active != nullptr, "generated transport has a live fixture");
    active->Access(address, destination, size, false);
}
void Write(uint32_t address, const void* source, size_t size) {
    Check(active != nullptr, "generated transport has a live fixture");
    active->Access(address, const_cast<void*>(source), size, true);
}
void Dispatch(uint32_t address, CpuContext& context) {
    Check(active != nullptr, "generated dispatch has a live fixture");
    active->Dispatch(address, context);
}
void ObserveGenerated(uint32_t address) {
    Check(active != nullptr, "generated entry has a live fixture");
    ++active->trace.generated[address];
}
}  // namespace movie_oracle

int main(int argc, char** argv) {
    movie_oracle::ExportArgs args;
    if (argc < 3 || std::string(argv[2]) != "--both-paths" ||
        !movie_oracle::ExportArgs::Parse(argc, argv, 3, args)) {
        std::fprintf(stderr, "usage: test_movie_decoder_oracle GAME_FILES --both-paths "
                             "[--export-dir DIR --source-manifest FILE]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    for (const char* name : {"SLUS_209.09", "FMV/VIVENDI.PSS", "FMV/TTIDENT.PSS"}) {
        if (!std::filesystem::is_regular_file(root / name)) {
            std::printf("SKIP MovieLifecycleOracle: needs owned NTSC ELF and both intro PSS files\n");
            return 77;
        }
    }
    const Inputs inputs{Load(root / "SLUS_209.09"), Load(root / "FMV/VIVENDI.PSS"), Load(root / "FMV/TTIDENT.PSS")};
    const auto run = [&inputs](bool selected) {
        TestMovieDecoderOracle fixture(inputs, selected);
        active = &fixture;
        Trace result = fixture.Run();
        active = nullptr;
        return result;
    };
    const Trace model = run(false), replacement = run(true);
    Check(model.input == replacement.input, "both arms consume the same owned input bytes");
    Check(model.picture == replacement.picture, "actual generated lifecycle and selected owner produce identical picture bytes");
    Check(model.pcm == replacement.pcm, "actual generated lifecycle and selected owner produce identical mixed PCM bytes");
    Check(model.clock == replacement.clock, "late delivery and Close/reopen preserve the source clock");
    Export(model, replacement, argc, argv, args);
    std::printf("MovieLifecycleOracle: %u checks; picture=%zu PCM=%zu clock=%zu input=%zu bytes exact; "
                "real BCLR model=%llu selected=%llu, audible fields=%llu/%llu\n",
        checks, model.picture.size(), model.pcm.size(), model.clock.size(), model.input.size(),
        static_cast<unsigned long long>(model.ipu[0]), static_cast<unsigned long long>(replacement.ipu[0]),
        static_cast<unsigned long long>(model.audible), static_cast<unsigned long long>(replacement.audible));
    return 0;
}
