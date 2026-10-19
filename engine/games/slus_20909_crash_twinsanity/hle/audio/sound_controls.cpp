#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"

#include <array>
#include <stdexcept>
#include <iterator>
#include <utility>
#include <vector>

#include "engine/ee/recomp_abi/host_app/memory.h"

namespace title {
namespace {
constexpr uint32_t kStates = 0x2e79a8, kSounds = 0x2e7680, kGroups = 0x2e7ac8;
constexpr uint32_t kLeft = 0x2e7a08, kRight = 0x2e7a68;
constexpr uint32_t kPlay = 0x1e18e8, kVolume = 0x1e3e80, kPitch = 0x1e6b50,
                   kRelease = 0x1e4038, kKeyOff = 0x1e6d30, kSend = 0x1e2958,
                   kReply = 0x1e1f48, kApplyGroup = 0x1e3688, kGroup = 0x1e7cb8,
                   kChannelGroup = 0x1e7d38, kPause = 0x1e6dd0, kResume = 0x1e6e18,
                   kReset = 0x1e6e60, kStream = 0x1e1500, kChild = 0x1e30d0,
                   kCommit = 0x1e13b0;
constexpr std::array<uint32_t, 16> kEntries{
    kPlay, kVolume, kPitch, kRelease, kKeyOff, kSend, kReply, kApplyGroup,
    kGroup, kChannelGroup, kPause, kResume, kReset, kStream, kChild, kCommit};
using Kind = SoundControls::Kind;
using Command = SoundControls::Command;
Audio* g_audio = nullptr;
SoundControls::RemainingVolume g_remaining;
SoundControls::MusicApply g_music;
SoundControls::BufferApply g_buffers;
std::function<void()> g_finish_music;
std::array<StreamLibrary::Body, kEntries.size()> g_originals;
StreamLibrary::Body g_store_volumes, g_compute_volumes;
std::vector<Command> g_pending;
std::array<bool, Audio::kVoices> g_owned{};
std::array<uint32_t, Audio::kVoices> g_requested{};
uint32_t g_words = 0;
bool g_installed = false, g_sending = false, g_clocked = false;
uint64_t g_tick = 0;
SoundControls::Counters g_count;
std::vector<SoundControls::PlayRecord> g_play_head;

uint32_t Arg(const CpuContext& ctx, unsigned reg) { return ctx.cpuRegs.GPR.r[reg].UL[0]; }
void Result(CpuContext& ctx, int32_t value) { ctx.cpuRegs.GPR.r[2].SD[0] = value; }
uint32_t Gp(const CpuContext& ctx) { return Arg(ctx, 28); }
bool Owned(uint32_t channel) { return channel < Audio::kVoices && g_owned[channel]; }
void PublishStatus() {
    if (!g_audio) return;
    for (uint32_t channel = 0; channel < Audio::kVoices; ++channel)
        if (Owned(channel))
            memory::write<uint8_t>(kStates + channel, g_requested[channel] ? 3 :
                                  g_audio->IsVoiceFree(channel) ? 0 : g_audio->IsMusicVoice(channel) ? 2 : 1);
}
StreamLibrary::Body& Original(uint32_t address) {
    for (size_t i = 0; i < kEntries.size(); ++i)
        if (kEntries[i] == address) return g_originals[i];
    throw std::logic_error("unknown sound service entry");
}
void Apply(Command& command) {
    auto& a = *g_audio;
    switch (command.kind) {
    case Kind::Play: {
        const bool started = a.StartVoice(command.channel, std::move(command.voice));
        if (!started) ++g_count.errors;
        if (command.record && command.record <= g_play_head.size()) {
            auto& record = g_play_head[command.record - 1];
            record.started = started;
            record.boundary = g_tick;
        }
        if (g_requested[command.channel]) --g_requested[command.channel];
        ++g_count.plays;
        break;
    }
    case Kind::Volume:
        if (a.SetVoiceLevels(command.channel, command.raw_left, command.raw_right,
                             command.left, command.right, command.smooth)) ++g_count.errors;
        ++g_count.volume; break;
    case Kind::Pitch: a.SetVoicePitch(command.channel, static_cast<uint16_t>(command.value)); ++g_count.pitch; break;
    case Kind::Release: a.SetVoiceRelease(command.channel, command.value); ++g_count.release; break;
    case Kind::KeyOff: a.KeyOff(command.channel); ++g_count.key_off; break;
    case Kind::Group: a.StoreGroupVolume(command.value, command.left, command.right); break;
    case Kind::ChannelGroup: a.SetVoiceGroup(command.channel, command.value); break;
    case Kind::Pause: a.PauseAll(); break;
    case Kind::Resume: a.ResumeAll(); break;
    case Kind::Stop:
        if (g_music) {
            MusicRequest reset;
            reset.kind = MusicRequest::Kind::Reset;
            g_music(reset);
        }
        a.StopVoices(); break;
    case Kind::HandOff: a.HandOffVoice(command.channel); ++g_count.handoffs; break;
    case Kind::RemainingVolume:
        g_remaining(command.channel, command.left, command.right);
        ++g_count.remaining_volumes; break;
    case Kind::Music:
        g_music(command.music);
        if ((command.music.kind == MusicRequest::Kind::Play || command.music.kind == MusicRequest::Kind::Child) &&
            g_requested[command.music.channel]) --g_requested[command.music.channel];
        break;
    case Kind::Buffer: g_buffers(command.buffer); break;
    }
    ++g_count.applied;
}
void Send(CpuContext& ctx, uint32_t entry) {
    if (g_sending) { ++g_count.busy; Result(ctx, -1); return; }
    // Callback-issued work belongs to the next batch. A failed send restores
    // this finite prefix before anything enqueued by a callback.
    auto batch = std::move(g_pending);
    g_pending.clear();
    const uint32_t words = std::exchange(g_words, 0);
    g_sending = true;
    Original(kSend)(ctx, entry);
    const int32_t sent = ctx.cpuRegs.GPR.r[2].SL[0];
    if (sent < 0) {
        g_pending.insert(g_pending.begin(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        g_words += words;
        ++g_count.busy;
    } else {
        for (auto& command : batch) Apply(command);
        if (g_finish_music) g_finish_music();
        ++g_count.batches;
        PublishStatus();
        Result(ctx, sent + static_cast<int32_t>(words));
    }
    g_sending = false;
}
bool Space(CpuContext& ctx, uint32_t words) {
    const auto retained_words = memory::read<uint32_t>(Gp(ctx) - 0x7a0c);
    if (static_cast<uint64_t>(retained_words) + g_words + words + 2 <= 0x400) return true;
    if (g_sending) { ++g_count.errors; return false; }
    // Same source MsCommit limit, without constructing a sample wire packet.
    auto call = ctx;
    call.cpuRegs.GPR.r[4].UD[0] = 0; // SendWait
    memory::write<uint8_t>(Gp(ctx) - 0x7948, 1); // full batch cannot append status
    Send(call, 0);
    memory::write<uint8_t>(Gp(ctx) - 0x7948, 0);
    return call.cpuRegs.GPR.r[2].SL[0] >= 0;
}
bool Stage(CpuContext& ctx, Command command) {
    if (!Space(ctx, command.words)) {
        memory::write<uint16_t>(Gp(ctx) - 0x7920, 1);
        ++g_count.errors;
        return false;
    }
    g_words += command.words;
    g_pending.push_back(std::move(command));
    ++g_count.staged;
    return true;
}
void Levels(CpuContext& ctx, Command& command, bool store) {
    if (store) {
        auto call = ctx;
        call.cpuRegs.GPR.r[4].UD[0] = command.channel;
        call.cpuRegs.GPR.r[5].SD[0] = static_cast<int16_t>(command.raw_left);
        call.cpuRegs.GPR.r[6].SD[0] = static_cast<int16_t>(command.raw_right);
        g_store_volumes(call, 0); // pure client-side source arithmetic
    } else {
        auto call = ctx;
        call.cpuRegs.GPR.r[4].UD[0] = memory::read<uint32_t>(kGroups + command.channel * 4);
        call.cpuRegs.GPR.r[5].UD[0] = command.channel;
        g_compute_volumes(call, 0);
        command.raw_left = memory::read<uint16_t>(kLeft + command.channel * 2);
        command.raw_right = memory::read<uint16_t>(kRight + command.channel * 2);
    }
    command.left = static_cast<uint16_t>(memory::read<uint32_t>(0x30a554));
    command.right = static_cast<uint16_t>(memory::read<uint32_t>(0x30a558));
}
void Enter(uint32_t address, CpuContext& ctx, uint32_t entry) {
    auto& original = Original(address);
    if (!g_audio || (entry != 0 && entry != address)) {
        ++g_count.fallback;
        original(ctx, entry);
        return;
    }
    const uint32_t a = Arg(ctx, 4), b = Arg(ctx, 5), c = Arg(ctx, 6);
    if (address == kSend) { Send(ctx, entry); return; }
    if (address == kReply) {
        original(ctx, entry);
        PublishStatus();
        ++g_count.replies;
        return;
    }
    if (address == kCommit) {
        if (memory::read<int32_t>(Gp(ctx) - 0x78fc) == -1) { original(ctx, entry); return; }
        const auto length = memory::read<uint32_t>(Gp(ctx) - 0x7900);
        if (!Space(ctx, length + 2)) {
            memory::write<uint16_t>(Gp(ctx) - 0x7920, 1);
            return;
        }
        original(ctx, entry);
        return;
    }
    if (address == kPlay) {
        Command command;
        command.channel = b & 0xffff;
        command.words = 10; // source commit accounting: 8 arguments + header
        const auto result = g_audio->PrepareSound(a, b, static_cast<int16_t>(c),
            static_cast<int16_t>(Arg(ctx, 7)), static_cast<uint16_t>(Arg(ctx, 8)),
            Arg(ctx, 9), Arg(ctx, 10), Arg(ctx, 11), command.voice);
        if (result) { ++g_count.errors; Result(ctx, result); return; }
        if (!Space(ctx, command.words)) { Result(ctx, -1); return; }
        memory::write<uint32_t>(kGroups + command.channel * 4, b & 0xffff0000);
        command.raw_left = static_cast<uint16_t>(c);
        command.raw_right = static_cast<uint16_t>(Arg(ctx, 7));
        Levels(ctx, command, true);
        command.voice.player.SetVolumes(command.left, command.right);
        if (g_play_head.size() < SoundControls::kPlayHead) {
            SoundControls::PlayRecord record;
            record.sound = a; record.channel = command.channel; record.group = b >> 16;
            record.loops = Arg(ctx, 11);
            record.bytes = static_cast<uint32_t>(command.voice.player.Capture().samples->size());
            record.raw_left = command.raw_left; record.raw_right = command.raw_right;
            record.left = command.left; record.right = command.right;
            record.pitch = static_cast<uint16_t>(Arg(ctx, 8));
            record.attack = static_cast<uint8_t>(Arg(ctx, 9)); record.release = static_cast<uint8_t>(Arg(ctx, 10));
            g_play_head.push_back(record);
            command.record = g_play_head.size();
        }
        memory::write<uint8_t>(kStates + command.channel, 3);
        memory::write<uint32_t>(kSounds + command.channel * 4, a);
        g_owned[command.channel] = true;
        ++g_requested[command.channel];
        const bool staged = Stage(ctx, std::move(command));
        Result(ctx, staged ? 0 : -1);
        return;
    }
    if (address == kGroup) {
        const uint32_t group = a >> 16;
        const int32_t error = group == 0 || group > Audio::kGroups ? -1 :
                              b > 0x1000 ? -2 : c > 0x1000 ? -3 : 0;
        if (error) { Result(ctx, error); return; }
        Command command;
        command.kind = Kind::Group; command.value = a;
        command.left = static_cast<uint16_t>(b); command.right = static_cast<uint16_t>(c);
        if (!Stage(ctx, std::move(command))) { Result(ctx, -1); return; }
        // The original setter only stores the client group values and calls
        // ApplyGroup below. No sample or music packet is emitted by it.
        original(ctx, entry);
        return;
    }
    if (address == kApplyGroup) {
        for (uint32_t channel = 0; channel < Audio::kVoices; ++channel) {
            if (memory::read<uint32_t>(kGroups + channel * 4) != a ||
                memory::read<uint8_t>(kStates + channel) == 0) continue;
            Command command;
            command.kind = Owned(channel) ? Kind::Volume : Kind::RemainingVolume;
            command.channel = channel; command.words = 5;
            Levels(ctx, command, false);
            if (!Stage(ctx, std::move(command))) break;
        }
        return; // source void
    }
    if (address == kChannelGroup) {
        original(ctx, entry);
        if (Owned(a)) {
            Command command;
            command.kind = Kind::ChannelGroup; command.channel = a; command.value = b >> 16;
            Stage(ctx, std::move(command));
        }
        return;
    }
    if (address == kPause || address == kResume || address == kReset) {
        original(ctx, entry); // shared music/effect state still needs this arm
        ++g_count.shared;
        Command command;
        command.kind = address == kPause ? Kind::Pause : address == kResume ? Kind::Resume : Kind::Stop;
        Stage(ctx, std::move(command)); // original accounts the words
        if (address == kReset) g_owned.fill(false);
        return;
    }
    if (address == kStream || address == kChild) {
        const uint32_t channel = Arg(ctx, 6) & 0xffff;
        const uint32_t mode = Arg(ctx, 10);
        const int32_t streams = memory::read<int32_t>(Gp(ctx) - 0x794c);
        const bool valid_child = address == kChild && static_cast<int32_t>(a) >= 0 &&
            static_cast<int32_t>(a) < streams && b < 48 && channel < 48;
        original(ctx, entry);
        ++g_count.shared;
        if ((address == kStream && ctx.cpuRegs.GPR.r[2].SL[0] >= 0 && mode <= 1 && channel < 48) || valid_child) {
            Command command;
            command.kind = Kind::HandOff; command.channel = channel;
            Stage(ctx, std::move(command));
            g_owned[channel] = false;
        }
        return;
    }
    // Per-channel commands on remaining music still use the captured service.
    // Invalid channels also use it so its precise public error ABI is retained.
    const uint32_t channel = address == kVolume ? static_cast<uint16_t>(a) : a;
    if (!Owned(channel)) {
        ++g_count.fallback;
        original(ctx, entry);
        return;
    }
    Command command;
    command.channel = channel;
    if (address == kVolume) {
        if ((b | c) & 0x8000) { ++g_count.errors; Result(ctx, -2); return; }
        command.kind = Kind::Volume; command.words = 5; command.smooth = true;
        command.raw_left = static_cast<uint16_t>(b); command.raw_right = static_cast<uint16_t>(c);
        Levels(ctx, command, true);
    } else if (address == kPitch) {
        command.kind = Kind::Pitch; command.words = 4; command.value = static_cast<uint16_t>(b);
    } else if (address == kRelease) {
        if (b >= 32) { Result(ctx, -1); return; }
        command.kind = Kind::Release; command.words = 4; command.value = b;
    } else {
        command.kind = Kind::KeyOff; command.words = 3;
    }
    Result(ctx, Stage(ctx, std::move(command)) ? 0 : -1);
}
} // namespace

bool SoundControls::Install(const Capture& capture, const Installer& install,
                            Audio* selected, RemainingVolume remaining_volume) {
    if (g_sending) return false;
    if (selected && !remaining_volume) return false;
    if (selected != g_audio && g_audio) {
        if (!g_pending.empty() || g_sending) return false;
        for (uint32_t i = 0; i < Audio::kVoices; ++i) if (g_audio->OwnsVoice(i)) return false;
    }
    if (!g_installed) {
        for (size_t i = 0; i < kEntries.size(); ++i) {
            g_originals[i] = capture(kEntries[i]);
            if (!g_originals[i]) return false;
        }
        g_store_volumes = capture(0x1e7dd0);
        g_compute_volumes = capture(0x1e7d50);
        if (!g_store_volumes || !g_compute_volumes) return false;
        for (size_t i = 0; i < kEntries.size(); ++i) {
            const auto address = kEntries[i];
            if (!install(address, [address](CpuContext& ctx, uint32_t entry) { Enter(address, ctx, entry); })) {
                for (size_t j = 0; j < i; ++j) install(kEntries[j], g_originals[j]);
                return false;
            }
        }
        g_installed = true;
    }
    if (selected != g_audio) {
        g_owned.fill(false);
        g_requested.fill(0);
        g_clocked = false;
    }
    g_audio = selected;
    g_remaining = std::move(remaining_volume);
    return true;
}
bool SoundControls::Enabled() { return g_audio != nullptr; }
bool SoundControls::Owns(const Audio& audio) { return g_audio == &audio; }
bool SoundControls::CaptureState(State& state, std::string* error) {
    if (!g_audio || g_sending) {
        if (error) *error = "sound controls are unselected or delivering";
        return false;
    }
    State snapshot{g_pending, g_owned, g_requested, g_words, g_clocked,
                   static_cast<bool>(g_music), static_cast<bool>(g_buffers), g_tick};
    if (!ValidateState(snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool SoundControls::ValidateState(const State& state, std::string* error) {
    if (!g_audio || g_sending || state.music != static_cast<bool>(g_music) ||
        state.buffers != static_cast<bool>(g_buffers)) {
        if (error) *error = "sound control owner/provider mismatch or active delivery";
        return false;
    }
    return ValidateData(state, error);
}
bool SoundControls::RestoreState(const State& state, std::string* error) {
    if (!ValidateState(state, error)) return false;
    auto pending = state.pending; // all allocation before replacing live work
    g_pending = std::move(pending);
    g_owned = state.owned; g_requested = state.requested; g_words = state.words;
    g_clocked = state.clocked; g_tick = state.tick;
    return true;
}
bool SoundControls::Reset() {
    if (g_sending) return false;
    g_pending.clear(); g_words = 0;
    g_owned.fill(false); g_requested.fill(0); g_clocked = false;
    return true;
}
bool SoundControls::CanCheckpoint() {
    // No portable Audio/control composition is installed yet.
    return !Enabled();
}
bool SoundControls::BindMusic(MusicApply apply, std::function<void()> finish_batch) {
    if (g_sending || !g_pending.empty() || static_cast<bool>(apply) != static_cast<bool>(finish_batch)) return false;
    g_music = std::move(apply); g_finish_music = std::move(finish_batch);
    return true;
}
bool SoundControls::StageMusic(CpuContext& ctx, MusicRequest request, uint32_t words) {
    const bool play = request.kind == MusicRequest::Kind::Play || request.kind == MusicRequest::Kind::Child;
    if (!g_audio || !g_music || (play && (request.channel >= Audio::kVoices ||
        request.group > Audio::kGroups || ((request.left | request.right) & 0x8000))) ||
        !Space(ctx, words)) return false;
    Command command;
    command.kind = Kind::Music; command.words = words;
    if (play) {
        memory::write<uint32_t>(kGroups + request.channel * 4, request.group << 16);
        command.channel = request.channel; command.raw_left = request.left; command.raw_right = request.right;
        Levels(ctx, command, true);
        request.raw_left = command.raw_left; request.raw_right = command.raw_right;
        request.left = command.left; request.right = command.right;
        memory::write<uint8_t>(kStates + request.channel, 3);
        g_owned[request.channel] = true;
        ++g_requested[request.channel];
    }
    command.music = request;
    return Stage(ctx, std::move(command));
}
bool SoundControls::BindBuffers(BufferApply apply) {
    if (g_sending || !g_pending.empty()) return false;
    g_buffers = std::move(apply);
    return true;
}
bool SoundControls::StageBuffer(CpuContext& ctx, BufferRequest request, uint32_t words) {
    if (!g_audio || !g_buffers) return false;
    Command command;
    command.kind = Kind::Buffer; command.words = words; command.buffer = request;
    return Stage(ctx, std::move(command));
}
bool SoundControls::Flush(CpuContext& ctx) {
    if (!g_audio || g_sending) return false;
    auto call = ctx;
    call.cpuRegs.GPR.r[4].UD[0] = 0;
    Send(call, 0);
    return call.cpuRegs.GPR.r[2].SL[0] >= 0 && g_pending.empty();
}
bool SoundControls::AdvanceBoundary(uint64_t tick) {
    if (!g_audio) return true;
    if (g_clocked && tick < g_tick) return false;
    if (!g_clocked || tick != g_tick) {
        g_tick = tick; g_clocked = true;
        g_audio->AdvanceControls();
        ++g_count.boundaries;
    }
    return true;
}
SoundControls::Counters SoundControls::Count() { return g_count; }
std::vector<SoundControls::PlayRecord> SoundControls::PlayHead() { return g_play_head; }
} // namespace title
