// OLD FMV2 BUG-045, SOUND_P5 and FMV3: accepted backlog survives refill EOF,
// full queues do not decode, failed decode still completes its source Step,
// and reopen cannot reuse pictures. The actual NEW Step and memcpy execute
// in the control; both arms use the same production Movie owners/providers.
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_oracle_args.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include "engine/ee/cpu/guest_park.h"
#include "engine/ee/hle/kernl_boot.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/hle/syscall_table.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/gs/hle/sce/libgraph_bindings.h"
#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_step.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_start.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_wait.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stop.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

namespace {
using Bytes = std::vector<uint8_t>;
using Decoder = title::MovieDecoder;
using Pictures = title::MovieGraphics;
constexpr uint32_t player = 0x10000, decoder = player + 0x80, stack = 0x1f00000;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Word(Bytes& bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
struct Trace {
    Bytes input, picture, state;
    std::string census;
    uint64_t model_steps = 0, copies = 0, served = 0;
    uint64_t model_starts = 0;
    uint64_t model_waits = 0;
    uint64_t model_stops = 0, registrations = 0;
};

class Fixture final : public ps2r::ipu::GuestMemory {
public:
    MainMemory ram;
    Scratchpad scratch;
    AddressMap map{ram,scratch};
    MmioSink sink;
    MmioRouter router{sink};
    FastMem fast{ram,map,router};
    CpuContext context{};
    std::map<uint32_t, Decoder::Body> bindings;
    std::map<uint32_t, uint64_t> generated;
    std::unique_ptr<Decoder> movie;
    std::unique_ptr<Pictures> pictures;
    std::unique_ptr<title::MovieStep> step;
    std::unique_ptr<title::MovieStart> start;
    std::unique_ptr<title::MovieWait> wait;
    std::unique_ptr<title::MovieStop> stop;
    std::unique_ptr<title::MovieStream> stream;
    title::StreamBuffers buffers{0x100000};
    std::map<uint32_t, std::string> strings;
    Trace trace;
    bool pending = true, exhausted = false;
    bool found = true, fail_loan = false, bad_sequence = false, prime_failure = false, read_error = false;
    float volume = 0.5f;
    uint32_t file_bytes = 0x8000, demuxes = 0, lends = 0, reclaims = 0, gain = 0;
    uint32_t served = 0;
    Fixture(bool selected, bool whole_start = false, bool whole_wait = false, bool whole_stop = false) {
        whole_wait = whole_wait || whole_stop;
        if (whole_stop) {
            RecompAbi::BindMemory(&fast);
            SyscallTable::Instance().ResetForTest();
            ps2r::gs::LibgraphBindings::ResetForTest();
            ps2r::gs::LibgraphBindings::Bind(SdkBindingManifest::Instance(), nullptr, {0x180000});
        }
        auto hooks = Decoder::Hooks{};
        hooks.create = hooks.reset = [](CpuContext&) {};
        hooks.initialize = [] {};
        hooks.destroy = [](uint32_t) {};
        hooks.volume = [this](uint16_t value) { gain = value; };
        hooks.picture_rgb32 = hooks.picture_rgb16 = [](CpuContext&) {
            Check(false, "owned Step never writes a guest picture plane");
        };
        hooks.has_pending = [this](uint32_t) { return pending; };
        hooks.call = [this](CpuContext& c, uint32_t at) { Call(at, c); };
        movie = std::make_unique<Decoder>(*this, hooks);
        Check(movie->Install(Capture(), Install(), true) == 7 &&
              movie->InstallAudio(Capture(), Install()) == 2, "real lifecycle and PCM service consumers");
        Pictures::Hooks output;
        output.picture = [this](CpuContext& c, ps2r::ipu::DecodedPicture& out) {
            if (exhausted) { c.cpuRegs.GPR.r[2].SD[0] = -1; return; }
            out = {32, 16, Bytes(32 * 16 * 4)};
            for (size_t i = 0; i < out.rgba.size(); ++i) out.rgba[i] = uint8_t(i * 37 + served * 19);
            ++served;
            c.cpuRegs.GPR.r[2].SD[0] = 0;
        };
        output.present = [] { return true; };
        pictures = std::make_unique<Pictures>(*this, *movie, output, Pictures::OutputServices{
            [this](auto image) {
                Word(trace.picture, image->id);
                trace.picture.insert(trace.picture.end(), image->pixels.rgba.begin(), image->pixels.rgba.end());
                return true;
            }, [](const auto&, auto) { return true; }});
        Check(pictures->Install(Capture(), Install(), true) == 3, "same owned picture output in both Step arms");
        step = std::make_unique<title::MovieStep>(*this, *movie, *pictures);
        Check(step->Install(Capture(), Install(), whole_wait || (selected && !whole_start)),
              "one real root Step selection");
        if (whole_start || whole_wait) {
            EnableStartServices();
            start = std::make_unique<title::MovieStart>(*this, *movie, *pictures,
                [this](CpuContext& c, uint32_t at) { Call(at, c); });
            Check(start->Install(Capture(), Install(), whole_wait || selected, whole_stop && selected),
                  "complete root Start service selection");
        }
        if (whole_wait) {
            wait = std::make_unique<title::MovieWait>(*this, *movie, *pictures,
                [this](CpuContext& c, uint32_t at) { Call(at, c); });
            Check(wait->Install(Capture(), Install(), whole_stop || selected),
                  "same complete root with one WaitFrame selection");
        }
        if (whole_stop) {
            stop = std::make_unique<title::MovieStop>(*this, *movie, *pictures,
                [this](CpuContext& c, uint32_t at) { Call(at,c); });
            Check(stop->Install(Capture(), Install(), selected), "paired direct Start and complete Stop");
            Write32(0x306928, 0); Write32(0x30692c, 0x2afa00);
            Write32(0x309740, 0x180100); Write32(0x180108, 0x180200);
            Write32(0x180214, 0x8a715235);
            Write32(0x309728, 0x180300); Write32(0x18031c, 0x180400);
            for (uint32_t slot : {0x50u,0x68u,0x70u}) {
                // Explicit shared game renderer providers outside the Movie
                // service claim; their call inputs/order are observable.
                Write32(0x180400+slot, 0); Write32(0x180404+slot, 0x190000+slot);
            }
        }
        context.cpuRegs.GPR.r[29].UD[0] = stack;
        context.cpuRegs.GPR.r[28].UD[0] = 0x3114f0;
        context.cpuRegs.CP0.n.Status = 1; // source BeginPresenting/Open return with interrupts enabled
        for (unsigned r = 16; r < 24; ++r) context.cpuRegs.GPR.r[r].UD[0] = uint64_t(r) << 40 | r;
        // Explicit owned fixture bytes: all supplied pictures, independent of
        // how many a particular implementation happens to request.
        for (unsigned frame = 0; frame < 32; ++frame)
            for (unsigned i = 0; i < 32 * 16 * 4; ++i) trace.input.push_back(uint8_t(i * 37 + frame * 19));
    }
    ~Fixture() {
        pictures->Close();
        if (stream) stream->ResetAfterRestore();
        Check(!buffers.ActiveLoans(), "all owned source loans returned");
        if (stop) RecompAbi::BindMemory(nullptr);
    }
    uint8_t* Translate(uint32_t address, size_t size) override {
        return Scratchpad::Contains(address) ? scratch.Pointer(Scratchpad::Offset(address), size) :
            ram.Pointer(MainMemory::Phys(address), size);
    }
    uint32_t Read32(uint32_t address) override {
        uint32_t value;
        auto* data = Translate(address, 4); Check(data, "mapped input word");
        std::memcpy(&value, data, 4); return value;
    }
    void Write32(uint32_t address, uint32_t value) override {
        auto* data = Translate(address, 4); Check(data, "mapped output word");
        std::memcpy(data, &value, 4);
    }
    Decoder::Capture Capture() {
        return [this](uint32_t at) -> Decoder::Body {
            if (at >= 0x190000 && at < 0x190100 &&
                at != 0x190050 && at != 0x190068 && at != 0x190070) return {};
            if (bindings.count(at)) return bindings.at(at);
            return [this, at](CpuContext& c, uint32_t entry) {
                Check(!entry || entry == at, "model receives complete entry");
                Original(at, c);
            };
        };
    }
    Decoder::Installer Install() {
        return [this](uint32_t at, Decoder::Body body) { bindings[at] = std::move(body); return true; };
    }
    void Call(uint32_t at, CpuContext& c) {
        if (bindings.count(at)) { bindings.at(at)(c, at); return; }
        Original(at, c);
    }
    void Original(uint32_t at, CpuContext& c) {
        if (movie_oracle::RunGenerated(at, c)) return;
        if (at == Decoder::kDemultiplex) {
            // Shared synthetic sequence input, not a demux-retirement claim.
            const uint8_t sequence[]{0, 0, 1, 0xb3, 2, 0, 16, 0x15};
            std::memcpy(Translate(Read32(decoder + 0x100), 8), sequence, 8);
            c.cpuRegs.GPR.r[2].SD[0] = 1; return;
        }
        if (at == 0x2d4dc0 || at == 0x2d4e10) {
            const auto* row = SdkBindingManifest::Instance().Find("libkernl.a", at == 0x2d4dc0 ? "DIntr" : "EIntr");
            Check(row && row->handler, "actual retained kernel helper");
            row->handler(c); return;
        }
        if (at == 0x2cf560) { SyscallTable::Instance().Dispatch(100, c); return; }
        if (stream) {
            const auto arg = [&](unsigned n) { return c.cpuRegs.GPR.r[4 + n].UL[0]; };
            // Identical service/game inputs outside the copied-stack claim.
            // Production buffers, stream, decoder, EOF, picture and audio
            // consumers execute in both arms.
            const auto publish = [&](uint32_t at) {
                Write32(at, 0x300000); Write32(at + 4, uint32_t(strings[at].size()));
                std::memcpy(Translate(0x300000, strings[at].size() + 1), strings[at].c_str(), strings[at].size() + 1);
            };
            switch (at) {
            case 0x19fcb8:
                // Explicit shared game presentation provider outside the
                // WaitFrame-wrapper claim. Real MovieGraphics queue/upload
                // executes here; the graphics transport/raster is tested separately.
                Check(wait && arg(0) == 0 && c.cpuRegs.GPR.r[31].UL[0] == 0x2af588,
                      "same existing NEW game presentation service ABI");
                Word(trace.state, at);
                c.cpuRegs.GPR.r[4].UD[0] = player;
                pictures->QueuePicture(c);
                return;
            case 0x2cfc68: case 0x2cfcd0: case 0x2c1e10:
                if (stop) {
                    if (at == 0x2c1e10) ps2r::gs::LibgraphBindings::SyncVCallback(c);
                    else SyscallTable::Instance().Dispatch(at == 0x2cfc68 ? 21 : 20, c);
                    return;
                }
                Word(trace.state, at); Word(trace.state, arg(0)); return;
            case 0x1e6308: case 0x1e76f8: case 0x1e70d0: case 0x1e7740: case 0x1e6330:
                Word(trace.state, at); return;
            case 0x11bf10:
                Check(arg(1) == 8, "source ColourBlack lookup");
                Write32(arg(0), 0x80000000); Word(trace.state, at); return;
            case 0x19f1c8:
                Check(arg(0) == 0x180200 && arg(1) == 1 && arg(2) == 1, "same complete SetUpFrame input");
                Word(trace.state, at); Word(trace.state, Read32(arg(0)+0x14)); return;
            case 0x190050: case 0x190068: case 0x190070:
                Check(arg(0) == 0x180300, "source renderer virtual object");
                Word(trace.state, at);
                if (at != 0x190068) Word(trace.state, arg(1));
                Word(trace.state, Read32(0x180214)); return;
            case 0x1818b0: case 0x17dbf8: case 0x2c1b50: case 0x17ff50:
                Word(trace.state, at); return;
            case 0x1e01b0: ++lends; c.cpuRegs.GPR.r[2].UD[0] = 0; return;
            case 0x1e6230: ++reclaims; return;
            case 0x203dc8: strings[arg(0)] = "movies/"; publish(arg(0)); return;
            case 0x204060:
                strings[arg(0)] += arg(1) == 0x30a048 ? "logo" : ".pss"; publish(arg(0)); return;
            case 0x203ba8: strings.erase(arg(0)); return;
            case 0x2c6cb0:
                c.cpuRegs.GPR.r[2].UD[0] = arg(0) >= 'a' && arg(0) <= 'z' ? arg(0) - 32 : arg(0); return;
            case 0x1e60a8:
                Check(arg(0) == 3, "source movie volume group");
                Word(trace.state, at); c.fpuRegs.fpr[0].f = volume; return;
            }
        }
        Check(at == 0x2b1120, "only shared source audio registration is outside the Step scope");
    }
    void EnableStartServices() {
        stream = std::make_unique<title::MovieStream>(*this,
            [this](uint64_t, size_t bytes, uint8_t* out) {
                if (read_error) return int64_t{-1};
                std::fill(out, out + bytes, 0x5a); return int64_t(bytes);
            });
        Check(movie->InstallAudioPlayback(Capture(), Install(), true) == 1, "same software ring consumer");
        Check(stream->Install(Capture(), Install(), true) == 3, "same owned file cursor");
        Check(movie->InstallStream(Capture(), Install(), {
            [this](uint32_t l) { return stream->Seek(l); },
            [this](uint8_t* b, uint32_t n) { return stream->Read(b,n); }}) == 2, "one shared cursor");
        Check(movie->InstallInput(Capture(), Install(), [this](uint32_t d, uint32_t, int32_t bytes) {
            ++demuxes;
            const uint8_t sequence[]{0, 0, 1, 0xb3, 2, 0, 16, 0x15};
            std::memcpy(Translate(Read32(d + 0x100), 8), sequence, 8);
            if (bad_sequence) *Translate(Read32(d + 0x100), 1) = 1;
            return bytes;
        }) == 1, "same demux provider supplies its own sequence input");
        Check(stream->InstallBuffers(Capture(), Install(), *movie, {
            [this](CpuContext&, uint32_t lender, title::MovieStream::BufferPart part) {
                using Part = title::StreamBuffers::MoviePart;
                if (fail_loan) return title::MovieStream::BufferLoan{};
                const auto loan = buffers.BorrowMovie(lender,
                    part == title::MovieStream::BufferPart::SoundRing ? Part::SoundRing : Part::DiscStream);
                const auto view = buffers.Resolve(loan);
                return title::MovieStream::BufferLoan{loan.handle,view.bytes,view.data,
                    [this,loan] { return buffers.ReturnMovie(loan); }};
            }, [this](CpuContext& c, uint32_t a) { Call(a,c); }}) == 5, "two genuine balanced buffer consumers");
        Check(stream->InstallLookup(Capture(), Install(), [this](std::string_view path, auto& file) {
            Check(path == "MOVIES/LOGO.PSS", "unchanged source-built path");
            file = {32,file_bytes};
            return found ? title::MovieStream::LookupResult::Found : title::MovieStream::LookupResult::Missing;
        }) == 4, "same file lookup intent");
    }
    void PlayerStart(int expected) {
        Word(trace.input, 9); Word(trace.input, file_bytes); Word(trace.input, found);
        Word(trace.input, fail_loan); Word(trace.input, bad_sequence); Word(trace.input, prime_failure);
        Word(trace.input, read_error);
        uint32_t gain_bits; std::memcpy(&gain_bits, &volume, 4); Word(trace.input, gain_bits);
        pending = true; exhausted = prime_failure;
        context.cpuRegs.GPR.r[29].UD[0] = 0x70003f00;
        Write32(player + 0x18, 0x3068e0);
        Write32(0x306920, 0); Write32(0x306924, 0x2af9b8);
        Write32(player + 4, 64); Write32(player + 8, 16);
        Write32(0x309824, 0x400000); Write32(0x30a760, 0x410000);
        *Translate(0x3114f0 - 0x74c0, 1) = 1;
        context.cpuRegs.GPR.r[4].UD[0] = player;
        context.cpuRegs.GPR.r[31].UD[0] = 0x19a000;
        context.fpuRegs.fpr[20].UL = 0x12345678;
        Call(title::MovieStart::kAddress, context);
        Check(context.cpuRegs.GPR.r[2].SL[0] == expected, "unchanged Start return");
        Check(context.fpuRegs.fpr[20].UL == 0x12345678, "source callee floating register survives both primes");
        Observe();
        Word(trace.state, Read32(player + 0x1e0));
        Word(trace.state, Read32(Decoder::kSoundStarted));
        Word(trace.state, gain); Word(trace.state, demuxes); Word(trace.state, lends);
        Word(trace.state, buffers.ActiveLoans()); Word(trace.state, buffers.Used());
        if (expected) Check(buffers.ActiveLoans() == 2 && Read32(Decoder::kSoundStarted) == 1,
                            "Start owns both loans and completes initial audio-fill EOF");
        else Check(!buffers.ActiveLoans() && stream->CanCheckpoint(), "failed open returns both loans");
    }
    void PlayerClose() {
        Word(trace.input, 10);
        context.cpuRegs.GPR.r[4].UD[0] = player;
        Call(0x2af5d0, context);
        const auto ring = Read32(Decoder::kSoundRing);
        context.fpuRegs.fpr[12].f = 0;
        movie->SetVolume(context);
        const uint32_t args[]{1,0x80e0,0,2,0,0};
        for (unsigned n = 0; n < 6; ++n) context.cpuRegs.GPR.r[4+n].UD[0] = args[n];
        context.cpuRegs.GPR.r[31].UL[0] = 0x2ae6e4;
        Call(Decoder::kSoundRemote, context);
        context.cpuRegs.GPR.r[4].UD[0] = ring;
        context.cpuRegs.GPR.r[31].UL[0] = 0x2ae714;
        Call(0x2d46c8, context);
        Close();
        Check(!buffers.ActiveLoans() && !buffers.Used() && stream->CanCheckpoint() && start->CanCheckpoint(),
              "source cleanup returns both real loans before reopen");
    }
    void PlayerStop() {
        Word(trace.input, 12);
        context.cpuRegs.GPR.r[4].UD[0] = player;
        context.cpuRegs.GPR.r[31].UD[0] = 0x19a000;
        const auto color = Read32(0x180214);
        Call(title::MovieStop::kAddress,context);
        Check(context.cpuRegs.GPR.r[2].UL[0] == 1 && Read32(0x180214) == color,
              "source Stop returns one and restores target clear color");
        Check(!buffers.ActiveLoans() && !buffers.Used() && stream->CanCheckpoint() &&
              movie->CanCheckpoint() && pictures->CanCheckpoint() && stop->CanCheckpoint(),
              "complete Stop balances both buffers, PCM and picture owner before reopen");
        Check(SyscallTable::Instance().IntcHandlers().empty(), "both Stop arms leave no Movie callback");
        Observe();
        for (uint32_t at : {player, player+0x1e0, Decoder::kSoundRing, Decoder::kSoundRingSize, 0x180214u})
            Word(trace.state, Read32(at));
        Word(trace.state, gain); Word(trace.state, reclaims);
    }
    void Start() {
        Word(trace.input, 1);
        pending = true; exhausted = false;
        context.cpuRegs.GPR.r[29].UD[0] = stack;
        Write32(Decoder::kArenaNext, 0x400000); Write32(Decoder::kArenaEnd, 0xc00000);
        const uint32_t args[]{1, decoder, 1, 2, 1, 0, 0, 0};
        for (unsigned i = 0; i < 8; ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        for (unsigned i = 0; i < 4; ++i) Write32(stack + i * 8, i == 3 ? 0x100 : i == 1 ? 1 : 0);
        Call(Decoder::kStart, context);
        Check(context.cpuRegs.GPR.r[2].UL[0] == 1, "Start owns complete software pictures");
        Write32(Decoder::kSoundMode, 1); Write32(Decoder::kSoundStarted, 1);
        Write32(player + 0x1e4, 0);
        context.cpuRegs.GPR.r[29].UD[0] = 0x70003f00;
    }
    void Decode() {
        Word(trace.input, 2);
        context.cpuRegs.GPR.r[4].UD[0] = player + 0x44;
        pictures->DecodeAndUpload(context);
    }
    void Queue() {
        Word(trace.input, 3);
        context.cpuRegs.GPR.r[4].UD[0] = player;
        pictures->QueuePicture(context);
    }
    void Observe() {
        for (uint32_t offset : {0x38u, 0x3cu, 0x40u, 0x44u, 0x1e4u})
            Word(trace.state, Read32(player + offset));
        Word(trace.state, served);
        Word(trace.state, context.cpuRegs.GPR.r[2].UL[0]);
        for (unsigned r : {16u,17u,18u,19u,20u,21u,22u,23u,28u,29u,30u,31u})
            Word(trace.state, context.cpuRegs.GPR.r[r].UD[0]);
        Word(trace.state, context.cpuRegs.CP0.n.Status);
    }
    void Step(int expected) {
        Word(trace.input, 4);
        context.cpuRegs.GPR.r[4].UD[0] = player;
        context.cpuRegs.GPR.r[31].UD[0] = 0x19a000;
        // Record each actual boundary input, not only the fixture recipe.
        // The complete callee ABI also remains an observed output.
        Word(trace.input, pending); Word(trace.input, exhausted);
        for (uint32_t offset : {0x1cu, 0x38u, 0x3cu, 0x40u, 0x44u, 0x1e4u})
            Word(trace.input, Read32(player + offset));
        for (uint32_t offset : {0x108u, 0x10cu, 0x110u, 0x148u, 0x14cu, 0x154u, 0x15cu})
            Word(trace.input, Read32(decoder + offset));
        Word(trace.input, Read32(Decoder::kDecoderPointer));
        Word(trace.input, Read32(Decoder::kSoundMode));
        for (unsigned r : {4u,16u,17u,18u,19u,20u,21u,22u,23u,28u,29u,30u,31u})
            Word(trace.input, context.cpuRegs.GPR.r[r].UD[0]);
        Word(trace.input, context.cpuRegs.CP0.n.Status);
        Call(title::MovieStep::kAddress, context);
        Check(context.cpuRegs.GPR.r[2].SL[0] == expected, "unchanged Step return contract");
        Observe();
    }
    void Wait() {
        Word(trace.input, 11);
        context.cpuRegs.GPR.r[4].UD[0] = player;
        context.cpuRegs.GPR.r[31].UD[0] = 0x19a000;
        // Neither arm changes the obsolete interrupt flags. Their deliberately
        // distinct values also pin the actual registration over the spin body.
        Write32(0x309c20, 0x93827165); Write32(0x30a020, 0x16573829);
        for (uint32_t at : {player + 0x38, player + 0x3c, player + 0x40, 0x309c20u, 0x30a020u})
            Word(trace.input, Read32(at));
        const auto reads = generated[0x2b0fe0];
        const auto queued = Read32(player + 0x40);
        Call(title::MovieWait::kAddress, context);
        Check(generated[0x2b0fe0] - reads == (queued < 2 ? 2 : 0),
              "bounded refill preserves the existing complete WaitFrame service");
        Check(context.cpuRegs.GPR.r[2].UL[0] == 1 && context.cpuRegs.pc == 0x19a000 &&
              context.cpuRegs.GPR.r[4].UL[0] == player &&
              Read32(0x309c20) == 0x93827165 && Read32(0x30a020) == 0x16573829,
              "completion returns with unchanged caller and dormant interrupt bookkeeping");
        Observe();
        Word(trace.state, context.cpuRegs.pc); Word(trace.state, generated[0x2b0fe0]);
    }
    void Put(uint32_t at, uint32_t value) {
        Word(trace.input, 5); Word(trace.input, at); Word(trace.input, value); Write32(at, value);
    }
    void Close() {
        Word(trace.input, 6);
        Call(Decoder::kClose, context);
        Check(movie->CanCheckpoint() && step->CanCheckpoint(), "only source Close releases both owners");
    }
};
Fixture* current = nullptr;

Trace Run(bool selected) {
    auto owner = std::make_unique<Fixture>(selected);
    auto& f = *owner; current = &f;
    f.Start();
    f.Step(1); // no decoded picture
    f.Decode();
    Check(f.pictures->QueueDecoded(player), "source first prime queues before the second decode");
    f.Decode();
    f.Step(1); f.Step(1); // second slot then queue-full wait
    for (unsigned i = 0; i < 5; ++i) { f.Queue(); f.Step(1); }
    f.Put(player + 0x1e4, UINT32_MAX);
    f.Queue(); f.Step(1);
    Check(f.Read32(player + 0x1e4) == 0, "source frame counter keeps uint32 wrap");
    f.Put(decoder + 0x15c, 1); // accepted backlog remains
    f.Queue(); f.Step(1);
    Word(f.trace.input, 7); f.pending = false;
    f.Step(0); // no queue or decode after terminal EOF
    f.Close(); f.Step(0);
    f.Start(); f.Decode();
    f.Put(decoder + 0x15c, 1);
    Word(f.trace.input, 8); f.exhausted = true;
    f.Step(1); // accepted queue advance still increments frames on decode error
    f.Step(0); // software release is terminal, not an endless empty queue
    f.Close();
    f.Start(); f.Decode();
    f.Put(f.Read32(decoder + 0x40), 1);
    f.Step(0); // software-finished status wins over pending input
    f.Close();
    f.trace.census = f.step->Census();
    f.trace.model_steps = f.generated[title::MovieStep::kAddress];
    f.trace.copies = f.generated[0x2c6ce0];
    f.trace.served = f.served;
    Check(selected ? f.trace.model_steps == 0 && f.trace.copies == 0 :
                     f.trace.model_steps > 0 && f.trace.copies > 0,
          "actual source body and real scratchpad copy positive control versus selected zero");
    current = nullptr;
    return std::move(f.trace);
}

Trace RunStart(bool selected) {
    auto owner = std::make_unique<Fixture>(selected, true);
    auto& f = *owner; current = &f;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        f.file_bytes = cycle == 0 ? 2048 : cycle * 0x4000;
        f.volume = cycle == 0 ? 0.0f : cycle == 1 ? 0.5f : 1.0f;
        f.prime_failure = cycle == 3;
        f.PlayerStart(1);
        if (!f.prime_failure) {
            Check(f.Read32(player + 0x40) == 1 && f.Read32(player + 0x44) != 0 &&
                  f.Read32(player + 0x1e4) == 0, "two primes, one queued, no Step advance");
            f.Queue();
            Check(f.pictures->QueueDecoded(player), "second prime has its own owned image");
            f.Queue();
        }
        f.PlayerClose();
    }
    f.prime_failure = false;
    for (unsigned fault = 0; fault < 5; ++fault) {
        f.found = fault != 0; f.bad_sequence = fault == 1; f.fail_loan = fault == 2;
        f.file_bytes = fault == 3 ? 0 : 0x8000; f.read_error = fault == 4;
        f.PlayerStart(0);
        Check(!f.pictures->Active() && f.movie->CanCheckpoint(),
              "missing file, malformed sequence and allocation failure retain no pictures");
    }
    f.found = true; f.bad_sequence = f.fail_loan = f.read_error = false;
    f.PlayerStart(1); f.Queue(); f.PlayerClose();
    f.trace.census = f.start->Census();
    f.trace.model_starts = f.generated[title::MovieStart::kAddress];
    f.trace.copies = f.generated[0x2c6ce0];
    f.trace.served = f.served;
    Check(selected ? f.trace.model_starts == 0 && f.trace.copies == 0 :
                     f.trace.model_starts > 0 && f.trace.copies > 0,
          "actual Start/copies are positive only in the retained control");
    current = nullptr;
    return std::move(f.trace);
}

Trace RunWait(bool selected) {
    auto owner = std::make_unique<Fixture>(selected, false, true);
    auto& f = *owner; current = &f;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        f.file_bytes = cycle == 0 ? 2048 : 0x18000;
        f.PlayerStart(1);
        f.Wait(); // first primed picture; accepted decoder backlog survives EOF
        f.Step(1); f.Step(1); // full two-picture source queue
        f.Wait(); f.Wait(); f.Wait(); // full, shallow and empty/held frame
        if (cycle == 1) {
            // Decoder exhaustion closes compressed playback, not accepted
            // picture leases. The last queued picture still reaches the
            // same owned upload before the next source Step reports done.
            f.Put(decoder + 0x15c, 1);
            Word(f.trace.input, 8); f.exhausted = true;
            f.Step(1);
            Check(f.pictures->Active(), "terminal decode preserves queued picture owner until Close");
            f.Wait(); f.Step(0);
        }
        f.PlayerClose();
        Check(f.wait->CanCheckpoint(), "source Close leaves no WaitFrame continuation");
    }
    f.trace.census = f.wait->Census();
    f.trace.model_waits = f.generated[title::MovieWait::kAddress];
    f.trace.served = f.served;
    Check(selected ? !f.trace.model_waits : f.trace.model_waits > 0,
          "actual live registration wrapper is positive only in the comparison arm");
    current = nullptr;
    return std::move(f.trace);
}

Trace RunStop(bool selected) {
    auto owner = std::make_unique<Fixture>(selected, false, false, true);
    auto& f = *owner; current = &f;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        f.file_bytes = cycle == 0 ? 2048 : 0x18000;
        f.prime_failure = cycle == 2;
        f.PlayerStart(1);
        if (!f.prime_failure) { f.Wait(); f.Step(1); f.Wait(); }
        f.PlayerStop();
        Check(f.wait->CanCheckpoint() && f.start->CanCheckpoint(), "source Stop leaves no suspended Movie work");
    }
    f.trace.census = f.stop->Census();
    f.trace.model_stops = f.generated[title::MovieStop::kAddress];
    f.trace.registrations = ps2r::gs::LibgraphBindings::SyncVCallbackCalls();
    f.trace.served = f.served;
    Check(selected ? !f.trace.model_stops && !f.trace.registrations :
                     f.trace.model_stops == 4 && f.trace.registrations == 8,
          "actual generated Stop and actual libgraph registration positive versus selected zero");
    current = nullptr;
    return std::move(f.trace);
}

