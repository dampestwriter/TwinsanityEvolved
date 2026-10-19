#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"

#include <array>

#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"

namespace title {
namespace {
Audio* g_audio = nullptr;
bool g_installed = false;
SoundBanks::Counters g_count;

void Result(CpuContext& ctx, int32_t value) { ctx.cpuRegs.GPR.r[2].SD[0] = value; }
bool Empty(const Audio& audio) {
    Audio::State state;
    if (!audio.Capture(state) || !state.samples.empty()) return false;
    for (uint32_t i = 0; i < Audio::kVoices; ++i) if (audio.OwnsVoice(i)) return false;
    return true;
}
}

bool SoundBanks::Install(const Capture& capture, const Installer& install, Audio* selected) {
    if (selected && !StreamLibrary::Enabled()) return false;
    if (selected != g_audio && g_audio && (!Empty(*g_audio) || StreamLibrary::HasSoundReads())) return false;
    if (g_installed && selected == g_audio) return true;
    if (!g_installed) {
        // NTSC bodies checked against PAL platform/ps2/stream.cpp and
        // multistream/commands.cpp; full entries only, never continuations.
        constexpr std::array<uint32_t, 5> addresses{0x2ada68, 0x1e7540, 0x1e75d8, 0x1e8278, 0x1e2958};
        std::array<StreamLibrary::Body, addresses.size()> originals;
        for (size_t i = 0; i < addresses.size(); ++i) {
            originals[i] = capture(addresses[i]);
            if (!originals[i]) return false;
        }
        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto address = addresses[i];
            const auto original = originals[i];
            if (!install(address, [address, original, i](CpuContext& ctx, uint32_t entry) {
                if (!g_audio || !StreamLibrary::Enabled() || (entry != 0 && entry != address)) {
                    ++g_count.fallbacks;
                    original(ctx, entry);
                    return;
                }
                if (i == 4) {
                    auto* schedule = StreamLibrary::Scheduling();
                    const auto through = schedule->LastOrder();
                    const auto accepted_ms = schedule->ServiceInstantMs();
                    // Remaining music/control traffic still owns this send.
                    // Accept only the bank prefix present before it ran.
                    original(ctx, entry);
                    if (ctx.cpuRegs.GPR.r[2].SL[0] >= 0 &&
                        schedule == StreamLibrary::Scheduling()) schedule->AcceptSounds(through, accepted_ms);
                    return;
                }
                const auto argument = ctx.cpuRegs.GPR.r[4].UL[0];
                if (i == 1) {
                    g_audio->ReserveSounds(static_cast<uint16_t>(argument));
                    ++g_count.reserved;
                    return; // source signature void
                }
                if (i == 2) {
                    g_audio->ReleaseSound(argument); // free of an absent label is idempotent
                    ++g_count.freed;
                    return;
                }
                if (i == 3) {
                    if (g_audio->SoundBankLoaded(static_cast<uint16_t>(argument))) ++g_count.acknowledged;
                    else { ++g_count.errors; Result(ctx, -1); }
                    return;
                }
                const auto ticket = g_audio->BeginSoundLoad(ctx.cpuRegs.GPR.r[5].UL[0]);
                if (!ticket) { ++g_count.errors; Result(ctx, -2); return; }
                if (!LoaderFileIo::StartSoundRead(ctx, ticket.generation, ticket.sequence)) {
                    g_audio->CancelSoundLoad(ticket);
                    ++g_count.errors;
                    Result(ctx, -1);
                    return;
                }
                ++g_count.issued;
            })) {
                for (size_t j = 0; j < i; ++j) install(addresses[j], originals[j]);
                return false;
            }
        }
        g_installed = true;
    }
    if (!StreamLibrary::BindSounds(selected ? Deliver : FileSchedule::Apply{},
                                   selected ? Cancel : FileSchedule::Apply{})) return false;
    g_audio = selected;
    return true;
}
void SoundBanks::Deliver(const FileSchedule::Payload& payload) {
    const Audio::LoadTicket ticket{payload.sound, payload.generation, payload.sequence};
    if (g_audio && g_audio->CompleteSoundLoad(ticket, payload.bytes)) ++g_count.delivered;
    else ++g_count.stale;
}
void SoundBanks::Cancel(const FileSchedule::Payload& payload) {
    const Audio::LoadTicket ticket{payload.sound, payload.generation, payload.sequence};
    if (g_audio && g_audio->CancelSoundLoad(ticket)) ++g_count.cancelled;
}
bool SoundBanks::Enabled() { return g_audio != nullptr; }
bool SoundBanks::Owns(const Audio& audio) { return g_audio == &audio; }
void SoundBanks::Reset() { if (g_audio) g_audio->Reset(); }
SoundBanks::Counters SoundBanks::Count() { return g_count; }

} // namespace title
