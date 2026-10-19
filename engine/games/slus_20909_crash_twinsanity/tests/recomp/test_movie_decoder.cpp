// Movie boundary contracts, without libipu, DMAC, FFmpeg or game assets.
// Layout oracle: TR movie/mpeg.h and mpeg.cpp, checked against the US Create/Reset.
// US ELF SLUS_209.09: Create's SD zero at 0x2b7158/0x2b716c clears both
// 64-bit flags, not just their low words. Function starts were checked in
// PT_LOAD bytes using their prologues and SDK JAL targets.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"

namespace {

using Decoder = title::MovieDecoder;
int g_checks = 0;
int g_failures = 0;

#define CHECK(condition, description)                                                       \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            std::fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, description, #condition); \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

class TestMovieDecoder final : public ps2r::ipu::GuestMemory {
public:
    static constexpr uint32_t kRamSize = 0x400000;
    static constexpr uint32_t kDecoder = 0x10000;
    static constexpr uint32_t kWork = 0x40000;
    static constexpr uint32_t kPlane0 = 0x80000;
    static constexpr uint32_t kPlane1 = 0x90000;
    static constexpr uint32_t kRaw8 = 0xa0000;
    static constexpr uint32_t kArena = 0x120000;
    static constexpr uint32_t kArenaLimit = 0x280000;
    static constexpr uint32_t kStack = 0x3b0000;
    static constexpr uint32_t kSeek = 0x2000;
    static constexpr uint32_t kRead = 0x2010;
    static constexpr uint32_t kPath = 0x2020;
    static constexpr uint8_t kGuard = 0xa5;
    static constexpr int32_t kOriginalResult = 12345;

    struct PictureCall {
        bool rgb16;
        uint32_t decoder, argument_buffer, macroblocks, decoded, destination;
    };
    struct OriginalCall {
        uint32_t function, entry;
        std::array<uint32_t, 3> arguments;
    };
    struct ServiceCall {
        uint32_t function;
        std::array<uint32_t, 5> arguments;
        uint32_t sys, stack;
    };

    std::vector<uint8_t> ram = std::vector<uint8_t>(kRamSize, kGuard);
    CpuContext context{};
    std::unique_ptr<Decoder> decoder;
    std::map<uint32_t, Decoder::Body> installed;
    std::vector<PictureCall> pictures;
    std::vector<OriginalCall> originals;
    std::vector<ServiceCall> services;
    std::vector<uint32_t> destroyed;
    std::vector<uint16_t> volumes;
    std::vector<char> events;
    std::vector<bool> service_checkpoint_ready;
    int create_calls = 0, reset_calls = 0, invalid_accesses = 0;
    int initialize_calls = 0;
    bool fail_picture = false;
    bool fail_demux = false;
    bool throw_seek = false;
    bool throw_upload = false;
    bool invalid_sequence = false;
    bool pending_playback = false;
    uint32_t sequence_width = 32, sequence_height = 48;

    TestMovieDecoder() {
        Write32(Decoder::kDecoderPointer, kDecoder);
        Write32(kDecoder + 0x40, kWork);
        Write32(kDecoder + 0x54, 0);
        Write32(kDecoder + 0x58, 32);
        Write32(kDecoder + 0x5c, 48);
        Write32(kDecoder + 0x94, kPlane0);
        Write32(kDecoder + 0x98, kPlane1);
        Write32(kDecoder + 0x9c, kRaw8);
        Write32(kDecoder + 0x148, 2);
        Write32(kDecoder + 0x14c, 1);
        Write32(kDecoder + 0x154, 0x100);
        Write32(kDecoder + 0x158, 1);
        Write32(kDecoder + 0x15c, 0);
        Write32(kWork, 0);
        Write32(Decoder::kSoundStarted, 1);
        decoder = std::make_unique<Decoder>(*this, MakeHooks());
        CHECK(decoder->Install(Capture(), Installer(), true) == 7, "all seven boundaries explicitly installed");
        CHECK(installed.size() == 7, "installation keeps seven distinct function starts");
    }