void RejectedStops() {
    for (unsigned fault = 0; fault < 9; ++fault) {
        auto owner = std::make_unique<Fixture>(true, false, false, true);
        auto& f = *owner; current = &f; f.PlayerStart(1);
        f.context.cpuRegs.GPR.r[4].UD[0] = player;
        uint32_t entry = title::MovieStop::kAddress;
        if (fault == 0) entry += 4;
        if (fault == 1) entry = title::MovieStop::kCallback;
        if (fault == 2) f.context.cpuRegs.GPR.r[28].UD[0] = 0;
        if (fault == 3) f.Write32(0x309728, 0);
        if (fault == 4) f.Write32(0x180454, 0);
        if (fault == 5) f.context.cpuRegs.CP0.n.Status &= ~1u;
        if (fault == 6) f.Write32(decoder + 0x14c, 2);
        if (fault == 7) f.PlayerStop();
        if (fault == 8) f.Write32(0x180454, 0x190051);
        const auto picture = f.trace.picture, state = f.trace.state;
        const auto loans = f.buffers.ActiveLoans();
        f.context.cpuRegs.GPR.r[4].UD[0] = player;
        bool refused = false;
        try { f.bindings.at(fault == 1 ? title::MovieStop::kCallback : title::MovieStop::kAddress)(f.context,entry); }
        catch (const std::runtime_error&) { refused = true; }
        Check(refused && !f.stop->CanCheckpoint() && f.buffers.ActiveLoans() == loans &&
              f.trace.picture == picture && f.trace.state == state &&
              !f.generated[title::MovieStop::kAddress] && !f.generated[title::MovieStop::kCallback] &&
              !ps2r::gs::LibgraphBindings::SyncVCallbackCalls(),
              "invalid Stop/callback refuses before service teardown or retained execution");
        current = nullptr;
    }
}

