#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_transport.h"

// The transport owner (service-only startup): the title's STREAM batches are
// answered without a module. Pins: the title's startup batch (TR stream.cpp
// Initialise: InitDisc x2, InitStreamData, SetMaxStreamLimit, FastLoad bind +
// on, the appended GetStatus) is served with every command owned; the reply has
// the title's layout (record count 0, a counter that changes per reply, load
// request -1, no disc error, no IOP buffers, no DSP transfers) and the client
// never sees a stale counter; effects commands reach SoundEffects; a producer
// the composition does not own is counted per opcode (never silently served);
// a malformed header serves nothing; the cold-wait progress word advances once
// per batch.
namespace {
using T = title::StreamTransport;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL StreamTransport %s\n", what); }
}
// A batch as the title stages it: {version, count}, then {op, nparams, params...} per command.
using Words = std::vector<uint16_t>;
Words NewBatch() { return Words{T::kVersion, 0}; }
Words& Add(Words& words, uint16_t op, std::vector<uint16_t> params) {
    words.push_back(op);
    words.push_back(static_cast<uint16_t>(params.size()));
    words.insert(words.end(), params.begin(), params.end());
    ++words[1];
    return words;
}
std::vector<uint8_t> Bytes(const Words& words) {
    std::vector<uint8_t> b(T::kTransferBytes, 0);
    for (size_t i = 0; i < words.size(); ++i) { b[i * 2] = words[i] & 0xff; b[i * 2 + 1] = words[i] >> 8; }
    return b;
}
uint32_t Word(const std::vector<uint8_t>& r, size_t w) {
    return r[w * 4] | (r[w * 4 + 1] << 8) | (r[w * 4 + 2] << 16) | (static_cast<uint32_t>(r[w * 4 + 3]) << 24);
}
std::map<uint32_t, uint32_t> g_ram;
}  // namespace