    uint8_t* Translate(uint32_t address, size_t size) override {
        if (address > ram.size() || size > ram.size() - address) return nullptr;
        return ram.data() + address;
    }
    uint32_t Read32(uint32_t address) override {
        const uint8_t* p = Translate(address, 4);
        if (!p) { ++invalid_accesses; CHECK(false, "invalid guest word read"); return 0; }
        return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
    }
    void Write32(uint32_t address, uint32_t value) override {
        uint8_t* p = Translate(address, 4);
        if (!p) { ++invalid_accesses; CHECK(false, "invalid guest word write"); return; }
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    std::vector<uint8_t> Bytes(uint32_t address, uint32_t size) const {
        return {ram.begin() + address, ram.begin() + address + size};
    }
    bool Filled(uint32_t address, uint32_t size, uint8_t value) const {
        return std::all_of(ram.begin() + address, ram.begin() + address + size,
                           [value](uint8_t byte) { return byte == value; });
    }
    void Args(uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
        context.cpuRegs.GPR.r[4].UD[0] = a0;
        context.cpuRegs.GPR.r[5].UD[0] = a1;
        context.cpuRegs.GPR.r[6].UD[0] = a2;
        context.cpuRegs.GPR.r[2].SD[0] = -999;
    }
    int32_t Result() const { return context.cpuRegs.GPR.r[2].SL[0]; }
    void Call(uint32_t function) { installed.at(function)(context, function); }
    void Create(uint32_t work = kWork, uint32_t size = 0x2800) {
        Args(kDecoder, work, size);
        Call(Decoder::kCreate);
    }
    uint64_t Count(const char* key) const {
        const std::string census = "," + decoder->Census();
        const std::string prefix = std::string(",") + key + "=";
        const size_t at = census.find(prefix);
        CHECK(at != std::string::npos, "requested census field exists");
        return at == std::string::npos ? 0 : std::strtoull(census.c_str() + at + prefix.size(), nullptr, 10);
    }
    Decoder::Hooks MakeHooks() {
        Decoder::Hooks hooks;
        hooks.create = [this](CpuContext&) { ++create_calls; events.push_back('C'); };
        hooks.reset = [this](CpuContext&) { ++reset_calls; events.push_back('R'); };
        hooks.picture_rgb32 = [this](CpuContext& ctx) { Serve(ctx, false); };
        hooks.picture_rgb16 = [this](CpuContext& ctx) { Serve(ctx, true); };
        hooks.initialize = [this] { ++initialize_calls; events.push_back('I'); };
        hooks.destroy = [this](uint32_t address) {
            destroyed.push_back(address);
            pending_playback = false;
            events.push_back('X');
        };
        hooks.call = [this](CpuContext& ctx, uint32_t address) { Service(ctx, address); };
        hooks.has_pending = [this](uint32_t address) {
            CHECK(address == Read32(Decoder::kDecoderPointer), "status queries the current playback");
            return pending_playback;
        };
        hooks.volume = [this](uint16_t gain) { volumes.push_back(gain); };
        return hooks;
    }
    Decoder::Capture Capture() {
        return [this](uint32_t function) -> Decoder::Body {
            return [this, function](CpuContext& ctx, uint32_t entry) {
                originals.push_back({function, entry, {ctx.cpuRegs.GPR.r[4].UL[0],
                    ctx.cpuRegs.GPR.r[5].UL[0], ctx.cpuRegs.GPR.r[6].UL[0]}});
                ctx.cpuRegs.GPR.r[2].SD[0] = kOriginalResult;
            };
        };
    }
    Decoder::Installer Installer() {
        return [this](uint32_t function, Decoder::Body body) {
            installed[function] = std::move(body);
            return true;
        };
    }
    void StartArgs(bool create = true, uint32_t format = 0x100, uint32_t buffers = 1, uint32_t audio = 1) {
        const std::array<uint32_t, 8> args = {create ? 1u : 0u, kDecoder, 1, 2, audio, 7, kSeek, kRead};
        for (uint32_t i = 0; i < args.size(); ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        context.cpuRegs.GPR.r[29].UD[0] = kStack;
        context.cpuRegs.GPR.r[28].UD[0] = 0x12348000;
        context.cpuRegs.GPR.r[31].UD[0] = 0x123456789abcdef0;
        for (uint32_t i = 16; i < 24; ++i) {
            context.cpuRegs.GPR.r[i].UD[0] = 0x1122334400000000ull + i;
            context.cpuRegs.GPR.r[i].UD[1] = 0xaabbccdd00000000ull + i;
        }
        Write32(kStack, kPath);
        Write32(kStack + 8, buffers);
        Write32(kStack + 0x10, 10);
        Write32(kStack + 0x18, format);
    }
    void Arena(uint32_t begin = kArena, uint32_t end = kArenaLimit) {
        Write32(Decoder::kArenaNext, begin);
        Write32(Decoder::kArenaEnd, end);
    }

private:
    void Service(CpuContext& ctx, uint32_t address) {
        service_checkpoint_ready.push_back(decoder->CanCheckpoint());
        ServiceCall call{address, {}, Read32(kDecoder + 0x40), ctx.cpuRegs.GPR.r[29].UL[0]};
        for (uint32_t i = 0; i < call.arguments.size(); ++i) call.arguments[i] = ctx.cpuRegs.GPR.r[4 + i].UL[0];
        services.push_back(call);
        ctx.cpuRegs.GPR.r[31].UD[0] = address + 8;  // a generated body may make nested calls
        if (address == Decoder::kSeekStream) {
            installed.at(address)(ctx, address);
        } else if (address == kSeek) {
            events.push_back('K');
            if (throw_seek) throw 7;
            ctx.cpuRegs.GPR.r[2].SD[0] = -1;  // retail Start deliberately ignores seek's return
        } else if (address == 0x2b1120) {
            events.push_back('A');
            const uint32_t table = Read32(call.sys + 0x60);
            Write32(table + 0x10, 0x55550000);  // stand-in for the registered software video callback
            Write32(table + 0x14, call.arguments[0]);
            Write32(call.sys + 0x64, 1);
        } else if (address == 0x2b0ae0) {
            events.push_back('D');
            CHECK(call.arguments[0] == 1, "initial demux waits for data");
            CHECK(Read32(call.sys + 0x64) == 1, "PSS demux sees its stream registration");
            const uint32_t anchor = Read32(kDecoder + 0x100);
            uint8_t* bytes = Translate(anchor, 7);
            CHECK(bytes != nullptr, "demux owns a backed sequence header destination");
            if (bytes) {
                bytes[0] = bytes[1] = 0;
                bytes[2] = 1;
                bytes[3] = invalid_sequence ? 0x00 : 0xb3;
                bytes[4] = static_cast<uint8_t>(sequence_width >> 4);
                bytes[5] = static_cast<uint8_t>((sequence_width << 4) | (sequence_height >> 8));
                bytes[6] = static_cast<uint8_t>(sequence_height);
            }
            Write32(kDecoder + 0x4c, fail_demux ? 0 : 32);
            Write32(kDecoder + 0x15c, fail_demux ? 1 : 0);
            ctx.cpuRegs.GPR.r[2].SD[0] = fail_demux ? 0 : 128;
        } else if (address == 0x2b1448) {
            events.push_back('U');
            if (throw_upload) throw 8;
            CHECK(call.arguments[2] == sequence_width / 16 && call.arguments[3] == sequence_height / 16,
                  "upload builder receives column and row counts");
            Write32(call.arguments[0], 0x12345678);
            Write32(call.arguments[0] + 4, call.arguments[1]);
        } else {
            CHECK(false, "Start calls only the retained software services");
        }
    }
    void Serve(CpuContext& ctx, bool rgb16) {
        const uint32_t d = ctx.cpuRegs.GPR.r[4].UL[0];
        const uint32_t buffer = ctx.cpuRegs.GPR.r[5].UL[0];
        const uint32_t mb = ctx.cpuRegs.GPR.r[6].UL[0];
        const uint32_t count = Read32(d + 0x54);
        // The RGB16 service accepts the RAW8 SDK argument but writes the final
        // selected picture plane. Its successful SDK result is the MB count.
        const bool odd = (count & 1u) != 0 && Read32(d + 0x158) != 0;
        const uint32_t destination = rgb16 ? Read32(d + (odd ? 0x98 : 0x94)) : buffer;
        pictures.push_back({rgb16, d, buffer, mb, count, destination});
        events.push_back(rgb16 ? 'S' : 'P');
        if (!fail_picture) Write32(destination, 0x70000000u + static_cast<uint32_t>(pictures.size()));
        ctx.cpuRegs.GPR.r[2].SD[0] = fail_picture ? -1 : (rgb16 ? static_cast<int32_t>(mb) : 0);
    }
};

using Rig = TestMovieDecoder;

void TestUsEntrypoints() {
    CHECK(Decoder::kCreate == 0x002b70a8u, "US Create entry");
    CHECK(Decoder::kReset == 0x002b7cb0u, "US Reset entry");
    CHECK(Decoder::kDecodePicture == 0x002b01e8u, "US private DecodePicture entry");
    CHECK(Decoder::kDecoderPointer == 0x0030a700u, "US DecodePicture global decoder load");
    CHECK(Decoder::kInitialise == 0x002b0d30u, "US Initialise entry, ELF prologue and JAL checked");
    CHECK(Decoder::kStart == 0x002afc00u, "US shared Start entry, ELF Open JAL checked");
    CHECK(Decoder::kClose == 0x002b1000u, "US Close entry, ELF Delete JAL checked");
    CHECK(Decoder::kFinished == 0x002b0f68u, "US Finished entry, EOF and IsEnd branches checked");
    CHECK(Decoder::kSoundStarted == 0x0030a938u, "US Prepare/StartSound software phase stores checked");
    CHECK(Decoder::kArenaNext == 0x0030a704u && Decoder::kArenaEnd == 0x0030a708u,
          "US SetArena stores checked against instructions");
}

void CheckPublicContext(Rig& r, uint32_t sys) {
    for (uint32_t offset : {0u, 4u, 8u, 0x20u, 0x24u, 0x38u, 0x3cu})
        CHECK(r.Read32(Rig::kDecoder + offset) == 0, "geometry, count and both full-width flags cleared");
    for (uint32_t offset : {0x10u, 0x18u, 0x28u, 0x30u}) {
        CHECK(r.Read32(Rig::kDecoder + offset) == 0xffffffffu, "PTS/DTS low word is -1");
        CHECK(r.Read32(Rig::kDecoder + offset + 4) == 0xffffffffu, "PTS/DTS high word is -1");
    }
    CHECK(r.Read32(Rig::kDecoder + 0x40) == sys, "published 4-byte-aligned sys pointer");
    CHECK(r.Filled(Rig::kDecoder + 0xc, 4, Rig::kGuard), "public alignment padding preserved");
    CHECK(r.Filled(Rig::kDecoder + 0x44, 4, Rig::kGuard), "public trailing padding preserved");
}

void CheckSoftwareContext(Rig& r, uint32_t s, uint32_t table, uint32_t end) {
    CHECK(r.Filled(s + 0xc, 7 * 0xc, 0), "callback slots empty, including retired DMA callbacks");
    CHECK(r.Read32(s + 0x60) == table && (table & 7u) == 0, "callback table is aligned to 8 after private context");
    CHECK(r.Read32(s + 0x64) == 0, "no stream callbacks registered yet");
    CHECK(r.Filled(s + 0x19a0, table - (s + 0x19a0), 0), "callback alignment gap cleared within work");
    CHECK(r.Filled(table, 0x800, 0), "all 64 stream entries zeroed");
    CHECK(r.Read32(s + 0x118) == s + 0x19a0, "arena base");
    CHECK(r.Read32(s + 0x11c) == end - (s + 0x19a0), "arena capacity excludes private context");
    CHECK(r.Read32(s + 0x120) == table + 0x800, "arena next reserves callback table");
    CHECK(r.Read32(s + 0x124) == table + 0x800, "arena mark follows table allocation");
    CHECK(r.Read32(s + 0x868) == Rig::kDecoder, "private context points back to public object");
    CHECK(r.Read32(s + 0xc4) == 1, "initial RGB32 output");
    for (uint32_t offset : {0u, 4u, 8u, 0xc0u, 0x108u, 0x128u, 0x1c0u, 0x858u, 0x878u})
        CHECK(r.Read32(s + offset) == 0, "initial software decode state");
    for (uint32_t offset : {0x98u, 0xa8u, 0xacu, 0xb0u, 0x100u, 0x104u, 0x860u})
        CHECK(r.Read32(s + offset) == 0xffffffffu, "initial timestamp/limit sentinel");
    CHECK(r.Read32(s + 0x828) == 1, "software bit cache starts stale");
    // Nine MpegImage records, with the current-reference slot deliberately empty.
    constexpr uint32_t slots[] = {0x1c8, 0x1cc, 0x1d4, 0x1d8, 0x1dc, 0x1e4, 0x1e8, 0x1ec, 0x1f4};
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t image = r.Read32(s + slots[i]);
        CHECK(image == s + 0x1f8 + i * 0x68, "reference slot names its software image descriptor");
        CHECK(r.Filled(image, 0x68, 0), "new descriptor contains no picture or pixel allocation");
    }
    for (uint32_t slot : {0x1d0u, 0x1e0u, 0x1f0u})
        CHECK(r.Read32(s + slot) == 0, "current reference remains empty");
}

void TestCreateLayoutAndGuards() {
    // Software compatibility needs 4-byte context / 8-byte table alignment.
    // The old 64-byte DMA alignment is not a software Create precondition.
    for (uint32_t work : {Rig::kWork, Rig::kWork - 1, Rig::kWork - 2, Rig::kWork - 3,
                          Rig::kWork + 1, Rig::kWork + 4, Rig::kWork + 0x10}) {
        Rig r;
        const uint32_t sys = (work + 3u) & ~3u;
        const uint32_t table = (sys + 0x19a0u + 7u) & ~7u;
        const uint32_t size = table + 0x800u - work;  // exact minimum, including alignment gaps
        std::fill(r.ram.begin() + work - 32, r.ram.begin() + work + size + 32, Rig::kGuard);
        const auto driver = r.Bytes(Rig::kDecoder + 0x48, 0x118);
        r.Create(work, size);
        CHECK(r.Result() == 1, "minimum valid work extent succeeds after 4-byte alignment");
        CHECK(r.Filled(work - 32, 32, Rig::kGuard), "guard before work intact");
        CHECK(r.Filled(work + size, 32, Rig::kGuard), "guard after work intact");
        CHECK(r.Filled(work, sys - work, 0), "supplied alignment prefix cleared within bounds");
        CHECK(r.Bytes(Rig::kDecoder + 0x48, 0x118) == driver, "Create preserves outer driver fields");
        CheckPublicContext(r, sys);
        CheckSoftwareContext(r, sys, table, work + size);
        CHECK((r.events == std::vector<char>{'C', 'R'}), "Create accounting precedes native reset once");
        CHECK(r.Count("created") == 1 && r.Count("reset") == 1, "Create census includes its reset");
        CHECK(r.invalid_accesses == 0 && r.originals.empty(), "native Create avoids hardware/original body");
    }
}

void TestInvalidWork() {
    const std::array<std::array<uint32_t, 2>, 9> cases = {{
        {0, 0x2800}, {Rig::kWork, 0}, {Rig::kWork, 0x19a0}, {Rig::kWork, 0x219f},
        {Rig::kWork + 4, 0x21a3}, {Rig::kWork + 1, 0x21a6},
        {Rig::kRamSize - 0x2000, 0x2800}, {0xfffffff0u, 0x2800},
        {Rig::kWork, 0xffffffffu},
    }};
    for (const auto& test : cases) {
        Rig r;
        const auto before = r.ram;
        r.Create(test[0], test[1]);
        CHECK(r.Result() == 0, "short, unbacked, wrapped or null work returns failure");
        CHECK(r.ram == before, "invalid Create publishes no sys pointer and writes no bytes");
        CHECK(r.reset_calls == 0, "invalid Create never resets an existing playback");
        CHECK(r.Count("invalid_work") == 1 && r.Count("reset") == 0, "invalid work is observable");
        CHECK(r.originals.empty() && r.invalid_accesses == 0, "bad work is not passed to generated code");
    }
}

void TestResetPreservesOwnership() {
    Rig r;
    r.Create();
    const uint32_t s = Rig::kWork;
    const uint32_t callbacks = r.Read32(s + 0x60);
    // Exercise the words that generated AddCallback/AddStrCallback populate.
    for (uint32_t offset : {0xcu, 0x10u, 0x14u}) r.Write32(s + offset, 0x12340000 + offset);
    for (uint32_t offset = 0; offset < 0x20; offset += 4) r.Write32(callbacks + offset, 0x56780000 + offset);
    r.Write32(s + 0x64, 1);
    r.Write32(s + 0x120, callbacks + 0x880);
    r.Write32(s + 0x124, callbacks + 0x840);
    r.Write32(s + 0x1f8, Rig::kPlane0);
    const auto callback_state = r.Bytes(s + 0xc, 0x80);
    const auto stream_state = r.Bytes(callbacks, 0x800);
    const auto arena = r.Bytes(s + 0x118, 0x10);
    const auto images = r.Bytes(s + 0x1c8, 0x3d8);
    const auto driver = r.Bytes(Rig::kDecoder + 0x48, 0x118);
    for (uint32_t offset : {0u, 4u, 8u, 0x98u, 0xc0u, 0x128u, 0x1c0u, 0x828u, 0x858u, 0x878u})
        r.Write32(s + offset, 99);
    r.Write32(Rig::kDecoder + 8, 123);
    r.Args(Rig::kDecoder);
    r.Call(Decoder::kReset);
    CHECK(r.Result() == 1, "owned Reset succeeds");
    for (uint32_t offset : {0u, 4u, 8u, 0xc0u, 0x128u, 0x1c0u, 0x858u, 0x878u})
        CHECK(r.Read32(s + offset) == 0, "Reset clears software playback state");
    CHECK(r.Read32(s + 0x98) == 0xffffffffu && r.Read32(s + 0x828) == 1, "Reset restores timestamp/cache sentinels");
    CHECK(r.Read32(Rig::kDecoder + 8) == 0, "Reset clears public frame count");
    CHECK(r.Read32(Rig::kDecoder + 0x40) == s, "Reset retains the same work allocation");
    CHECK(r.Bytes(s + 0xc, 0x80) == callback_state, "Reset preserves registered callbacks and table pointer/count");
    CHECK(r.Bytes(callbacks, 0x800) == stream_state, "Reset preserves stream registrations");
    CHECK(r.Bytes(s + 0x118, 0x10) == arena, "Reset preserves arena next and mark");
    CHECK(r.Bytes(s + 0x1c8, 0x3d8) == images, "Reset preserves reference descriptors");
    CHECK(r.Bytes(Rig::kDecoder + 0x48, 0x118) == driver, "Reset preserves driver buffers/mode/counter");
    CHECK(r.create_calls == 1 && r.reset_calls == 2, "explicit Reset calls native reset once");
}

void TestInvalidReset() {
    for (uint32_t sys : {0u, Rig::kRamSize - 0x100u, 0xfffffff0u}) {
        Rig r;
        r.Write32(Rig::kDecoder + 0x40, sys);
        const auto before = r.ram;
        r.Args(Rig::kDecoder);
        r.Call(Decoder::kReset);
        CHECK(r.Result() == 0 && r.ram == before, "invalid Reset has no guest writes");
        CHECK(r.reset_calls == 0 && r.Count("invalid_work") == 1, "invalid Reset does not mutate native playback");
    }
}

void TestPictureFormatsAndBuffering() {
    for (uint32_t format : {0x100u, 0x101u, 0x102u, 0x103u}) {
        for (uint32_t double_buffer : {0u, 1u, 7u}) {
            Rig r;
            r.Create();
            r.Write32(Rig::kDecoder + 0x154, format);
            r.Write32(Rig::kDecoder + 0x158, double_buffer);
            const auto snapshot = r.Bytes(Rig::kDecoder + 0x70, 0x24);
            const bool rgb16 = format >= 0x102;
            for (uint32_t count = 0; count < 3; ++count) {
                const uint32_t target = count == 1 && double_buffer != 0 ? Rig::kPlane1 : Rig::kPlane0;
                const uint32_t other = target == Rig::kPlane0 ? Rig::kPlane1 : Rig::kPlane0;
                const uint32_t previous_other = r.Read32(other);
                r.Args(0xdead, 0xbeef, 0xbad);  // private function obtains D from the global
                r.Call(Decoder::kDecodePicture);
                CHECK(r.Result() == 1, "SDK zero/positive picture result becomes driver success");
                CHECK(r.Read32(Rig::kDecoder + 0x54) == count + 1, "one increment per served picture");
                const auto& call = r.pictures.back();
                CHECK(call.rgb16 == rgb16 && call.decoder == Rig::kDecoder, "correct native service/context");
                CHECK(call.argument_buffer == (rgb16 ? Rig::kRaw8 : target), "SDK buffer argument");
                CHECK(call.macroblocks == 6 && call.decoded == count, "service sees geometry and pre-increment count");
                CHECK(call.destination == target && r.Read32(target) == 0x70000001u + count, "selected final plane written");
                CHECK(r.Read32(other) == previous_other, "unselected plane unchanged");
                CHECK(r.Filled(target - 16, 16, Rig::kGuard) && r.Filled(target + 4, 16, Rig::kGuard),
                      "picture hook guards intact");
            }
            CHECK(r.Filled(Rig::kRaw8, 32, Rig::kGuard), "RGB16 service supplies final pixels without RAW8/CSC staging");
            CHECK(r.Bytes(Rig::kDecoder + 0x70, 0x24) == snapshot, "picture serve does not save/restore DMA state");
            CHECK(r.Count("pictures") == 3 && r.Count("rgb16") == (rgb16 ? 3u : 0u), "picture census by format");
            CHECK(r.originals.empty() && r.invalid_accesses == 0, "native picture stays inside software boundary");
        }
    }
}

void TestNoPictureAndEof() {
    for (uint32_t format : {0x100u, 0x102u}) {
        Rig r;
        r.Create();
        r.Write32(Rig::kDecoder + 0x154, format);
        r.Write32(Rig::kDecoder + 0x54, 7);
        r.fail_picture = true;
        const auto before = r.ram;
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Result() == 0 && r.ram == before, "SDK -1 leaves count and planes untouched");
        CHECK(r.pictures.size() == 1 && r.pictures.back().destination == Rig::kPlane1, "failed serve selects expected plane");
        r.fail_picture = false;
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Result() == 1 && r.Read32(Rig::kDecoder + 0x54) == 8, "retry serves the same picture slot once");
        CHECK(r.pictures.back().destination == Rig::kPlane1, "failure did not advance double buffering");
        r.Write32(Rig::kWork, 1);
        const auto at_eof = r.ram;
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Result() == 0 && r.ram == at_eof, "EOF returns no picture without mutation");
        CHECK(r.pictures.size() == 2, "EOF does not invoke decoder service");
        CHECK(r.Count("pictures") == 1 && r.Count("nopic") == 2, "failure and EOF census");
    }
    for (uint32_t sys : {0u, Rig::kRamSize - 2u, 0xfffffff0u}) {
        Rig r;
        r.Write32(Rig::kDecoder + 0x40, sys);
        const auto before = r.ram;
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Result() == 0 && r.ram == before && r.pictures.empty(), "missing EOF word cannot serve/increment");
        CHECK(r.Count("nopic") == 1, "invalid picture context is visible");
    }
}