void RejectedWaits() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto owner = std::make_unique<Fixture>(true, false, true);
        auto& f = *owner; current = &f;
        f.PlayerStart(1);
        f.context.cpuRegs.GPR.r[4].UD[0] = player;
        uint32_t entry = title::MovieWait::kAddress;
        if (fault == 0) entry += 1;
        if (fault == 1) entry = title::MovieWait::kTail;
        if (fault == 2) f.context.cpuRegs.GPR.r[28].UD[0] = 0;
        if (fault == 3) f.context.cpuRegs.GPR.r[4].UD[0] = player + 4;
        if (fault == 4) f.Write32(player + 0x40, 3);
        const auto picture = f.trace.picture, state = f.trace.state;
        const auto reads = f.generated[0x2b0fe0];
        bool refused = false;
        try { f.bindings.at(fault == 1 ? title::MovieWait::kTail : title::MovieWait::kAddress)(f.context,entry); }
        catch (const std::runtime_error&) { refused = true; }
        Check(refused && !f.wait->CanCheckpoint() && f.trace.picture == picture && f.trace.state == state &&
              f.generated[0x2b0fe0] == reads && !f.generated[title::MovieWait::kAddress],
              "invalid wait/tail/owner refuses before refill, presentation or delegation");
        current = nullptr;
    }
}

