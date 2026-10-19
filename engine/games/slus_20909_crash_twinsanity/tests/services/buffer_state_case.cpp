#include "games/slus_20909_crash_twinsanity/tests/services/buffer_state_case.h"

#include <algorithm>
#include <cstdio>

#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/buffer_state.h"

int RunBufferStateCase(title::StreamBuffers::Loan& sound, title::StreamBuffers::Loan& disc) {
    int failures = 0, checks = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL buffer state %s\n", name); }
    };
    using title::MusicBuffers;
    using title::BufferState;
    auto sound_view = MusicBuffers::Resolve(sound), disc_view = MusicBuffers::Resolve(disc);
    check(sound_view && disc_view, "production providers resolve both loans");
    if (!sound_view || !disc_view) return failures;
    std::fill_n(sound_view.data, sound_view.bytes, uint8_t{0xb9});
    std::fill_n(disc_view.data, disc_view.bytes, uint8_t{0xd7});
    const auto before = MusicBuffers::Count();
    MusicBuffers::State saved;
    check(MusicBuffers::CaptureState(saved), "capture does not flush a control batch");
    std::vector<uint8_t> bytes, roundtrip;
    check(BufferState::Encode(saved, bytes), "encode complete storage and metadata");
    MusicBuffers::State decoded;
    check(BufferState::Decode(bytes, decoded), "decode while original providers remain attached");
    check(BufferState::ValidateMovie(decoded, {sound, disc}), "join both successful provider tokens");
    check(!BufferState::ValidateMovie(decoded, {sound}) && !BufferState::ValidateMovie(decoded, {sound, sound}),
          "missing or duplicate Movie token rejected before teardown");
    auto forged = disc;
    forged.part = title::StreamBuffers::MoviePart::SoundRing;
    check(!BufferState::ValidateMovie(decoded, {sound, forged}), "provider part cannot be forged");
    check(BufferState::Encode(decoded, roundtrip) && roundtrip == bytes, "portable exact roundtrip");
    auto bad = bytes;
    bad.push_back(0);
    decoded.free_value = 0xabcdef;
    check(!BufferState::Decode(bad, decoded) && decoded.free_value == 0xabcdef, "strict tail without mutation");
    for (size_t size : {size_t{0}, size_t{3}, size_t{40}, bytes.size() / 2, bytes.size() - 1}) {
        bad.assign(bytes.begin(), bytes.begin() + size);
        check(!BufferState::Decode(bad, decoded) && decoded.free_value == 0xabcdef, "truncated buffer snapshot");
    }
    check(BufferState::Decode(bytes, decoded), "decode preflight");
    title::StreamBuffers::RenewedLoans renewed;
    check(!MusicBuffers::RestoreState(decoded, renewed) && MusicBuffers::Resolve(sound),
          "restore refuses any outstanding current view");
    check(MusicBuffers::ReturnMovie(sound) && MusicBuffers::ReturnMovie(disc), "both providers detach");
    memory::write<uint32_t>(0x3b3fe0 + 2 * 4, 0); // disturb the source reply cache
    check(MusicBuffers::RestoreState(decoded, renewed), "restore on parked owner");
    check(!MusicBuffers::Resolve(sound) && !MusicBuffers::ReturnMovie(disc), "pre-restore tokens are stale");
    if (!renewed.count(sound.sequence) || !renewed.count(disc.sequence)) return failures + 1;
    sound = renewed.at(sound.sequence);
    disc = renewed.at(disc.sequence);
    sound_view = MusicBuffers::Resolve(sound); disc_view = MusicBuffers::Resolve(disc);
    check(sound_view && disc_view &&
          std::all_of(sound_view.data, sound_view.data + sound_view.bytes, [](uint8_t b) { return b == 0xb9; }) &&
          std::all_of(disc_view.data, disc_view.data + disc_view.bytes, [](uint8_t b) { return b == 0xd7; }),
          "rebound Movie providers see the saved bytes");
    check(memory::read<uint32_t>(0x3b3fe0 + 2 * 4) == sound.handle &&
          memory::read<uint32_t>(0x3b3e80 + 2 * 4) == decoded.storage.slots[2].sound_address,
          "restored status publishes saved service identity");
    check(MusicBuffers::Count().allocations == before.allocations &&
          MusicBuffers::Count().closes == before.closes && !MusicBuffers::CanCheckpoint(),
          "restore runs no producer; root composition guard remains");
    std::printf("BufferState production: %d checks, %d failures\n", checks, failures);
    return failures;
}