void CheckFallback(Rig& r, uint32_t function, uint32_t argument) {
    r.Args(argument, 0x1234, 0x5678);
    const auto before = r.ram;
    const size_t calls = r.originals.size();
    r.Call(function);
    CHECK(r.Result() == Rig::kOriginalResult && r.originals.size() == calls + 1, "original handler owns fallback result");
    const auto& original = r.originals.back();
    CHECK(original.function == function && original.entry == function, "fallback uses matching original start");
    CHECK((original.arguments == std::array<uint32_t, 3>{argument, 0x1234, 0x5678}), "fallback forwards arguments unchanged");
    CHECK(r.ram == before && r.events.empty(), "fallback does not partially execute native lifecycle");
}

void TestOwnershipFallbacks() {
    for (uint32_t format : {0u, 1u, 3u}) {
        Rig r;
        r.Write32(Rig::kDecoder + 0x148, format);
        for (uint32_t function : {Decoder::kCreate, Decoder::kReset, Decoder::kDecodePicture})
            CheckFallback(r, function, Rig::kDecoder);
        CHECK(r.Count("fallback") == 3, "other-format fallbacks counted");
    }
    Rig foreign;
    foreign.Write32(Rig::kDecoder + 0x1000 + 0x148, 2);
    CheckFallback(foreign, Decoder::kCreate, Rig::kDecoder + 0x1000);
    CheckFallback(foreign, Decoder::kReset, Rig::kDecoder + 0x1000);
    CHECK(foreign.Count("fallback") == 2, "same-format foreign SDK contexts retain original handlers");
    for (uint32_t active : {0u, Rig::kRamSize - 0x80u}) {
        Rig r;
        r.Write32(Decoder::kDecoderPointer, active);
        CheckFallback(r, Decoder::kDecodePicture, 0x1234);
    }
}

void TestContinuationsAndZeroEntry() {
    Rig r;
    const auto before = r.ram;
    for (uint32_t function : {Decoder::kCreate, Decoder::kReset, Decoder::kDecodePicture,
                              Decoder::kInitialise, Decoder::kStart, Decoder::kFinished, Decoder::kClose}) {
        r.Args(Rig::kDecoder, Rig::kWork, 0x2800);
        r.context.cpuRegs.GPR.r[29].UD[0] = 0x70003fc0;
        r.context.cpuRegs.GPR.r[31].UD[0] = 0x123456;
        r.installed.at(function)(r.context, function + 4);
        CHECK(r.originals.back().entry == function + 4, "interior continuation keeps its original address");
        CHECK(r.originals.back().function == function && r.Result() == Rig::kOriginalResult, "matching original executes continuation");
        CHECK(r.context.cpuRegs.GPR.r[29].UD[0] == 0x70003fc0 &&
              r.context.cpuRegs.GPR.r[31].UD[0] == 0x123456, "continuation forwarding preserves stack/return registers");
    }
    CHECK(r.ram == before && r.events.empty(), "restored continuations bypass native work entirely");
    CHECK(r.Count("continuations") == 7 && r.Count("fallback") == 0, "continuations and ownership fallbacks distinct");
    r.Args(Rig::kDecoder, Rig::kWork, 0x2800);
    r.installed.at(Decoder::kCreate)(r.context, 0);
    CHECK(r.Result() == 1 && r.create_calls == 1, "zero address means normal Create entry");
    r.Args(Rig::kDecoder);
    r.installed.at(Decoder::kReset)(r.context, 0);
    CHECK(r.Result() == 1 && r.reset_calls == 2, "zero address means normal Reset entry");
    r.installed.at(Decoder::kDecodePicture)(r.context, 0);
    CHECK(r.Result() == 1 && r.pictures.size() == 1, "zero address means normal DecodePicture entry");
}

void TestInstallRequiresDependencies() {
    Rig r;
    int installed = 0;
    const Decoder::Installer install = [&installed](uint32_t, Decoder::Body) { ++installed; return true; };
    Decoder missing_body(r, r.MakeHooks());
    CHECK(missing_body.Install([](uint32_t) { return Decoder::Body{}; }, install) == 0,
          "installation refuses missing original handlers");
    auto hooks = r.MakeHooks();
    hooks.picture_rgb16 = {};
    Decoder missing_hook(r, std::move(hooks));
    CHECK(missing_hook.Install(r.Capture(), install) == 0, "installation refuses incomplete native picture service");
    for (unsigned missing = 0; missing < 4; ++missing) {
        auto incomplete = r.MakeHooks();
        if (missing == 0) incomplete.initialize = {};
        if (missing == 1) incomplete.destroy = {};
        if (missing == 2) incomplete.call = {};
        if (missing == 3) incomplete.has_pending = {};
        Decoder lifecycle(r, std::move(incomplete));
        CHECK(lifecycle.Install(r.Capture(), install, true) == 0, "opt-in installation requires every lifecycle service");
    }
    CHECK(installed == 0, "dependency failure installs no partial boundary");
}