void RejectedStarts() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto owner = std::make_unique<Fixture>(true, true);
        auto& f = *owner; current = &f;
        f.PlayerStart(1);
        if (fault != 4) f.PlayerClose();
        f.context.cpuRegs.GPR.r[4].UD[0] = player;
        uint32_t entry = title::MovieStart::kAddress;
        if (fault == 0) entry += 4;
        if (fault == 1) f.context.cpuRegs.GPR.r[28].UD[0] = 0;
        if (fault == 2) f.Write32(player + 0x18, 0);
        if (fault == 3) f.context.cpuRegs.GPR.r[29].UD[0] = 0;
        const auto served = f.served, loans = f.buffers.ActiveLoans(), lends = f.lends;
        bool rejected = false;
        try { f.bindings.at(title::MovieStart::kAddress)(f.context, entry); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && !f.start->CanCheckpoint() && !f.generated[title::MovieStart::kAddress] &&
              !f.generated[0x2c6ce0], "invalid/active Start never delegates or becomes checkpointable");
        Check(f.served == served && f.buffers.ActiveLoans() == loans && f.lends == lends,
              "rejected Start preserves existing owner and loans");
        current = nullptr;
    }
}

void RejectedEntries() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto owner = std::make_unique<Fixture>(true);
        auto& f = *owner; current = &f;
        f.Start(); f.Decode();
        uint32_t entry = title::MovieStep::kAddress;
        f.context.cpuRegs.GPR.r[4].UD[0] = player;
        if (fault == 0) entry += 4;
        if (fault == 1) f.context.cpuRegs.GPR.r[4].UD[0] = player + 16;
        if (fault == 2) f.Write32(player + 0x40, 3);
        if (fault == 3) f.Write32(player + 0x44, 0xbad);
        if (fault == 4) f.Write32(decoder + 0x110, 1);
        const auto queued = f.Read32(player + 0x40), frames = f.Read32(player + 0x1e4), served = f.served;
        bool rejected = false;
        try { f.bindings.at(title::MovieStep::kAddress)(f.context, entry); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && !f.step->CanCheckpoint() && f.generated[title::MovieStep::kAddress] == 0 &&
              f.generated[0x2c6ce0] == 0, "invalid selected entry never falls back or becomes checkpointable");
        Check(f.Read32(player + 0x40) == queued && f.Read32(player + 0x1e4) == frames && f.served == served,
              "invalid source/caller/audio/queue state rejects before advancing or decoding");
        current = nullptr;
    }
}

