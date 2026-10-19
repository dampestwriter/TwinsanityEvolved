#include "games/slus_20909_crash_twinsanity/tests/services/control_state_case.h"

#include <algorithm>
#include <cstdio>

#include "engine/ee/recomp_abi/host_app/memory.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/control_state.h"

int RunControlStateCase(CpuContext& ctx, title::Audio& audio,
    const std::function<int32_t(uint32_t, std::initializer_list<uint32_t>)>& call, bool& busy) {
    using title::SoundControls;
    using title::ControlState;
    int failures = 0, checks = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL controls restore: %s\n", name); }
    };
    check(SoundControls::Reset(), "empty previous batch");
    std::vector<uint8_t> samples(32, 0x17);
    samples[0] = 0; samples[1] = 4; samples[16] = 0; samples[17] = 3;
    check(audio.StoreSound(77, samples) && audio.SoundBankLoaded(77), "real asset");
    check(call(0x1e18e8, {77, 4, 0x2500, 0x1700, 0x1000, 0, 7, 0}) == 0, "queued leased play");
    check(call(0x1e6b50, {4, 0xc00}) == 0 && call(0x1e3e80, {4, 0x2000, 0x1000}) == 0,
          "queued controls after play");
    check(audio.ReleaseSound(77), "unload before accepted play");
    const auto before = audio.CaptureLoads();
    SoundControls::State saved;
    std::string error;
    const auto count_before = SoundControls::Count();
    check(SoundControls::CaptureState(saved, &error), "capture pending batch");
    check(saved.pending.size() == 3 && saved.words == 19 && saved.requested[4] == 1,
          "complete pending ownership and accounting");
    std::vector<uint8_t> bytes;
    check(ControlState::Encode(saved, bytes, &error), "portable encode");
    check(SoundControls::Count().batches == count_before.batches && !audio.OwnsVoice(4),
          "capture does not accept or start");
    SoundControls::State decoded;
    check(ControlState::Decode(bytes, decoded, &error), "portable decode");
    check(decoded.pending[0].voice.player.Capture().samples != saved.pending[0].voice.player.Capture().samples &&
          *decoded.pending[0].voice.player.Capture().samples == samples, "owned serialized asset bytes");
    for (size_t length = 0; length < bytes.size(); ++length) {
        auto unchanged = decoded;
        check(!ControlState::Decode(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length), unchanged) &&
              unchanged.pending.size() == 3 && unchanged.requested[4] == 1, "truncated state does not publish");
    }
    auto malformed = decoded;
    ++malformed.requested[4];
    check(!SoundControls::RestoreState(malformed) && !audio.OwnsVoice(4), "reject unowned readiness");
    malformed = decoded;
    ++malformed.words;
    check(!SoundControls::RestoreState(malformed), "reject incorrect batch budget");
    malformed = decoded;
    malformed.pending[0].voice.player = title::SampleVoice{};
    check(!SoundControls::RestoreState(malformed), "reject lost lease");
    const auto render = [&] {
        std::vector<float> out;
        for (unsigned tick = 0; tick < 40; ++tick) {
            if (tick == 7) { call(0x1e6dd0, {}); call(0x1e2958, {0}); }
            if (tick == 10) { call(0x1e6e18, {}); call(0x1e2958, {0}); }
            if (tick == 20) { call(0x1e6d30, {4}); call(0x1e2958, {0}); }
            const auto pcm = audio.MixTick();
            out.insert(out.end(), pcm.begin(), pcm.end());
        }
        return out;
    };
    check(call(0x1e2958, {0}) >= 0 && audio.OwnsVoice(4), "original accepted batch");
    const auto original = render();
    check(std::any_of(original.begin(), original.end(), [](float v) { return v != 0; }), "audible oracle");
    check(SoundControls::Reset(), "discard post-snapshot world");
    uint64_t generation = 0;
    check(audio.RestoreLoads(before, generation), "restore playback owner");
    memory::write<uint8_t>(0x2e79a8 + 4, 0x5a);
    const auto batches_before_restore = SoundControls::Count().batches;
    check(SoundControls::RestoreState(decoded, &error) && !audio.OwnsVoice(4), "restore unsent play");
    check(memory::read<uint8_t>(0x2e79a8 + 4) == 0x5a &&
          SoundControls::Count().batches == batches_before_restore, "restore neither sends nor writes game status");
    busy = true;
    check(call(0x1e2958, {1}) == -1 && !audio.OwnsVoice(4), "failed restored send retains lease");
    busy = false;
    check(call(0x1e2958, {0}) >= 0 && audio.OwnsVoice(4), "restored accepted batch");
    const auto restored = render();
    check(original == restored, "32000 resumed frames exactly equal");
    check(SoundControls::AdvanceBoundary(81), "save control clock");
    check(SoundControls::CaptureState(saved), "capture clocked state");
    check(SoundControls::AdvanceBoundary(99) && SoundControls::RestoreState(saved), "restore earlier epoch anchor");
    const auto boundaries = SoundControls::Count().boundaries;
    check(SoundControls::AdvanceBoundary(81) && !SoundControls::AdvanceBoundary(80) &&
          SoundControls::Count().boundaries == boundaries, "resume preserves once-only control pulse");

    // Requests issued during delivery must stay in the next finite batch,
    // including after replacing that batch from a portable checkpoint.
    check(SoundControls::Reset(), "clear sample owner queue");
    audio.StopVoices();
    std::vector<uint32_t> delivered;
    bool callback_queued = false;
    check(SoundControls::BindMusic([&](const title::MusicRequest& r) {
        delivered.push_back(r.offset);
        SoundControls::State during;
        check(!SoundControls::CaptureState(during), "capture refuses active delivery");
        if (!callback_queued) {
            callback_queued = true;
            title::BufferRequest next;
            next.kind = title::BufferRequest::Kind::Close; next.stream = 2;
            check(SoundControls::StageBuffer(ctx, next, 3), "callback queues next batch");
        }
    }, [] {}), "music consumer");
    check(SoundControls::BindBuffers([&](const title::BufferRequest& r) {
        delivered.push_back(r.kind == title::BufferRequest::Kind::Close ? 3 : r.size);
    }), "buffer consumer");
    title::MusicRequest music;
    music.file = 3; music.offset = 123; music.size = 456;
    title::BufferRequest buffer;
    buffer.stream = 2; buffer.size = 0x40000; buffer.sound_address = 0x18000;
    check(SoundControls::StageMusic(ctx, music, 8) && SoundControls::StageBuffer(ctx, buffer, 7),
          "ordered cross-service requests");
    check(SoundControls::CaptureState(saved) && ControlState::Encode(saved, bytes) &&
          ControlState::Decode(bytes, decoded), "queued music/buffer snapshot");
    check(SoundControls::Reset() && SoundControls::BindMusic({}, {}), "remove a required owner");
    check(!SoundControls::RestoreState(decoded), "missing owner refuses before mutation");
    check(SoundControls::BindMusic([&](const title::MusicRequest& r) {
        delivered.push_back(r.offset);
        SoundControls::State during;
        check(!SoundControls::CaptureState(during), "restored delivery cannot be captured");
        title::BufferRequest next;
        next.kind = title::BufferRequest::Kind::Close; next.stream = 2;
        check(SoundControls::StageBuffer(ctx, next, 3), "restored callback queues next batch");
    }, [] {}) && SoundControls::RestoreState(decoded), "reattach exact request owner");
    check(call(0x1e2958, {0}) >= 0 && delivered == std::vector<uint32_t>{123, 0x40000},
          "saved finite prefix ordering");
    check(SoundControls::CaptureState(saved) && saved.pending.size() == 1 && saved.words == 3,
          "callback work remains pending");
    check(ControlState::Encode(saved, bytes) && ControlState::Decode(bytes, decoded) &&
          SoundControls::Reset() && SoundControls::RestoreState(decoded), "callback batch survives restore");
    check(call(0x1e2958, {0}) >= 0 && delivered == std::vector<uint32_t>{123, 0x40000, 3},
          "callback delivery exactly once");
    check(SoundControls::Reset() && SoundControls::BindMusic({}, {}) && SoundControls::BindBuffers({}),
          "detach isolated consumers");
    std::printf("ControlState: %d checks, %d failures, 32000 compared stereo frames\n", checks, failures);
    return failures;
}