void TestDefaultInstallCompatibility() {
    Rig r;
    auto hooks = r.MakeHooks();
    hooks.initialize = {};
    hooks.destroy = {};
    hooks.call = {};
    hooks.has_pending = {};
    hooks.volume = {};
    Decoder baseline(r, std::move(hooks));
    r.installed.clear();
    const auto original_capture = r.Capture();
    const Decoder::Capture capture = [&original_capture](uint32_t address) {
        CHECK(address == Decoder::kCreate || address == Decoder::kReset || address == Decoder::kDecodePicture,
              "default install never asks for lifecycle bodies");
        return original_capture(address);
    };
    CHECK(baseline.Install(capture, r.Installer()) == 3, "four original hooks still install the original three entries");
    CHECK(r.installed.size() == 3 && r.installed.count(Decoder::kInitialise) == 0 &&
          r.installed.count(Decoder::kStart) == 0 && r.installed.count(Decoder::kFinished) == 0 &&
          r.installed.count(Decoder::kClose) == 0,
          "preparatory lifecycle stays unbound by default");
    r.Create();
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && r.create_calls == 1 && r.pictures.size() == 1, "baseline installed handlers remain functional");
    r.Write32(Rig::kDecoder + 0x15c, 1);
    r.Write32(Decoder::kSoundStarted, 0);
    r.fail_picture = true;
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && r.Read32(Rig::kDecoder + 0x54) == 1,
          "default installed picture path keeps EOF no-picture retryable with empty lifecycle hooks");
    r.fail_picture = false;
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && r.Read32(Rig::kDecoder + 0x54) == 2,
          "default path can serve after EOF no-picture without lifecycle ownership");
    CHECK(r.initialize_calls == 0 && r.destroyed.empty() && r.services.empty(), "baseline never enters optional services");
}

void TestInitializeAndClose() {
    Rig r;
    const auto before = r.ram;
    r.Call(Decoder::kInitialise);
    CHECK(r.initialize_calls == 1 && r.Count("initialized") == 1, "Initialise invokes software init once");
    CHECK(r.ram == before && r.services.empty(), "Initialise does not publish transport or stack state");
    r.Create();
    const auto opened = r.ram;
    r.Call(Decoder::kClose);
    CHECK((r.destroyed == std::vector<uint32_t>{Rig::kDecoder}), "Close releases the active host playback");
    CHECK(r.Result() == 1 && r.Count("closed") == 1, "Close successful census");
    CHECK(r.ram == opened, "Close preserves public and private guest context as retail Delete does");
    CHECK(r.reset_calls == 1 && r.create_calls == 1 && r.services.empty(), "Close does not implicitly reset or run guest services");
    r.events.clear();
    r.Write32(Rig::kDecoder + 0x148, 1);
    CheckFallback(r, Decoder::kClose, 0x99);
}

// A5 qol.skip_movies: RequestSkip on an owned playback makes the NEXT Finished
// poll answer 1 (the title's Step then takes its own Stop path); before Start it
// is a no-op; Close and a new Start clear it.
void TestSkipRequest() {
    Rig r;
    CHECK(!r.decoder->Playing(), "nothing plays before Start");
    r.decoder->RequestSkip();
    CHECK(r.Count("skips") == 0, "a skip before any playback is ignored");
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    CHECK(r.decoder->Playing(), "Start owns a playback");
    r.Write32(Rig::kDecoder + 0x15c, 0);
    r.pending_playback = true;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0, "a playing movie has not ended");
    r.decoder->RequestSkip();
    CHECK(r.Count("skips") == 1, "the skip is counted");
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "after RequestSkip the next Finished poll answers 1 (the owned Movie stop)");
    r.Call(Decoder::kClose);
    CHECK(!r.decoder->Playing(), "Close ends the playback");
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    r.Write32(Rig::kDecoder + 0x15c, 0);
    r.pending_playback = true;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0, "a new Start clears the skip request");
    r.Call(Decoder::kClose);
}

void TestPersistenceLifetime() {
    Rig r;
    CHECK(r.decoder->CanCheckpoint(), "idle movie owner permits root quiescence");
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    std::string reason;
    CHECK(!r.decoder->CanCheckpoint(&reason) && !reason.empty(), "successful Start owns the player buffers");
    CHECK(!r.service_checkpoint_ready.empty() &&
          std::none_of(r.service_checkpoint_ready.begin(), r.service_checkpoint_ready.end(), [](bool v) { return v; }),
          "preparation is not checkpointable before compressed input reaches the provider");
    r.Write32(Rig::kDecoder + 0x15c, 1);
    r.pending_playback = false;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1 && !r.decoder->CanCheckpoint(), "picture EOF does not end player buffer ownership");
    r.fail_picture = true;
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && !r.decoder->CanCheckpoint(), "terminal decoder error still waits for player Close");
    r.Call(Decoder::kClose);
    CHECK(r.decoder->CanCheckpoint(), "Close releases root movie quiescence");
    r.fail_picture = false;
    r.Arena();
    r.StartArgs();
    r.fail_demux = true;
    r.Call(Decoder::kStart);
    CHECK(r.Result() == 0 && r.decoder->CanCheckpoint(), "failed preparation releases all owned buffers");
    r.fail_demux = false;
    r.Arena();
    r.StartArgs();
    r.throw_seek = true;
    bool threw = false;
    try { r.Call(Decoder::kStart); } catch (int) { threw = true; }
    CHECK(threw && r.decoder->CanCheckpoint(), "exception cleanup releases preparation quiescence");
    r.throw_seek = false;
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    CHECK(!r.decoder->CanCheckpoint(), "reopen reacquires player lifetime");
    const auto restored_ram = r.ram;
    const int initialized = r.initialize_calls;
    r.decoder->ResetAfterRestore();
    CHECK(r.ram == restored_ram && r.initialize_calls == initialized + 1,
          "restore discards unpersisted software context without altering restored guest bytes");
    CHECK(r.decoder->CanCheckpoint() && r.Count("restore_resets") == 1, "restore invalidates old playback ownership");
    const size_t pictures = r.pictures.size();
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && r.pictures.size() == pictures, "restored stale descriptor cannot resurrect prior playback");
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && !r.decoder->CanCheckpoint(), "post-restore Start reacquires normal picture ownership");
    r.Call(Decoder::kClose);
    auto hooks = r.MakeHooks();
    hooks.initialize = {};
    Decoder baseline(r, hooks);
    CHECK(baseline.Install(r.Capture(), r.Installer()) == 3, "picture-only owner still installs without lifecycle");
    baseline.ResetAfterRestore();
    CHECK(baseline.CanCheckpoint(), "picture-only owner leaves root's existing provider quiescence responsible");
}

void CheckCaller(const Rig& r, const GPRregs& saved) {
    for (uint32_t index : {16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 28u, 29u, 30u, 31u}) {
        CHECK(r.context.cpuRegs.GPR.r[index].UD[0] == saved.r[index].UD[0], "callee-save low register half preserved");
        CHECK(r.context.cpuRegs.GPR.r[index].UD[1] == saved.r[index].UD[1], "callee-save upper register half preserved");
    }
}

void TestStartSoftwareContract() {
    for (uint32_t output : {0x100u, 0x101u, 0x102u, 0x103u}) {
        for (uint32_t buffers : {0u, 7u}) {
            Rig r;
            r.Arena(Rig::kArena + 3);
            r.StartArgs(true, output, buffers);
            const auto saved = r.context.cpuRegs.GPR;
            const auto snapshot = r.Bytes(Rig::kDecoder + 0x70, 0x24);
            const auto stack_args = r.Bytes(Rig::kStack, 0x20);
            r.Call(Decoder::kStart);
            CHECK(r.Result() == 1 && r.Count("started") == 1 && r.Count("start_failed") == 0, "MPEG2 PSS Start succeeds");
            CheckCaller(r, saved);
            CHECK(r.Bytes(Rig::kStack, 0x20) == stack_args, "eight-byte stack arguments untouched");
            CHECK(r.Bytes(Rig::kDecoder + 0x70, 0x24) == snapshot, "Start does not snapshot IPU/DMA");
            CHECK(r.Read32(Rig::kDecoder + 0x48) == Rig::kArena + 0x40, "read buffer honors 64-byte arena alignment");
            CHECK(r.Read32(Rig::kDecoder + 0xf0) == Rig::kArena + 0x4040, "video allocation follows 16KB read buffer");
            CHECK(r.Read32(Rig::kDecoder + 0xf4) == 0x40000, "video ring retains software capacity");
            const uint32_t video = r.Read32(Rig::kDecoder + 0xf0);
            CHECK(r.Read32(Rig::kDecoder + 0xfc) == video &&
                  r.Read32(Rig::kDecoder + 0xf8) == video + 0x40 &&
                  r.Read32(Rig::kDecoder + 0x100) == video + 0x40, "software ring cursors and sequence anchor");
            CHECK(r.Read32(Rig::kDecoder + 0x104) == Rig::kArena + 0x44040 &&
                  r.Read32(Rig::kDecoder + 0x108) == 0x19000, "PCM allocation follows video ring");
            for (const auto [offset, value] : std::array<std::array<uint32_t, 2>, 12>{{
                     {0x60, Rig::kRead}, {0x64, Rig::kSeek}, {0x68, Rig::kPath}, {0x6c, 10},
                     {0x144, 1}, {0x148, 2}, {0x14c, 1}, {0x150, 7},
                     {0x154, output}, {0x158, buffers}, {0x58, 32}, {0x5c, 48}}})
                CHECK(r.Read32(Rig::kDecoder + offset) == value, "Start publishes source-defined decoder field");
            CHECK(r.Read32(Rig::kDecoder + 0x54) == 0 && r.Read32(Rig::kDecoder + 0x15c) == 0,
                  "Start begins with no served pictures and no EOF");
            CHECK(r.create_calls == 2 && r.reset_calls == 2, "temporary and persistent Create retain reset policy");
            CHECK(r.services[0].function == Rig::kSeek && r.services[0].arguments[0] == Rig::kPath,
                  "seek sees the ninth ABI argument, and its negative return is ignored");
            CHECK(r.services[1].sys == Rig::kStack - 0x28d0, "initial callback table lives in checked temporary work");
            const uint32_t sys = r.Read32(Rig::kDecoder + 0x40);
            CHECK(sys == Rig::kArena + 0x5d040 && sys == r.Read32(Rig::kDecoder + 0xa4),
                  "persistent context replaces temporary one after sequence discovery");
            CHECK(r.Read32(sys + 0xc) == 0x2b1240 && r.Read32(sys + 0x14) == 0x12348000,
                  "software error callback captures caller GP");
            CHECK(r.Filled(sys + 0x18, 0x3c, 0), "no no-data, DMA or background hardware callbacks installed");
            for (uint32_t offset : {0x5a0u, 0x5a4u, 0x6e0u, 0x6e4u, 0x830u})
                CHECK(r.Read32(sys + offset) == 0, "no retired scratchpad buffer addresses");
            CHECK(r.Read32(r.Read32(sys + 0x60) + 0x10) == 0x55550000, "persistent stream registration survives second Create");
            const bool rgb16 = output >= 0x102;
            uint32_t next = sys + 0x4300;  // 32*48*9/2 plus SDK software work extent
            if (rgb16) {
                CHECK(r.Read32(Rig::kDecoder + 0x9c) == next, "RGB16 RAW8 argument allocation retained for service ABI");
                next += 6 * 0x180;
            }
            const uint32_t count = buffers ? 2 : 1;
            for (uint32_t i = 0; i < count; ++i) {
                CHECK(r.Read32(Rig::kDecoder + 0x94 + i * 4) == next, "picture plane allocation order");
                next += 6 * (rgb16 ? 0x200 : 0x400);
                CHECK(r.Read32(Rig::kDecoder + 0xa8 + i * 0x20) == next, "column-upload allocation order");
                const auto& upload = r.services[4 + i];
                CHECK(upload.function == 0x2b1448 && upload.arguments[4] == (rgb16 ? 2u : 0u),
                      "retained upload builder receives output pixel format");
                next += 0x140;  // source UploadBytes(2 columns)
                if (output & 1u) CHECK(r.Read32(Rig::kDecoder + 0xe8 + i * 4) == 0, "image-format stub result retained");
            }
            CHECK(r.Read32(Decoder::kArenaNext) == next && r.Filled(next, 32, Rig::kGuard),
                  "allocator publishes exact end without touching following guard");
            CHECK(r.Filled(Rig::kArena, 0x40, Rig::kGuard), "leading arena alignment padding preserved");
            CHECK(r.originals.empty() && r.invalid_accesses == 0, "supported Start never calls generated transport");
            r.Call(Decoder::kDecodePicture);
            CHECK(r.Result() == 1 && r.pictures.back().rgb16 == rgb16, "newly started context serves through existing picture hook");
        }
    }
}