Bytes Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(in.good(), "artifact input exists");
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
std::string Digest(const Bytes& bytes) {
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    Check(sha && av_sha_init(sha.get(), 256) == 0, "SHA256 initialized");
    av_sha_update(sha.get(), bytes.data(), bytes.size());
    std::array<uint8_t, 32> hash{}; av_sha_final(sha.get(), hash.data());
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t b : hash) out << std::setw(2) << unsigned(b);
    return out.str();
}
std::string Quote(const std::string& value) {
    std::ostringstream out; out << std::quoted(value); return out.str();
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    Check(!std::filesystem::exists(path), "artifact path is fresh");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.close(); Check(out.good(), "artifact write completed");
}
void Export(const Trace& model, const Trace& selected, int argc, char** argv,
            const movie_oracle::ExportArgs& args, bool start, bool wait, bool stop) {
    if (args.directory.empty()) return;
    const auto directory = std::filesystem::absolute(args.directory);
    Check(!std::filesystem::exists(directory) && std::filesystem::create_directories(directory), "fresh oracle export");
    const auto binary = std::filesystem::canonical(argv[0]);
    std::ostringstream command;
    command << '[' << Quote(binary.string());
    for (int i = 1; i < argc; ++i) command << ',' << Quote(argv[i]);
    command << ']';
    for (bool native : {false, true}) {
        const auto& t = native ? selected : model;
        const std::string mode = native ? "native" : "model";
        const auto arm = directory / mode;
        Check(std::filesystem::create_directory(arm), "separate arm artifacts");
        Save(arm / "input.bin", t.input); Save(arm / "picture.bin", t.picture); Save(arm / "state.bin", t.state);
        std::ostringstream receipt;
        receipt << "{\"schema\":\"retirement-oracle-run-v1\",\"mode\":" << Quote(mode)
            << ",\"test\":" << Quote(stop ? "MovieStopOracle" : wait ? "MovieWaitOracle" : start ? "MovieStartOracle" : "MovieStepOracle")
            << ",\"command\":" << command.str()
            << ",\"binary_sha256\":" << Quote(Digest(Load(binary)))
            << ",\"source_sha256\":" << Quote(Digest(Load(args.source_manifest)))
            << ",\"input_sha256\":" << Quote(Digest(t.input)) << ",\"exit_code\":0,\"completed\":true,"
            << "\"settings\":{" << Quote(stop ? "hle.movie_stop" : wait ? "hle.movie_wait" : start ? "hle.movie_start" : "hle.movie_step")
            << ':' << Quote(native ? "on" : "off")
            << ",\"hle.movie_lifecycle\":\"on\",\"hle.graphics_movie\":\"on\""
            << ((start || wait || stop) ? ",\"hle.music\":\"on\",\"hle.music_buffers\":\"on\"" : "")
            << ((wait || stop) ? ",\"hle.movie_start\":\"on\",\"hle.movie_step\":\"on\"" : "")
            << (stop ? ",\"hle.movie_wait\":\"on\"" : "")
            << ",\"pictures\":\"own-NEW-fixture\"},"
            << "\"outputs\":{\"picture\":" << Quote(Digest(t.picture)) << ",\"state\":" << Quote(Digest(t.state)) << "},"
            << "\"counters\":{" << Quote(stop ? "movie_stop" : wait ? "movie_wait" : start ? "movie_start" : "movie_step") << ':' << Quote(t.census)
            << ",\"generated_start\":" << t.model_starts << ",\"generated_step\":" << t.model_steps
            << ",\"registered_wait\":" << t.model_waits
            << ",\"generated_stop\":" << t.model_stops << ",\"libgraph_calls\":"
            << Quote("syncvcb=" + std::to_string(t.registrations))
            << ",\"generated_copy\":" << t.copies << ",\"pictures_served\":" << t.served << "}}\n";
        const auto text = receipt.str();
        Save(arm / "receipt.json", Bytes(text.begin(), text.end()));
    }
}
}  // namespace