int main() {
    T::Owners owners;
    owners.files = owners.banks = owners.controls = owners.music = owners.buffers = owners.movie_pcm = true;
    check(T::Install(true, owners, [](uint32_t a, uint32_t v) { g_ram[a] = v; }), "install");
    check(T::Enabled(), "enabled after install");
    title::SoundEffects::Install(true);

    // The title's startup batch (stream.cpp Initialise), then the first frame's send.
    Words startup = NewBatch();
    Add(startup, 0x2A, {2}); Add(startup, 0x2A, {5}); Add(startup, 0xC, {0, 36, 0, 40}); Add(startup, 0x10, {9});
    Add(startup, 0x51, {0, 0}); Add(startup, 0x51, {1, 1}); Add(startup, 0x9, {0xFFFF, 0});
    std::vector<uint8_t> reply(T::kTransferBytes, 0xEE);
    const auto bytes = Bytes(startup);
    check(T::Serve(bytes.data(), bytes.size(), reply.data(), reply.size()), "startup batch served");
    auto c = T::Count();
    check(c.batches == 1 && c.commands == 7 && c.registry == 6 && c.status == 1 && c.unowned == 0 && c.malformed == 0,
          "startup commands all owned by the transport registry");
    const auto& reg = T::Inspect();
    check(reg.disc_type_count == 2 && reg.disc_types[0] == 2 && reg.disc_types[1] == 5, "disc types recorded in order");
    check(reg.stream_data_initialised && reg.load_type == 0 && reg.max_files == 36 && reg.max_sounds == 0, "stream data registry");
    check(reg.stream_count == 9 && reg.fast_load_bound && reg.fast_load_mode == 1, "stream count and fast load recorded");
    check(reg.last_status_stream == 0xFFFF, "status request: no stream may load");
    // The reply layout.
    check(Word(reply, T::kWordRecordCount) == 0, "no in-service records");
    check(Word(reply, T::kWordCounter) == 1, "first reply counter 1");
    check(Word(reply, T::kWordLoadRequest) == 0xFFFFFFFFu, "no load request");
    check(Word(reply, T::kWordDiscBusy) == 0 && Word(reply, T::kWordDiscErrors) == 0, "disc idle, no error");
    check(Word(reply, T::kWordIopBufferCount) == 0 && Word(reply, T::kWordDspTransferCount) == 0, "no IOP buffers, no DSP transfers");
    bool zero_tail = true;
    for (size_t w = T::kWordEnd; w < T::kTransferBytes / 4; ++w) zero_tail = zero_tail && Word(reply, w) == 0;
    check(zero_tail, "reply zero-filled past ReplyEnd (the 0xEE fill overwritten)");
    check(T::kWordMaxIopMemory == T::kWordCounter + 49 && T::kWordDiscErrors == T::kWordCounter + 57 &&
          T::kWordIopBufferCount == T::kWordCounter + 66 && T::kWordChannels == T::kWordIopBufferCount + 1 &&
          T::kWordDspTransferCount == T::kWordChannels + 62 && T::kWordEnd == T::kWordDspTransferCount + 1,
          "layout offsets match the title's CHECK_OFFSETs (ReplyStatus +49/+57/+66, ReplyChannels +62)");

    // The per-frame batch: a status, effects, the global pause/resume/reset, compaction, the Movie hand-over.
    Words frame = NewBatch();
    Add(frame, 0x15, {0, 0x3fff, 0x3fff}); Add(frame, 0x13, {1, 3, 0x1000, 0x1000, 10, 20}); Add(frame, 0x18, {7});
    Add(frame, 0x19, {7}); Add(frame, 0xA, {}); Add(frame, 0xB, {}); Add(frame, 0x104, {}); Add(frame, 0x3B, {});
    Add(frame, 0x3C, {}); Add(frame, 0x9, {0xFFFF, 0});
    const auto fb = Bytes(frame);
    check(T::Serve(fb.data(), fb.size(), reply.data(), reply.size()), "frame batch served");
    c = T::Count();
    check(c.batches == 2 && c.effects == 4 && c.global == 2 && c.compactions == 1 && c.movie == 2 && c.status == 2 && c.unowned == 0,
          "frame commands routed to their owners");
    check(Word(reply, T::kWordCounter) == 2, "the counter changes per reply (the client's stale-reply test)");
    const auto& fx = title::SoundEffects::Inspect();
    check(fx.cores[0].volume_left == 0x3fff && fx.cores[1].mode == 3 && fx.cores[1].delay == 10 && fx.cores[1].feedback == 20,
          "effects recorded per core");
    check(!fx.send[7], "send on then off");
    check(title::SoundEffects::Count().sets == 1 && title::SoundEffects::Count().volumes == 1 && title::SoundEffects::Count().sends == 2,
          "effects counters");
    Words reset = NewBatch(); Add(reset, 0x7, {}); Add(reset, 0x9, {0xFFFF, 0});
    const auto rb = Bytes(reset);
    check(T::Serve(rb.data(), rb.size(), reply.data(), reply.size()), "reset batch");
    check(title::SoundEffects::Inspect().cores[1].mode == 0 && title::SoundEffects::Count().resets == 1, "InitSpu resets the effects state");
    check(T::Count().global == 3, "InitSpu counted as a global control");

    // An unowned producer in the batch is counted per opcode, the reply still produced.
    Words unowned = NewBatch(); Add(unowned, 0x1F, {0, 1, 2}); Add(unowned, 0x29, {0, 0, 0, 0, 0, 0});
    Add(unowned, 0x1F, {0, 2, 3}); Add(unowned, 0x9, {0xFFFF, 0});
    const auto ub = Bytes(unowned);
    check(T::Serve(ub.data(), ub.size(), reply.data(), reply.size()), "unowned batch still replied");
    check(T::Count().unowned == 3, "unowned commands counted");
    check(T::UnownedCensus() == "0x1f:2,0x29:1", (std::string("unowned census per opcode: ") + T::UnownedCensus()).c_str());
    check(Word(reply, T::kWordCounter) == 4, "counter still advances");
    // Compaction and the Movie hand-over are unowned without their owners.
    T::Owners partial = owners; partial.banks = false; partial.movie_pcm = false;
    T::Install(true, partial);
    Words nb = NewBatch(); Add(nb, 0x104, {}); Add(nb, 0x3B, {}); Add(nb, 0x9, {0xFFFF, 0});
    const auto nbb = Bytes(nb);
    T::Serve(nbb.data(), nbb.size(), reply.data(), reply.size());
    check(T::Count().unowned == 2 && T::Count().compactions == 0 && T::Count().movie == 0 && T::UnownedCensus() == "0x3b:1,0x104:1",
          "compaction / Movie hand-over unowned without SoundBanks / the Movie PCM sink");

    // The cold wait's progress word: written once per batch while open, closed by 0x49 with address 0.
    T::Install(true, owners, [](uint32_t a, uint32_t v) { g_ram[a] = v; });
    Words wait = NewBatch(); Add(wait, 0x49, {0x0030, 0x9EC0}); Add(wait, 0x9, {0xFFFF, 0});
    const auto wb = Bytes(wait);
    T::Serve(wb.data(), wb.size(), reply.data(), reply.size());
    check(T::Inspect().wait_update_word == 0x00309EC0u && g_ram[0x00309EC0u] == 1, "wait word recorded (Push32 high half first) and advanced");
    Words status = NewBatch(); Add(status, 0x9, {0xFFFF, 0});
    const auto sb = Bytes(status);
    T::Serve(sb.data(), sb.size(), reply.data(), reply.size());
    T::Serve(sb.data(), sb.size(), reply.data(), reply.size());
    check(g_ram[0x00309EC0u] == 3, "one run per batch");
    Words close = NewBatch(); Add(close, 0x49, {0, 0}); Add(close, 0x9, {0xFFFF, 0});
    const auto cb = Bytes(close);
    T::Serve(cb.data(), cb.size(), reply.data(), reply.size());
    T::Serve(sb.data(), sb.size(), reply.data(), reply.size());
    check(T::Inspect().wait_update_word == 0 && g_ram[0x00309EC0u] == 3, "closed wait word no longer written");

    // Malformed: wrong version, truncated command, undersized buffers -- nothing served.
    const uint64_t before = T::Count().batches;
    Words bad = status; bad[0] = 71;
    const auto bb = Bytes(bad);
    check(!T::Serve(bb.data(), bb.size(), reply.data(), reply.size()), "wrong version refused");
    Words trunc = NewBatch(); Add(trunc, 0x13, {1, 2, 3, 4, 5, 6}); trunc[3] = 60;   // claims more words than the batch holds
    std::vector<uint8_t> tb(trunc.size() * 2);
    for (size_t i = 0; i < trunc.size(); ++i) { tb[i * 2] = trunc[i] & 0xff; tb[i * 2 + 1] = trunc[i] >> 8; }
    check(!T::Serve(tb.data(), tb.size(), reply.data(), reply.size()), "truncated command refused");
    check(!T::Serve(sb.data(), sb.size(), reply.data(), 16), "undersized reply refused");
    check(T::Count().batches == before && T::Count().malformed == 3, "malformed counted, no batch served");

    const std::string census = T::Census();
    check(census.find("installed=1,batches=") == 0 && census.find(",unowned=0,malformed=3") != std::string::npos,
          (std::string("census: ") + census).c_str());
    check(title::SoundEffects::Census().find("installed=1,bus=none,") == 0, "effects census names no bus");

    std::printf("StreamTransport %d checks %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