void TestRestartAndAudioModes() {
    for (uint32_t audio : {0u, 1u, 2u, 3u}) {
        Rig r;
        r.Arena();
        r.StartArgs(true, 0x100, 0, audio);
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 1, "all defined audio modes can start MPEG2");
        if (audio != 0) CHECK(r.Read32(Rig::kDecoder + 0x108) == (audio == 2 ? 0x8000u : 0x19000u), "audio ring uses mode-specific capacity");
        const uint32_t sys = r.Read32(Rig::kDecoder + 0x40);
        const auto arena = r.Read32(Decoder::kArenaNext);
        const auto planes = r.Bytes(Rig::kDecoder + 0x94, 0x60);
        const auto table = r.Bytes(r.Read32(sys + 0x60), 0x800);
        r.Write32(Rig::kDecoder + 0x54, 17);
        r.Write32(Rig::kDecoder + 0x15c, 1);
        r.Write32(sys, 1);
        r.services.clear();
        r.events.clear();
        r.StartArgs(false, 0x100, 0, audio);
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 1 && r.Count("started") == 2, "restart succeeds without reallocating");
        CHECK(r.Read32(Decoder::kArenaNext) == arena && r.Read32(Rig::kDecoder + 0x40) == sys,
              "restart retains work and arena ownership");
        CHECK(r.Bytes(Rig::kDecoder + 0x94, 0x60) == planes && r.Bytes(r.Read32(sys + 0x60), 0x800) == table,
              "restart preserves pictures, upload packets and registrations");
        CHECK((r.events == std::vector<char>{'K', 'D', 'R'}), "restart seeks and demuxes before one existing playback reset");
        CHECK(r.Read32(Rig::kDecoder + 0x54) == 0 && r.Read32(sys) == 0, "restart clears picture count and EOF");
        if (audio != 0) CHECK(r.Read32(Rig::kDecoder + 0x108) == 0, "NTSC restart clears audioSize without reallocating audio");
    }
}

void TestStartFailuresAndFallback() {
    for (uint32_t limit : {Rig::kArena, Rig::kArena + 0x4000u, Rig::kArena + 0x44000u,
                           Rig::kArena + 0x5d000u, Rig::kArena + 0x61300u, Rig::kArena + 0x62b00u}) {
        Rig r;
        r.Arena(Rig::kArena, limit);
        r.StartArgs();
        const auto saved = r.context.cpuRegs.GPR;
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 0 && r.Count("start_failed") == 1, "each allocation exhaustion returns visible failure");
        CHECK(r.Read32(Decoder::kArenaNext) <= limit, "failed allocation never advances past arena limit");
        CHECK(r.Filled(limit, 32, Rig::kGuard), "failed allocation leaves guard at limit intact");
        CheckCaller(r, saved);
        CHECK(r.originals.empty() && r.invalid_accesses == 0, "allocation failure never falls into hardware body");
    }
    for (uint32_t stack : {0u, 0x2000u, Rig::kRamSize - 8u, 0xfffffff0u}) {
        Rig r;
        r.Arena();
        r.StartArgs();
        r.context.cpuRegs.GPR.r[29].UD[0] = stack;
        const auto before = r.ram;
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 0 && r.ram == before && r.events.empty(), "bad stack extent rejected before guest mutation or callbacks");
        CHECK(r.Count("invalid_work") == 1 && r.invalid_accesses == 0, "bad stack does not issue invalid reads");
    }
    for (uint32_t next : {0u, Rig::kRamSize - 0x1000u, 0xfffffff1u}) {
        Rig r;
        r.Arena(next, 0xffffffffu);
        r.StartArgs();
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 0 && r.Read32(Decoder::kArenaNext) == next,
              "null, unbacked and wrapping allocations never publish a new cursor");
        CHECK(r.Count("start_failed") == 1 && r.services.empty() && r.invalid_accesses == 0,
              "invalid arena is rejected before retained services");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        Rig r;
        r.Arena();
        r.StartArgs();
        if (mode == 0) r.context.cpuRegs.GPR.r[6].UD[0] = 0;  // raw container
        if (mode == 1) r.context.cpuRegs.GPR.r[7].UD[0] = 1;  // IPU columns
        if (mode == 2) r.context.cpuRegs.GPR.r[8].UD[0] = 4;  // foreign audio format
        if (mode == 3) r.Write32(Rig::kStack + 0x18, 0x200);
        const auto before = r.ram;
        r.Call(Decoder::kStart);
        CHECK(r.Result() == Rig::kOriginalResult && r.originals.size() == 1 &&
              r.events.empty() && r.ram == before, "unsupported Start goes to original without partial setup");
    }
    Rig eof;
    eof.Arena();
    eof.StartArgs();
    eof.fail_demux = true;
    eof.Call(Decoder::kStart);
    CHECK(eof.Result() == 0 && eof.create_calls == 1 && eof.pictures.empty(), "initial EOF never creates final work or serves a picture");
    CHECK(eof.Read32(Rig::kDecoder + 0x15c) == 1 && eof.Count("start_failed") == 1, "demux EOF and failed start remain observable");
    for (uint32_t extent : {0u, 15u, 17u, 4095u}) {
        Rig r;
        r.Arena();
        r.StartArgs();
        r.sequence_width = r.sequence_height = extent;
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 0 && r.create_calls == 1 && r.pictures.empty(),
              "zero, sub-macroblock and unallocatable dimensions do not publish final work");
        CHECK(r.Count("start_failed") == 1 && r.invalid_accesses == 0, "bad dimensions fail within mapped memory");
    }
    Rig exception;
    exception.Arena();
    exception.StartArgs();
    exception.throw_seek = true;
    const auto saved = exception.context.cpuRegs.GPR;
    bool caught = false;
    try { exception.Call(Decoder::kStart); } catch (int) { caught = true; }
    CHECK(caught && exception.Count("start_failed") == 1, "guest service exception propagates and is counted");
    CheckCaller(exception, saved);
}

void TestFinishedBacklogAndClose() {
    Rig r;
    r.Create();
    r.Write32(Rig::kDecoder + 0x15c, 0);
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0, "a waiting decoder has not ended");
    r.Write32(Rig::kDecoder + 0x15c, 1);
    r.pending_playback = true;
    const auto before = r.ram;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0 && r.ram == before, "file EOF cannot discard accepted compressed data or pictures");
    CHECK(r.Count("eof_deferred") == 1, "early refill EOF is counted at the owning boundary");
    r.Call(Decoder::kDecodePicture);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.pictures.size() == 2 && r.Read32(Rig::kDecoder + 0x54) == 2,
          "player's two priming serves remain available after refill EOF");
    CHECK(r.pictures[0].destination == Rig::kPlane0 && r.pictures[1].destination == Rig::kPlane1,
          "priming owns two distinct selected output buffers");
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0, "two priming pictures do not imply movie completion");
    r.pending_playback = false;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "EOF completes only when the software backlog has drained");
    r.Write32(Rig::kDecoder + 0x15c, 0);
    r.Write32(Rig::kWork, 1);
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "inner software end without a refill EOF also completes");
    r.Write32(Rig::kWork, 0);
    r.pending_playback = true;
    r.Call(Decoder::kClose);
    r.Call(Decoder::kClose);
    CHECK(r.destroyed.size() == 1 && !r.pending_playback, "skip/close releases the playback once");
    const auto count = r.pictures.size();
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && r.pictures.size() == count, "closed descriptor cannot recreate a playback by serving");
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "closed playback ends even though retail context words are retained");
    r.Args(Rig::kDecoder);
    r.Call(Decoder::kReset);
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0, "explicit reset reopens the same descriptor");
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && r.pictures.size() == count + 1, "reset permits serving again");
    r.Write32(Rig::kDecoder + 0x148, 1);
    r.events.clear();
    CheckFallback(r, Decoder::kFinished, 0x123);
}

void TestPrimingAudioEof() {
    Rig r;
    r.Create();
    r.Write32(Decoder::kSoundStarted, 0);  // player's Prepare, before its two serves
    r.Write32(Rig::kDecoder + 0x15c, 0);
    r.pending_playback = true;
    r.Call(Decoder::kDecodePicture);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.pictures.size() == 2, "prime has two ready pictures, not a drained movie");

    // Source player.cpp Open: while (!Finished()) { ReadOn(); FeedAudio(); }
    // The software PCM queue is populated directly; the retired IOP ring's
    // fullness stays zero. A bounded fixture reproduces that exact dependency.
    unsigned fills = 0;
    for (; fills < 4; ++fills) {
        r.Call(Decoder::kFinished);
        if (r.Result() != 0) break;
        if (fills == 1) r.Write32(Rig::kDecoder + 0x15c, 1);  // read provider reaches EOF
    }
    CHECK(fills == 2, "initial audio-fill exits at EOF despite accepted video backlog");
    CHECK(r.pending_playback && r.destroyed.empty() && r.pictures.size() == 2,
          "releasing the fill wait neither ends the playback nor discards its pictures");
    CHECK(r.Count("priming_eof") == 1 && r.Count("eof_deferred") == 0,
          "priming EOF and playback drain are separate service outcomes");
    r.Write32(Decoder::kSoundStarted, 1);  // source StartSound, after the fill loop
    const auto playing = r.ram;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 0 && r.ram == playing, "same EOF now defers to accepted playback without a gameplay write");
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && r.pictures.size() == 3, "playback continues past the two priming pictures");
    r.pending_playback = false;
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "genuine drained EOF completes after sound starts");

    // Muting PCM is still a started stream. No-audio movies have no audio-fill
    // loop at all and must not inherit a false end from its exception.
    Rig silent;
    silent.Create();
    silent.Write32(Decoder::kSoundStarted, 0);
    silent.Write32(Rig::kDecoder + 0x14c, 0);
    silent.Write32(Rig::kDecoder + 0x15c, 1);
    silent.pending_playback = true;
    silent.Call(Decoder::kFinished);
    CHECK(silent.Result() == 0 && silent.Count("priming_eof") == 0,
          "no-audio playback drains pictures without requiring a sound-start flag");
}