namespace movie_oracle {
std::ostringstream g_logFile;
void Read(uint32_t at, void* out, size_t size) {
    auto* p = current->Translate(at, size); Check(p, "generated read backed"); std::memcpy(out, p, size);
}
void Write(uint32_t at, const void* in, size_t size) {
    auto* p = current->Translate(at, size); Check(p, "generated write backed"); std::memcpy(p, in, size);
}
void Dispatch(uint32_t at, CpuContext& c) { current->Call(at, c); }
void ObserveGenerated(uint32_t at) { ++current->generated[at]; }
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t at) {
    Check(at == 0x2af9b8 || at == 0x2afa00 || at == 0x190050 || at == 0x190068 || at == 0x190070,
          "enumerated complete sound and shared game-renderer virtual calls");
    static const std::function<void(CpuContext&, uint32_t)> lend =
        [](CpuContext& c, uint32_t entry) { current->Call(entry,c); };
    return &lend;
}
}  // namespace movie_oracle

int main(int argc, char** argv) {
    movie_oracle::ExportArgs args;
    if (argc < 2 || (std::string(argv[1]) != "--both-paths" && std::string(argv[1]) != "--start-paths" &&
                    std::string(argv[1]) != "--wait-paths" && std::string(argv[1]) != "--stop-paths") ||
        !movie_oracle::ExportArgs::Parse(argc, argv, 2, args)) return 2;
    const bool start = std::string(argv[1]) == "--start-paths";
    const bool wait = std::string(argv[1]) == "--wait-paths";
    const bool stop = std::string(argv[1]) == "--stop-paths";
    KernlBoot::Register(SdkBindingManifest::Instance());
    GuestPark::SetSink([](const ParkRecord&) { throw std::runtime_error("invalid Movie intent"); });
    const auto model = stop ? RunStop(false) : wait ? RunWait(false) : start ? RunStart(false) : Run(false);
    const auto selected = stop ? RunStop(true) : wait ? RunWait(true) : start ? RunStart(true) : Run(true);
    Check(model.input == selected.input && model.picture == selected.picture && model.state == selected.state,
          "same input, uploaded picture bytes, queue/EOF/frame and callee ABI output");
    Check(!model.picture.empty() && model.served == selected.served, "positive observable picture work");
    if (stop) RejectedStops(); else if (wait) RejectedWaits(); else if (start) RejectedStarts(); else RejectedEntries();
    Export(model, selected, argc, argv, args, start, wait, stop);
    GuestPark::SetSink({});
    std::printf("Movie%sOracle: %u checks, actual body %llu->0, memcpy %llu->0, %llu pictures; exact output.\n",
        stop ? "Stop" : wait ? "Wait" : start ? "Start" : "Step", checks,
        static_cast<unsigned long long>(stop ? model.model_stops : wait ? model.model_waits : start ? model.model_starts : model.model_steps),
        static_cast<unsigned long long>(model.copies),
        static_cast<unsigned long long>(selected.served));
    if (stop) std::printf("Actual Libgraph registrations: %llu->0; same picture/state/renderer-service output.\n",
                         static_cast<unsigned long long>(model.registrations));
}
