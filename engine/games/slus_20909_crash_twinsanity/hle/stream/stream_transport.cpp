#include "games/slus_20909_crash_twinsanity/hle/stream/stream_transport.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"

namespace title {
namespace {
// The opcodes (TR multistream.h Opcode; the module's own additions from 0x101).
enum : uint16_t {
    kPlayStream = 0x1, kSetVolume = 0x2, kSetPitch = 0x3, kStopSound = 0x4, kStopStream = 0x5, kInitSpu = 0x7,
    kGetStatus = 0x9, kPause = 0xA, kResume = 0xB, kInitStreamData = 0xC, kAllocateStreamBuffer = 0xD,
    kSetSpuWriteAddress = 0xE, kCloseStreamBuffer = 0xF, kSetMaxStreamLimit = 0x10, kEnableEffect = 0x13,
    kDisableEffect = 0x14, kSetEffectVolume = 0x15, kEffectChannelOn = 0x18, kEffectChannelOff = 0x19,
    kPatchSoundBank = 0x1C, kCreateFileInfo = 0x1F, kFreeFileId = 0x20, kAllocateSoundTable = 0x25,
    kFreeSound = 0x28, kSetFileOffsetAndSize = 0x29, kInitDisc = 0x2A, kSetStreamParent = 0x2B,
    kSetStreamChild = 0x2C, kGetMaxIopMemory = 0x2D, kAllowKeyOn = 0x2E, kDisableKeyOn = 0x2F,
    kSetEeWriteAddress = 0x31, kSetIopWriteAddress = 0x32, kRestartFromDiscError = 0x35, kCheckDiscError = 0x37,
    kPlaySoundLoop = 0x39, kDisableSpuCallback = 0x3B, kEnableSpuCallback = 0x3C, kInitWait = 0x49,
    kFastLoad = 0x51, kResizeSpuBuffer = 0x52, kSetParameter = 0x5C, kSetVolumeSmooth = 0x60, kSetMibEnd = 0x63,
    kSoundBankLoaded = 0x103, kCompactSoundMemory = 0x104,
};
constexpr uint16_t kFastLoadBindRpc = 0, kFastLoadOnOff = 1;
constexpr size_t kBatchHeaderWords = 2, kCommandHeaderWords = 2;

bool g_installed = false;
StreamTransport::Owners g_owners;
StreamTransport::WriteWord g_write_word;
StreamTransport::Registry g_registry;
StreamTransport::Counters g_count;
std::map<uint16_t, uint64_t> g_unowned;

uint16_t Word(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Word32(const uint16_t* w) { return (static_cast<uint32_t>(w[0]) << 16) | w[1]; }   // Push32: high half first
void Put32(uint8_t* reply, size_t word, uint32_t value) {
    uint8_t* p = reply + word * 4;
    p[0] = static_cast<uint8_t>(value); p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16); p[3] = static_cast<uint8_t>(value >> 24);
}
void Unowned(uint16_t op) { ++g_count.unowned; ++g_unowned[op]; }

// One command of the batch, `n` words at `w` (the title's Push order).
void Answer(uint16_t op, const uint16_t* w, size_t n) {
    ++g_count.commands;
    switch (op) {
    // --- the transport's own registry (TR stream.cpp Initialise, transport.cpp MsSend) ---
    case kGetStatus:
        g_registry.last_status_stream = n >= 1 ? w[0] : 0xFFFF;
        ++g_count.status;
        return;
    case kInitDisc:
        if (n >= 1 && g_registry.disc_type_count < g_registry.disc_types.size())
            g_registry.disc_types[g_registry.disc_type_count++] = w[0];
        ++g_count.registry;
        return;
    case kInitStreamData:
        if (n >= 3) { g_registry.load_type = w[0]; g_registry.max_files = w[1]; g_registry.max_sounds = w[2]; }
        g_registry.stream_data_initialised = true;
        ++g_count.registry;
        return;
    case kSetMaxStreamLimit:
        if (n >= 1) g_registry.stream_count = w[0];
        ++g_count.registry;
        return;
    case kFastLoad:
        if (n >= 2 && w[0] == kFastLoadBindRpc) g_registry.fast_load_bound = true;
        else if (n >= 2 && w[0] == kFastLoadOnOff) g_registry.fast_load_mode = w[1];
        ++g_count.registry;
        return;
    case kInitWait:   // the address the service counts its runs at (0 = closed)
        g_registry.wait_update_word = n >= 2 ? Word32(w) : 0;
        ++g_count.registry;
        return;
    case kCheckDiscError:
    case kRestartFromDiscError:   // the owned media is the mounted image: no error to recover from
        ++g_count.recovery;
        return;
    // --- global sound controls: applied natively by SoundControls (Pause/Resume/Stop staged in the same send) ---
    case kInitSpu:
        SoundEffects::Reset();
        ++g_count.global;
        return;
    case kPause:
    case kResume:
        ++g_count.global;
        return;
    // --- effects (SoundEffects) ---
    case kEnableEffect:
        if (n >= 6) SoundEffects::SetEffect(w[0], static_cast<int16_t>(w[1]), w[2], w[3], w[4], w[5]);
        ++g_count.effects;
        return;
    case kDisableEffect:
        if (n >= 1) SoundEffects::ClearEffect(w[0]);
        ++g_count.effects;
        return;
    case kSetEffectVolume:
        if (n >= 3) SoundEffects::SetVolume(w[0], w[1], w[2]);
        ++g_count.effects;
        return;
    case kEffectChannelOn:
    case kEffectChannelOff:
        if (n >= 1) SoundEffects::SetSend(w[0], op == kEffectChannelOn);
        ++g_count.effects;
        return;
    // --- the owned bank registry keeps its assets as host leases: nothing to move ---
    case kCompactSoundMemory:
        if (!g_owners.banks) { Unowned(op); return; }
        ++g_count.compactions;
        return;
    // --- the Movie PCM sink is owned: the SPU DMA handler hand-over has no owned effect ---
    case kDisableSpuCallback:
    case kEnableSpuCallback:
        if (!g_owners.movie_pcm) { Unowned(op); return; }
        ++g_count.movie;
        return;
    // --- families whose producers bypass the batch when owned; here they are unowned ---
    case kPlayStream: case kCreateFileInfo: case kFreeFileId: case kSetFileOffsetAndSize:
    case kSetEeWriteAddress: case kSetIopWriteAddress:
    case kPatchSoundBank: case kAllocateSoundTable: case kFreeSound: case kSetSpuWriteAddress: case kSoundBankLoaded:
    case kSetVolume: case kSetPitch: case kStopSound: case kPlaySoundLoop: case kSetParameter: case kSetVolumeSmooth:
    case kStopStream: case kSetStreamParent: case kSetStreamChild: case kAllowKeyOn: case kDisableKeyOn: case kSetMibEnd:
    case kAllocateStreamBuffer: case kCloseStreamBuffer: case kGetMaxIopMemory: case kResizeSpuBuffer:
    default:
        Unowned(op);
        return;
    }
}
} // namespace

bool StreamTransport::Install(bool selected, Owners owners, WriteWord write_word) {
    g_installed = selected;
    g_owners = owners;
    g_write_word = std::move(write_word);
    g_registry = Registry{};
    g_count = Counters{};
    g_unowned.clear();
    return true;
}
bool StreamTransport::Enabled() { return g_installed; }
void StreamTransport::Reset() {
    g_registry = Registry{};
    g_count = Counters{};
    g_unowned.clear();
}

bool StreamTransport::Serve(const uint8_t* request, size_t request_size, uint8_t* reply, size_t reply_size) {
    if (!request || !reply || request_size < kBatchHeaderWords * 2 || reply_size < kReplyWords * 4) {
        ++g_count.malformed;
        return false;
    }
    const size_t words = request_size / 2;
    std::vector<uint16_t> w(words);
    for (size_t i = 0; i < words; ++i) w[i] = Word(request + i * 2);
    const uint16_t version = w[0], count = w[1];
    if (version != kVersion) { ++g_count.malformed; return false; }
    size_t at = kBatchHeaderWords;
    for (uint16_t c = 0; c < count; ++c) {
        if (at + kCommandHeaderWords > words) { ++g_count.malformed; return false; }
        const uint16_t op = w[at], n = w[at + 1];
        if (at + kCommandHeaderWords + n > words) { ++g_count.malformed; return false; }
        Answer(op, &w[at + kCommandHeaderWords], n);
        at += kCommandHeaderWords + n;
    }
    ++g_count.batches;
    // The service's own progress word (a cold wait reads it to know the
    // service ran again): one run per batch served.
    if (g_registry.wait_update_word && g_write_word) g_write_word(g_registry.wait_update_word, ++g_registry.wait_updates);
    // The reply: a new status (the counter differs from the last reply's), no
    // in-service records, no load request (-1), the disc not busy and without
    // error, the owned-library statics republished by their own reply hooks.
    std::memset(reply, 0, reply_size);
    Put32(reply, kWordRecordCount, 0);
    Put32(reply, kWordCounter, ++g_count.reply_counter);
    Put32(reply, kWordLoadRequest, 0xFFFFFFFFu);
    Put32(reply, kWordDiscBusy, 0);
    Put32(reply, kWordStereoNotStarted, 0);
    Put32(reply, kWordDiscErrors, 0);
    Put32(reply, kWordIopBufferCount, 0);
    Put32(reply, kWordDspTransferCount, 0);
    return true;
}

const StreamTransport::Registry& StreamTransport::Inspect() { return g_registry; }
StreamTransport::Counters StreamTransport::Count() { return g_count; }
std::string StreamTransport::Census() {
    std::string s = std::string("installed=") + (g_installed ? "1" : "0");
    s += ",batches=" + std::to_string(g_count.batches) + ",commands=" + std::to_string(g_count.commands);
    s += ",status=" + std::to_string(g_count.status) + ",registry=" + std::to_string(g_count.registry);
    s += ",global=" + std::to_string(g_count.global) + ",effects=" + std::to_string(g_count.effects);
    s += ",compactions=" + std::to_string(g_count.compactions) + ",movie=" + std::to_string(g_count.movie);
    s += ",recovery=" + std::to_string(g_count.recovery) + ",wait_updates=" + std::to_string(g_registry.wait_updates);
    s += ",streams=" + std::to_string(g_registry.stream_count) + ",files=" + std::to_string(g_registry.max_files);
    s += ",unowned=" + std::to_string(g_count.unowned) + ",malformed=" + std::to_string(g_count.malformed);
    return s;
}
std::string StreamTransport::UnownedCensus() {
    if (g_unowned.empty()) return "none";
    std::string s;
    char buf[32];
    for (const auto& [op, n] : g_unowned) {
        std::snprintf(buf, sizeof buf, "0x%x:%llu", op, static_cast<unsigned long long>(n));
        if (!s.empty()) s += ',';
        s += buf;
    }
    return s;
}

} // namespace title