void TestFinishedServiceOracle() {
    // Source MovieDecoding::Finished plus the existing NEW abort hold at
    // playback: compare identical service inputs, without mutating gameplay.
    // The initial fill uses the source predicate alone; the scene's hold never
    // ran inside that synchronous open call.
    for (uint32_t audio : {0u, 1u}) {
        for (uint32_t started : {0u, 1u}) {
            for (uint32_t refill : {0u, 1u}) {
                for (uint32_t inner : {0u, 1u}) {
                    for (bool pending : {false, true}) {
                        Rig r;
                        r.Create();
                        r.Write32(Rig::kDecoder + 0x14c, audio);
                        r.Write32(Decoder::kSoundStarted, started);
                        r.Write32(Rig::kDecoder + 0x15c, refill);
                        r.Write32(Rig::kWork, inner);
                        r.pending_playback = pending;
                        const bool initial_fill = audio != 0 && started == 0;
                        uint32_t old_abort = refill;
                        if (!initial_fill && pending) old_abort = 0;
                        const bool expected = old_abort != 0 || inner != 0;
                        const auto before = r.ram;
                        r.Call(Decoder::kFinished);
                        CHECK((r.Result() != 0) == expected,
                              "same NEW service input preserves old finished/abort semantics");
                        CHECK(r.ram == before && r.originals.empty(),
                              "finished replacement needs no scene state patch or retired body");
                    }
                }
            }
        }
    }
}

void TestOwnedDecodeErrorAtEof() {
    Rig r;
    r.Arena();
    r.StartArgs();
    r.Call(Decoder::kStart);
    CHECK(r.Result() == 1, "decode-error fixture starts through lifecycle owner");
    r.pending_playback = true;
    r.fail_picture = true;
    const auto planes = r.Bytes(r.Read32(Rig::kDecoder + 0x94), 6 * 0x400);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && r.destroyed.empty(), "no-picture while input is open remains retryable");
    CHECK(r.Read32(Rig::kDecoder + 0x54) == 0 && r.Count("decode_errors") == 0,
          "no-picture must never pretend that a frame was served");
    r.Write32(Rig::kDecoder + 0x15c, 1);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 0 && r.destroyed.size() == 1 && !r.pending_playback,
          "failed decode after input EOF releases pictures and PCM instead of hanging");
    CHECK(r.Count("decode_errors") == 1 && r.Read32(Rig::kDecoder + 0x54) == 0 &&
          r.Bytes(r.Read32(Rig::kDecoder + 0x94), 6 * 0x400) == planes,
          "terminal decode error publishes neither a bogus picture nor a buffer write");
    r.Call(Decoder::kFinished);
    CHECK(r.Result() == 1, "malformed terminal input cannot keep the service playing forever");
    r.Call(Decoder::kClose);
    CHECK(r.destroyed.size() == 1, "close after decode error is idempotent");
    r.Arena();
    r.StartArgs();
    r.fail_picture = false;
    r.Call(Decoder::kStart);
    r.Call(Decoder::kDecodePicture);
    CHECK(r.Result() == 1 && r.Read32(Rig::kDecoder + 0x54) == 1, "decode-error close permits a clean reopen");

    Rig base;
    base.Create();
    base.fail_picture = true;
    base.Write32(Rig::kDecoder + 0x15c, 1);
    base.Call(Decoder::kDecodePicture);
    CHECK(base.Result() == 0 && base.destroyed.empty() && base.Count("decode_errors") == 0,
          "existing public picture path is unaffected until Start transfers lifecycle ownership");
    base.fail_picture = false;
    base.Call(Decoder::kDecodePicture);
    CHECK(base.Result() == 1, "unowned public picture path still retries after negative result");
}

void TestStreamServices() {
    TestMovieDecoder f;
    constexpr uint32_t gp = 0x320000, buffer = 0x300000;
    f.context.cpuRegs.GPR.r[28].UD[0] = gp;
    f.Write32(f.kDecoder + 0x144, 1);
    f.Write32(f.kDecoder + 0x60, Decoder::kReadStream);
    f.Write32(f.kDecoder + 0x64, Decoder::kSeekStream);
    f.Write32(gp - Decoder::kFileLocationGp, 123);
    f.Write32(gp - Decoder::kFileSizeGp, 3 * 2048);
    f.Write32(gp - Decoder::kReadPositionGp, 4096);
    unsigned cursor = 0, reads = 0, seeks = 0;
    int32_t seek_result = 1;
    int32_t read_error = 0;
    Decoder::StreamServices services{
        [&](uint32_t location) {
            ++seeks;
            CHECK(location == 123, "seek uses title file location, not a decoder-ring address");
            if (seek_result > 0) cursor = 0;
            return seek_result;
        },
        [&](uint8_t* dst, uint32_t count) {
            ++reads;
            if (read_error) return read_error;
            // Short successful reads are legal; the cursor also belongs to
            // picture read-ahead, independently of the title refill count.
            const uint32_t got = std::min(count, cursor < 3 ? 1u : 0u);
            if (got) std::fill_n(dst, 2048, static_cast<uint8_t>(++cursor));
            return static_cast<int32_t>(got);
        }};
    CHECK(f.decoder->InstallStream(f.Capture(), f.Installer(), services) == 2, "two stream adapters installed");
    CHECK(f.installed.size() == 9, "stream opt-in leaves existing lifecycle installation intact");
    CHECK(f.decoder->InstallStream(f.Capture(), f.Installer(), services) == 0, "duplicate install cannot capture itself");
    f.decoder->SeekStream(f.context);
    CHECK(f.Result() == 1 && seeks == 1 && f.Read32(gp - Decoder::kReadPositionGp) == 0, "seek resets title read count");
    f.Args(buffer, 4097);
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == 2048 && f.Filled(buffer, 2048, 1) && f.Filled(buffer + 2048, 2049, f.kGuard),
          "short sector read publishes only delivered bytes, with guards intact");
    CHECK(f.Read32(gp - Decoder::kReadPositionGp) == 2048, "title position advances by returned bytes");
    std::array<uint8_t, 2048> ahead{};
    CHECK(services.read(ahead.data(), 1) == 1 && ahead[0] == 2, "decoder read-ahead advances the same provider cursor");
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == 2048 && f.Filled(buffer, 2048, 3), "next title read continues after decoder read-ahead");
    CHECK(f.Read32(gp - Decoder::kReadPositionGp) == 4096, "title count excludes provider-only read-ahead");
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == 0 && f.Read32(gp - Decoder::kReadPositionGp) == 4096,
          "provider exhaustion is zero without inventing decoder EOF");
    f.Write32(gp - Decoder::kReadPositionGp, 6144);
    const unsigned before = reads;
    f.Args(0xffffffff, 0x4000);
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == -1 && reads == before, "title file EOF is checked before translating the destination");
    CHECK(f.destroyed.empty(), "refill EOF never tears down pictures or PCM");
    f.decoder->SeekStream(f.context);
    f.Args(0xffffffff, 2048);
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == -1 && reads == before && f.invalid_accesses == 0, "invalid destination cannot enter provider");
    f.Args(0, 2047);
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == 0 && reads == before, "sub-sector capacity requests no bytes");
    f.Args(buffer, 2048);
    read_error = -1;
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == -1 && f.Read32(gp - Decoder::kReadPositionGp) == 0, "terminal file error preserves refill count");
    read_error = 2;
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == -1 && f.Read32(gp - Decoder::kReadPositionGp) == 0, "provider over-report is rejected");
    seek_result = 0;
    f.Write32(gp - Decoder::kReadPositionGp, 2048);
    f.decoder->SeekStream(f.context);
    CHECK(f.Result() == 0 && f.Read32(gp - Decoder::kReadPositionGp) == 0, "failed seek still clears title count per NTSC");
    read_error = 0;
    f.decoder->Close(f.context);
    const unsigned closed_reads = reads;
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == -1 && reads == closed_reads, "closed playback cannot refill stale file data");
    f.Write32(f.kDecoder + 0x60, 0xdeadbeef);
    f.decoder->ReadStream(f.context);
    CHECK(f.Result() == f.kOriginalResult && f.originals.back().function == Decoder::kReadStream,
          "foreign callbacks retain their existing service");
    f.installed.at(Decoder::kSeekStream)(f.context, Decoder::kSeekStream + 20);
    CHECK(f.originals.back().entry == Decoder::kSeekStream + 20, "restored interior continuation retains its body");
    CHECK(f.Count("stream_bytes") == 4096 && f.Count("stream_errors") == 4,
          "stream census records bytes and terminal service errors");
    CHECK(f.Count("stream_original_calls") == 2 && f.originals.size() == 2,
          "stream counter counts actual fallback and continuation body invocations");
}

void TestStreamInstallGuards() {
    TestMovieDecoder f;
    Decoder::StreamServices services{[](uint32_t) { return 1; }, [](uint8_t*, uint32_t) { return 0; }};
    CHECK(f.decoder->InstallStream(f.Capture(), f.Installer(), {}) == 0, "stream provider is required");
    const auto capture = f.Capture();
    CHECK(f.decoder->InstallStream([&](uint32_t address) {
        return address == Decoder::kReadStream ? Decoder::Body{} : capture(address);
    }, f.Installer(), services) == 0, "both original callbacks must exist before installation");
    auto installer = f.Installer();
    CHECK(f.decoder->InstallStream(capture, [&](uint32_t address, Decoder::Body body) {
        if (address == Decoder::kReadStream) return false;
        return installer(address, std::move(body));
    }, services) == 0, "failed second installation rolls back the first");
    f.installed.at(Decoder::kSeekStream)(f.context, Decoder::kSeekStream);
    CHECK(f.Result() == f.kOriginalResult, "rolled-back first callback invokes its original");
    CHECK(f.decoder->InstallStream(capture, installer, services) == 2, "clean retry can install both callbacks");
    f.context.cpuRegs.GPR.r[28].UD[0] = 0;
    f.decoder->SeekStream(f.context);
    CHECK(f.Result() == f.kOriginalResult && f.invalid_accesses == 0, "invalid callback gp falls back without invalid reads");
    auto hooks = f.MakeHooks();
    Decoder picture_only(f, hooks);
    CHECK(picture_only.Install(capture, installer) == 3, "default bindings remain exactly three");
    CHECK(picture_only.InstallStream(capture, installer, services) == 0, "default bindings cannot activate stream lifecycle");
}

