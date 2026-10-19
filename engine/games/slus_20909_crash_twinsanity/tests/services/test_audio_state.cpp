#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio_state.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)

std::vector<uint8_t> Sample(uint8_t seed) {
    std::vector<uint8_t> bytes(16 * 17);
    for (size_t block = 0; block < 17; ++block) {
        bytes[block * 16] = static_cast<uint8_t>((block % 5) * 16 + 2);
        for (size_t i = 2; i < 16; ++i) bytes[block * 16 + i] = static_cast<uint8_t>(seed + i * 21 + block);
    }
    bytes[1] = 4; bytes[16 * 16 + 1] = 3;
    return bytes;
}
}
int main() {
    title::Audio audio;
    audio.ReserveSounds(8);
    CHECK(audio.StoreSound(0x8000, Sample(11)) && audio.SoundBankLoaded(0x8000));
    CHECK(audio.PlaySound(0x8000, 0x10002, 8000, 0x7fff - 14000, 0x1743, 3, 17, 0) == 0);
    CHECK(audio.PlaySound(0x8000, 3, 12000, 9000, 0x877, 0, 8, 0) == 0);
    for (int i = 0; i < 9; ++i) audio.MixTick();
    CHECK(audio.SetVoiceVolume(2, 0x7fff - 6000, 7000) == 0);
    audio.AdvanceControls();
    audio.MixTick();
    // OLD SOUND_P5: audible resume includes a voice's old bank lease, even
    // when the registry has already replaced that label's storage.
    CHECK(audio.ReleaseSound(0x8000));
    CHECK(audio.StoreSound(0x8000, Sample(99)) && audio.SoundBankLoaded(0x8000));
    const auto old_ticket = audio.BeginSoundLoad(9);
    CHECK(old_ticket);
    const auto snapshot = audio.CaptureLoads();
    CHECK(title::Audio::Validate(snapshot));
    title::Audio::State incomplete;
    CHECK(!audio.Capture(incomplete));
    std::vector<uint8_t> bytes;
    CHECK(title::AudioState::Encode(snapshot, bytes));
    title::Audio::Snapshot decoded;
    CHECK(title::AudioState::Decode(bytes, decoded));
    CHECK(decoded.playback.voices[2].player.Capture().samples ==
          decoded.playback.voices[3].player.Capture().samples);
    CHECK(decoded.playback.voices[2].player.Capture().samples != decoded.playback.samples.at(0x8000).bytes);
    std::vector<uint8_t> roundtrip;
    CHECK(title::AudioState::Encode(decoded, roundtrip) && roundtrip == bytes);

    title::Audio resumed;
    uint64_t epoch = 0;
    CHECK(resumed.RestoreLoads(decoded, epoch) && epoch > old_ticket.generation);
    CHECK(!resumed.CompleteSoundLoad(old_ticket, Sample(14)));
    CHECK(!resumed.CancelSoundLoad(old_ticket));
    const title::Audio::LoadTicket new_ticket{9, epoch, old_ticket.sequence};
    CHECK(resumed.CompleteSoundLoad(new_ticket, Sample(14)) && resumed.SoundBankLoaded(new_ticket));
    bool audible = false;
    uint64_t frames = 0;
    for (int i = 0; i < 120; ++i) {
        if (i == 23) { audio.PauseAll(); resumed.PauseAll(); }
        if (i == 31) { audio.ResumeAll(); resumed.ResumeAll(); }
        if (i == 47) { audio.KeyOff(3); resumed.KeyOff(3); }
        audio.AdvanceControls(); resumed.AdvanceControls();
        const auto expected = audio.MixTick();
        const auto restored = resumed.MixTick();
        CHECK(expected.size() == restored.size() &&
              std::memcmp(expected.data(), restored.data(), expected.size() * sizeof(float)) == 0);
        audible |= std::any_of(expected.begin(), expected.end(), [](float f) { return f != 0; });
        frames += title::Audio::kFramesPerTick;
    }
    CHECK(audible && frames == 96000);
    const auto previous_epoch = epoch;
    CHECK(resumed.RestoreLoads(decoded, epoch) && epoch > previous_epoch);
    CHECK(!resumed.CancelSoundLoad(new_ticket) && !resumed.CompleteSoundLoad(new_ticket, Sample(14)));

    // Every truncation and the strict tail fail without replacing the output.
    title::Audio::Snapshot sentinel;
    sentinel.playback.ticks = 0xabcdef;
    for (size_t size = 0; size < bytes.size(); ++size) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + size);
        CHECK(!title::AudioState::Decode(truncated, sentinel) && sentinel.playback.ticks == 0xabcdef);
    }
    auto corrupt = bytes;
    corrupt.push_back(0);
    CHECK(!title::AudioState::Decode(corrupt, sentinel));
    corrupt = bytes; corrupt[0] = 99;
    CHECK(!title::AudioState::Decode(corrupt, sentinel));
    auto invalid = snapshot;
    invalid.pending_loads.emplace(0x8000, 1);
    CHECK(!title::Audio::Validate(invalid));
    invalid = snapshot; invalid.load_generation = UINT64_MAX;
    CHECK(!audio.CanRestoreLoads(invalid));
    invalid = snapshot;
    auto player = invalid.playback.voices[2].player.Capture();
    player.envelope = std::numeric_limits<float>::quiet_NaN();
    CHECK(!invalid.playback.voices[2].player.Restore(player));
    invalid.playback.group_left[1] = 0x1001;
    std::vector<uint8_t> unchanged{1, 2, 3};
    CHECK(!title::AudioState::Encode(invalid, unchanged) && unchanged == std::vector<uint8_t>({1, 2, 3}));
    std::printf("AudioState: %d checks, %llu resumed stereo frames, %d failures\n",
                checks, static_cast<unsigned long long>(frames), failures);
    return failures ? 1 : 0;
}
