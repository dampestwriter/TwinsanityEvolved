#include <cstdio>
#include <cstring>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"

int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL %s\n", name); }
    };
    using title::MusicAsset;
    std::vector<uint8_t> mono(48 + 64);
    std::memcpy(mono.data(), "MSVp", 4);
    mono[16] = 0; mono[17] = 0; mono[18] = 0x7d; mono[19] = 0;
    for (size_t block = 0; block < 4; ++block) {
        mono[48 + block * 16] = 0x20;
        for (size_t n = 2; n < 16; ++n) mono[48 + block * 16 + n] = static_cast<uint8_t>(block * 16 + n);
    }
    MusicAsset::Data data;
    check(MusicAsset::Read(mono, {}, data), "MSVp mono load");
    check(data.count == 1 && data.header_bytes == 48 && data.source_rate == 32000 &&
        data.tracks[0]->size() == 64, "header fields describe owned audio");
    for (size_t at = 0; at < 64; ++at)
        if (at % 16 != 1) check(data.tracks[0]->at(at) == mono.at(at + 48), "header never reaches predictor");
    check(data.tracks[0]->at(1) == 4 && data.tracks[0]->at(49) == 1, "one shot has one logical end");
    std::memcpy(mono.data(), "VAGp", 4);
    check(MusicAsset::Read(mono, {1, 0, 0, true}, data), "VAGp mono loop");
    check(data.tracks[0]->at(49) == 3, "loop end repeats at asset beginning");
    for (size_t n = 1; n < 48; ++n)
        check(MusicAsset::HeaderBytes(mono.data(), n) == 0, "truncated header not read out of bounds");
    check(!MusicAsset::Read({}, {}, data), "empty request fails");
    check(!MusicAsset::Read(std::vector<uint8_t>(47), {}, data), "partial block fails");
    check(!MusicAsset::Read(std::vector<uint8_t>(48), {3, 16, 0, false}, data), "unsupported track count fails");

    // Distinct block/side contents ensure a swapped or repeated first
    // interleave block cannot satisfy the source-layout pin.
    std::vector<uint8_t> pair(4 * 64);
    for (size_t chunk = 0; chunk < 4; ++chunk)
        for (size_t at = 0; at < 64; ++at) pair[chunk * 64 + at] = static_cast<uint8_t>(chunk * 37 + at);
    check(MusicAsset::Read(pair, {2, 64, 32, true}, data), "interleaved pair load");
    check(data.tracks[0]->size() == 96 && data.tracks[1]->size() == 96, "trim both final sides equally");
    for (size_t side = 0; side < 2; ++side)
        for (size_t at = 0; at < 96; ++at) {
            if (at % 16 == 1) continue;
            const size_t source = at < 64 ? side * 64 + at : (2 + side) * 64 + at - 64;
            check(data.tracks[side]->at(at) == pair.at(source), "ordered track deinterleave");
        }
    const auto previous = data.tracks[0];
    check(!MusicAsset::Read(pair, {2, 0, 0, true}, data), "zero block fails");
    check(!MusicAsset::Read(pair, {2, 64, 80, true}, data), "oversized last block fails");
    check(!MusicAsset::Read(pair, {2, 64, 17, true}, data), "unaligned last block fails");
    pair.pop_back();
    check(!MusicAsset::Read(pair, {2, 64, 32, true}, data) && data.tracks[0] == previous,
          "failed pair load cannot publish half an asset");
    // Generation ownership prevents a stopped stream from touching a later
    // sample or music assignment of the same channel.
    title::Audio audio;
    check(audio.ReserveMusicVoice(2, 5, 0, 0xaaa, 8000, 7000, 8000, 7000), "reserve stream voice");
    check(!audio.StartMusicVoice(2, 4, 0, previous), "stale generation cannot start");
    check(audio.StartMusicVoice(2, 5, 0, previous), "current generation starts");
    audio.StopMusicVoice(2, 4, 0);
    check(!audio.IsVoiceFree(0), "stale stop cannot retire current voice");
    check(audio.ReserveMusicVoice(3, 6, 0, 0xaaa, 8000, 7000, 8000, 7000), "reuse owned channel");
    audio.StopMusicVoice(2, 5, 0);
    check(!audio.IsVoiceFree(0) && audio.Inspect().voices[0].music_stream == 3, "previous stream cannot stop reused channel");
    std::printf("MusicAsset: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