void TestInputServices() {
    Rig r;
    constexpr uint32_t gp = 0x320000, buffer = 0x300000, capacity = 0x4000;
    r.Create();
    r.context.cpuRegs.GPR.r[28].UD[0] = gp;
    r.Write32(Rig::kDecoder + 0x144, 1);
    r.Write32(Rig::kDecoder + 0x60, Decoder::kReadStream);
    r.Write32(Rig::kDecoder + 0x64, Decoder::kSeekStream);
    r.Write32(Rig::kDecoder + 0x48, buffer);
    r.Write32(Rig::kDecoder + 0x4c, 0);
    r.Write32(Rig::kDecoder + 0x13c, 1);
    r.Write32(Rig::kDecoder + 0x140, 0x28);
    r.Write32(gp - Decoder::kFileLocationGp, 123);
    r.Write32(gp - Decoder::kFileSizeGp, 0x100000);
    r.Write32(gp - Decoder::kReadPositionGp, 0);
    unsigned reads = 0, demuxes = 0;
    int32_t delivered = capacity / 2048, used = capacity / 2;
    uint32_t last_data = 0;
    int32_t last_size = 0;
    CHECK(r.decoder->InstallStream(r.Capture(), r.Installer(), {
        [](uint32_t) { return 1; },
        [&](uint8_t* destination, uint32_t count) {
            ++reads;
            CHECK(count == capacity / 2048, "input service reads one source-sized window");
            if (delivered > 0) std::fill_n(destination, delivered * 2048, 0x5a);
            return delivered;
        },
    }) == 2, "input uses the shared synchronous file provider");
    CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(),
        [&](uint32_t decoder, uint32_t data, int32_t size) {
            ++demuxes;
            CHECK(decoder == Rig::kDecoder && r.Read32(decoder + 0x13c) == 0 &&
                  r.Read32(decoder + 0x140) == 0x28,
                  "demux owns the current descriptor without legacy audio backpressure");
            last_data = data;
            last_size = size;
            return used;
        }) == 1, "one input intent boundary installed");
    r.Args(1);
    r.Call(Decoder::kDemultiplex);
    CHECK(r.Result() == used && last_data == buffer && last_size == capacity && reads == 1,
          "initial input supplies the returned file window once");
    CHECK(r.Read32(Rig::kDecoder + 0x4c) == capacity / 2, "partial software consumption retains pending bytes");
    r.Args(0);
    r.Call(Decoder::kDemultiplex);
    CHECK(r.Result() == used && last_data == buffer + capacity / 2 &&
          last_size == capacity / 2 && reads == 1 && r.Read32(Rig::kDecoder + 0x4c) == 0,
          "pending source tail is consumed without reading ahead");

    used = 0;
    r.Args(1);
    r.Call(Decoder::kDemultiplex);
    CHECK(r.Result() == 0 && demuxes == 3 && r.Read32(Rig::kDecoder + 0x4c) == capacity &&
          r.Read32(Rig::kDecoder + 0x15c) == 0,
          "zero consumption keeps input for a later call instead of spinning or ending playback");
    used = capacity;
    r.Call(Decoder::kDemultiplex);
    CHECK(r.Result() == capacity && reads == 2 && r.Count("demux_bytes") == capacity * 2,
          "the same unconsumed window can subsequently complete");

    // OLD BUG-045 and PCM lifetime: source exhaustion releases the initial
    // audio fill, but is independent of accepted pictures, PCM and Close.
    r.pending_playback = true;
    for (int32_t eof : {0, -1}) {
        delivered = eof;
        r.Write32(Rig::kDecoder + 0x4c, 0);
        r.Write32(Rig::kDecoder + 0x15c, 0);
        r.Write32(Decoder::kSoundStarted, 0);
        r.Args(1);
        const unsigned before = reads;
        r.Call(Decoder::kDemultiplex);
        CHECK(r.Result() == 0 && reads == before + 1 && r.Read32(Rig::kDecoder + 0x15c) == 1,
              "synchronous exhausted/error input terminates even the wait=true refill");
        r.Call(Decoder::kFinished);
        CHECK(r.Result() == 1 && r.pending_playback && r.destroyed.empty(),
              "initial audio-fill exits without releasing accepted provider output");
        r.Write32(Decoder::kSoundStarted, 1);
        r.Call(Decoder::kFinished);
        CHECK(r.Result() == 0 && r.pending_playback, "playing pictures still drain after refill EOF");
    }
    CHECK(r.Count("input_original_calls") == 0 && r.Count("stream_original_calls") == 0,
          "supported input never executes either captured original");

    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        r.Write32(Rig::kDecoder + 0x15c, 0);
        r.Write32(Rig::kDecoder + 0x48, buffer);
        r.Write32(Rig::kDecoder + 0x4c, capacity);
        if (invalid == 0) r.Write32(Rig::kDecoder + 0x48, 0);
        if (invalid == 1) r.Write32(Rig::kDecoder + 0x48, Rig::kRamSize - 4);
        if (invalid == 2) r.Write32(Rig::kDecoder + 0x48, 0xfffffff0);
        if (invalid == 3) r.Write32(Rig::kDecoder + 0x4c, capacity + 1);
        used = invalid == 4 ? -1 : capacity + 1;
        r.Call(Decoder::kDemultiplex);
        CHECK(r.Result() == 0 && r.Read32(Rig::kDecoder + 0x15c) == 1 &&
              r.invalid_accesses == 0 && r.pending_playback && r.destroyed.empty(),
              "invalid buffer/pending/consumption terminates safely without discarding accepted output");
    }
    CHECK(r.Count("input_errors") == 6, "each invalid input contract is diagnosed");
    r.Write32(Rig::kDecoder + 0x48, buffer);
    r.Call(Decoder::kClose);
    const auto closed = r.ram;
    const unsigned before_reads = reads, before_demuxes = demuxes;
    r.Call(Decoder::kDemultiplex);
    CHECK(r.Result() == 0 && reads == before_reads && demuxes == before_demuxes && r.ram == closed,
          "closed playback cannot read or demux stale file data");
    for (unsigned foreign = 0; foreign < 3; ++foreign) {
        r.Write32(Rig::kDecoder + 0x144, foreign == 0 ? 0 : 1);
        r.Write32(Rig::kDecoder + 0x60, foreign == 1 ? 0x1234 : Decoder::kReadStream);
        r.Write32(Rig::kDecoder + 0x14c, foreign == 2 ? 2 : 1);
        r.Args(1);
        const auto before = r.ram;
        r.Call(Decoder::kDemultiplex);
        CHECK(r.Result() == Rig::kOriginalResult && r.originals.back().arguments[0] == 1 &&
              r.ram == before, "foreign container, callback or audio keeps the original ABI");
    }
    r.installed.at(Decoder::kDemultiplex)(r.context, Decoder::kDemultiplex + 0x18);
    CHECK(r.originals.back().entry == Decoder::kDemultiplex + 0x18 &&
          r.Count("input_original_calls") == 4 && r.originals.size() == 4,
          "input census counts actual fallback and continuation execution, not just a selection decision");
}

void TestInputInstallGuards() {
    Rig r;
    const Decoder::DemuxService demux = [](uint32_t, uint32_t, int32_t size) { return size; };
    CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(), demux) == 0,
          "input selection requires the shared-cursor stream contract");
    CHECK(r.decoder->InstallStream(r.Capture(), r.Installer(),
          {[](uint32_t) { return 1; }, [](uint8_t*, uint32_t) { return 0; }}) == 2,
          "install-guard fixture selects stream services");
    CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(), {}) == 0,
          "input selection requires a software demux service");
    CHECK(r.decoder->InstallInput([](uint32_t) { return Decoder::Body{}; }, r.Installer(), demux) == 0,
          "input selection requires an original body for unsupported callers");
    CHECK(r.decoder->InstallInput(r.Capture(), [](uint32_t, Decoder::Body) { return false; }, demux) == 0,
          "failed input installation publishes no selected state");
    CHECK(r.installed.count(Decoder::kDemultiplex) == 0, "failed attempts leave the original binding intact");
    CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(), demux) == 1,
          "a failed installation allows a clean retry");
    CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(), demux) == 0,
          "duplicate input selection cannot recursively capture itself");
}

void TestInputStartSelection() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Rig r;
        constexpr uint32_t gp = 0x320000;
        CHECK(r.decoder->InstallStream(r.Capture(), r.Installer(),
            {[](uint32_t) { return 1; }, [](uint8_t*, uint32_t size) {
                return static_cast<int32_t>(size);
            }}) == 2, "Start fixture owns file callbacks");
        unsigned demuxes = 0;
        CHECK(r.decoder->InstallInput(r.Capture(), r.Installer(),
            [&](uint32_t decoder, uint32_t, int32_t size) {
                ++demuxes;
                // Synthetic MPEG2 sequence header: the service, not guest
                // callback registration, publishes the 32x48 output contract.
                const std::array<uint8_t, 7> sequence{0, 0, 1, 0xb3, 2, 0, 48};
                std::copy(sequence.begin(), sequence.end(), r.Translate(r.Read32(decoder + 0x100), 7));
                return size;
            }) == 1, "Start fixture owns software input");
        const uint32_t audio = mode == 2 ? 2 : 1;
        r.Arena();
        r.StartArgs(true, 0x100, 1, audio);
        r.context.cpuRegs.GPR.r[28].UD[0] = gp;
        r.Write32(gp - Decoder::kFileLocationGp, 123);
        r.Write32(gp - Decoder::kFileSizeGp, 0x100000);
        r.Write32(gp - Decoder::kReadPositionGp, 0);
        if (mode != 1) {
            r.context.cpuRegs.GPR.r[10].UD[0] = Decoder::kSeekStream;
            r.context.cpuRegs.GPR.r[11].UD[0] = Decoder::kReadStream;
        }
        r.Call(Decoder::kStart);
        const bool selected = mode == 0 || mode == 3;
        CHECK(r.Result() == 1 && r.create_calls == 2, "selected and retained Start paths keep the two software creations");
        CHECK(demuxes == (selected ? 1u : 0u) &&
              r.Count("registrations_retired") == (selected ? 2u : 0u),
              "registration is skipped only when this Start owns the matching input");
        CHECK(std::count(r.events.begin(), r.events.end(), 'A') == (selected ? 0 : 2) &&
              std::count(r.events.begin(), r.events.end(), 'D') == (selected ? 0 : 1),
              "foreign callbacks or audio still receive the retained registration and demux services");
        CHECK(r.originals.empty(), "supported Start selection does not invoke captured fallbacks");
        if (mode == 3) {
            // Restart arguments do not reconfigure descriptor formats. A
            // previously configured foreign audio path must retain demux even
            // if a restart call supplies the usual PCM argument.
            r.Write32(Rig::kDecoder + 0x14c, 2);
            r.Write32(r.Read32(Rig::kDecoder + 0x40) + 0x64, 1);
            r.StartArgs(false);
            r.context.cpuRegs.GPR.r[28].UD[0] = gp;
            r.Call(Decoder::kStart);
            CHECK(r.Result() == 1 && demuxes == 1 && std::count(r.events.begin(), r.events.end(), 'D') == 1,
                  "restart chooses the configured descriptor's input contract, not uncommitted arguments");
        }
    }
}

void TestAudioServices() {
    Rig r;
    CHECK(r.decoder->InstallAudio(r.Capture(), r.Installer()) == 2, "audio opt-in installs two service boundaries");
    CHECK(r.installed.size() == 9, "audio services do not replace the seven lifecycle bindings");
    r.Create();
    r.Write32(Decoder::kSoundMode, 1);
    r.Write32(Rig::kDecoder + 0x108, 0x19000);
    r.Write32(Rig::kDecoder + 0x10c, 0x400);
    r.Write32(Rig::kDecoder + 0x110, 0);
    for (uint32_t started : {0u, 1u}) {
        r.Write32(Decoder::kSoundStarted, started);
        r.Args(Decoder::kPlayerFeedSound, 0);
        const auto before = r.ram;
        r.Call(Decoder::kFeedAudio);
        CHECK(r.Result() == 0 && r.ram == before, "empty legacy PCM ring needs no second feed or ring cursor write");
    }
    CHECK(r.Count("audio_feeds") == 2 && r.originals.empty() && r.services.empty(),
          "software PCM path retires IOP status/copy calls in both phases");
    for (float volume : {0.0f, 0.1f, 0.5f, 1.0f}) {
        r.context.fpuRegs.fpr[12].f = volume;
        r.Call(Decoder::kSetVolume);
    }
    CHECK((r.volumes == std::vector<uint16_t>{0, 3276, 16383, 32767}),
          "Audio receives the exact single-precision/truncate source gains");
    CHECK(r.Count("volume_changes") == 4 && r.originals.empty() && r.services.empty(),
          "volume intent reaches Audio without libsdr/SIF or gameplay calls");

    r.Write32(Decoder::kSoundMode, 0);
    r.Write32(Rig::kDecoder + 0x14c, 0);
    r.Args(0x999, 0x123);
    r.Call(Decoder::kFeedAudio);
    r.Call(Decoder::kSetVolume);
    CHECK(r.Count("audio_feeds") == 2 && r.volumes.size() == 4 && r.originals.empty(),
          "no-audio source arms ignore consumers and volume");
    CHECK(r.Count("audio_original_calls") == 0, "selected audio services count no captured-body invocations");
}

void TestAudioFallbackAndInstallGuards() {
    Rig r;
    auto hooks = r.MakeHooks();
    hooks.volume = {};
    Decoder no_volume(r, hooks);
    CHECK(no_volume.Install(r.Capture(), r.Installer(), true) == 7, "volume service stays optional for lifecycle");
    CHECK(no_volume.InstallAudio(r.Capture(), r.Installer()) == 0, "audio opt-in requires the Audio gain adapter");
    Decoder base(r, r.MakeHooks());
    CHECK(base.Install(r.Capture(), r.Installer()) == 3, "audio test baseline has only public picture bindings");
    CHECK(base.InstallAudio(r.Capture(), r.Installer()) == 0, "audio retirement requires lifecycle ownership first");
    CHECK(r.installed.count(Decoder::kFeedAudio) == 0 && r.installed.count(Decoder::kSetVolume) == 0,
          "failed audio installation publishes no partial entries");

    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        Rig bad;
        CHECK(bad.decoder->InstallAudio(bad.Capture(), bad.Installer()) == 2, "fallback fixture audio installed");
        bad.Create();
        bad.Write32(Decoder::kSoundMode, 1);
        bad.Write32(Rig::kDecoder + 0x108, 0x19000);
        bad.Write32(Rig::kDecoder + 0x10c, 0);
        bad.Write32(Rig::kDecoder + 0x110, 0);
        bad.Args(Decoder::kPlayerFeedSound, 0);
        if (invalid == 0) bad.Write32(Rig::kDecoder + 0x148, 0);
        if (invalid == 1) bad.Write32(Rig::kDecoder + 0x14c, 2);
        if (invalid == 2) bad.Args(0x9876, 0);
        if (invalid == 3) bad.Args(Decoder::kPlayerFeedSound, 1);
        if (invalid == 4) bad.Write32(Rig::kDecoder + 0x110, 0x400);
        if (invalid == 5) bad.Write32(Rig::kDecoder + 0x108, 0);
        if (invalid == 6) bad.Write32(Rig::kDecoder + 0x10c, 0x19000);
        if (invalid == 7) bad.Write32(Decoder::kSoundMode, 2);
        const auto before = bad.ram;
        bad.Call(Decoder::kFeedAudio);
        CHECK(bad.originals.size() == 1 && bad.originals[0].function == Decoder::kFeedAudio &&
              bad.Result() == Rig::kOriginalResult && bad.ram == before,
              "foreign consumer, unsupported audio or nonempty/invalid legacy ring uses original ABI");
        CHECK(bad.Count("audio_original_calls") == 1, "audio census measures actual fallback execution");
    }
    Rig volume;
    CHECK(volume.decoder->InstallAudio(volume.Capture(), volume.Installer()) == 2, "volume fallback fixture installed");
    volume.Create();
    volume.Write32(Decoder::kSoundMode, 1);
    for (float value : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        volume.context.fpuRegs.fpr[12].f = value;
        const size_t before = volume.originals.size();
        volume.Call(Decoder::kSetVolume);
        CHECK(volume.originals.size() == before + 1 && volume.volumes.empty(),
              "out-of-contract gain uses original handler without float-to-integer UB");
    }
    for (uint32_t entry : {Decoder::kFeedAudio, Decoder::kSetVolume}) {
        const size_t before = volume.originals.size();
        volume.installed.at(entry)(volume.context, entry + 4);
        CHECK(volume.originals.size() == before + 1 && volume.originals.back().entry == entry + 4,
              "audio interior continuation finishes through its original body");
    }
    CHECK(volume.Count("audio_original_calls") == volume.originals.size() && volume.originals.size() == 7,
          "audio census includes captured bodies reached by continuations");
}

void TestPictureOutputValidation() {
    for (uint32_t format : {0x100u, 0x102u}) {
        for (unsigned invalid = 0; invalid < 7; ++invalid) {
            Rig r;
            r.Create();
            r.Write32(Rig::kDecoder + 0x154, format);
            if (invalid == 0) r.Write32(Rig::kDecoder + 0x94, 0);
            if (invalid == 1) r.Write32(Rig::kDecoder + 0x94, Rig::kRamSize - 16);
            if (invalid == 2) r.Write32(Rig::kDecoder + 0x94, 0xfffffff0);
            if (invalid == 3) r.Write32(Rig::kDecoder + 0x58, 0);
            if (invalid == 4) r.Write32(Rig::kDecoder + 0x58, 17);
            if (invalid == 5) r.Write32(Rig::kDecoder + 0x5c, 31);
            if (invalid == 6) r.Write32(Rig::kDecoder + 0x58, 0xfffffff0);
            const auto before = r.ram;
            r.Call(Decoder::kDecodePicture);
            CHECK(r.Result() == 0 && r.pictures.empty() && r.ram == before,
                  "invalid final plane is rejected without consuming provider output");
            CHECK(r.Count("invalid_picture") == 1 && r.invalid_accesses == 0,
                  "bad output cannot wrap memory and has an explicit diagnostic");
        }
    }
    Rig unknown;
    unknown.Create();
    unknown.Write32(Rig::kDecoder + 0x154, 0x200);
    unknown.events.clear();
    CheckFallback(unknown, Decoder::kDecodePicture, 0x77);
}

void TestFailedStartReopen() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        Rig r;
        r.Arena();
        const auto descriptor = r.Bytes(Rig::kDecoder, 0x15c);
        r.StartArgs();
        if (failure == 0) r.Arena(Rig::kArena, Rig::kArena + 0x63000);
        if (failure == 1) r.fail_demux = true;
        if (failure == 2) r.invalid_sequence = true;
        if (failure == 3) r.throw_seek = true;
        if (failure == 4) r.throw_upload = true;
        const auto saved = r.context.cpuRegs.GPR;
        bool caught = false;
        try { r.Call(Decoder::kStart); } catch (int) { caught = true; }
        CHECK(caught == (failure >= 3), "only service exceptions unwind the caller");
        if (!caught) CHECK(r.Result() == 0, "failed Start is reported to the player");
        CheckCaller(r, saved);
        CHECK(r.Read32(Decoder::kArenaNext) == Rig::kArena, "failed Start restores arena ownership");
        CHECK(r.Bytes(Rig::kDecoder, 0x15c) == descriptor, "partial descriptors and temporary stack pointers do not escape failure");
        CHECK(r.Read32(Rig::kDecoder + 0x15c) == 1 && r.destroyed.size() == 1,
              "failed Start is terminal and releases decoder plus PCM ownership");
        r.Call(Decoder::kFinished);
        CHECK(r.Result() == 1, "the player can observe a failed Start as finished");
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Result() == 0 && r.pictures.empty(), "failed Start cannot serve stale buffers");
        r.Arena();
        r.fail_demux = r.invalid_sequence = r.throw_seek = r.throw_upload = false;
        r.StartArgs();
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 1 && r.Count("started") == 1 && r.Count("start_failed") == 1,
              "the same descriptor and arena reopen after every failure");
        r.Call(Decoder::kDecodePicture);
        r.Call(Decoder::kDecodePicture);
        CHECK(r.Read32(Rig::kDecoder + 0x54) == 2 && r.pictures.size() == 2,
              "reopen starts picture ownership at a fresh pair of priming buffers");
        r.Call(Decoder::kClose);
        const auto closed = r.Bytes(Rig::kDecoder, 0x160);
        r.Arena();
        r.StartArgs();
        r.Call(Decoder::kStart);
        CHECK(r.Result() == 1 && r.Read32(Rig::kDecoder + 0x54) == 0 &&
              r.Bytes(Rig::kDecoder, 0x160) != closed, "normal close permits a subsequent fresh movie");
    }
}

}  // namespace

int main() {
    TestUsEntrypoints();
    TestCreateLayoutAndGuards();
    TestInvalidWork();
    TestResetPreservesOwnership();
    TestInvalidReset();
    TestPictureFormatsAndBuffering();
    TestNoPictureAndEof();
    TestOwnershipFallbacks();
    TestContinuationsAndZeroEntry();
    TestInstallRequiresDependencies();
    TestDefaultInstallCompatibility();
    TestInitializeAndClose();
    TestPersistenceLifetime();
    TestStartSoftwareContract();
    TestRestartAndAudioModes();
    TestStartFailuresAndFallback();
    TestFinishedBacklogAndClose();
    TestSkipRequest();
    TestPrimingAudioEof();
    TestFinishedServiceOracle();
    TestOwnedDecodeErrorAtEof();
    TestStreamServices();
    TestStreamInstallGuards();
    TestInputServices();
    TestInputInstallGuards();
    TestInputStartSelection();
    TestAudioServices();
    TestAudioFallbackAndInstallGuards();
    TestPictureOutputValidation();
    TestFailedStartReopen();
    std::printf("MovieDecoder: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
