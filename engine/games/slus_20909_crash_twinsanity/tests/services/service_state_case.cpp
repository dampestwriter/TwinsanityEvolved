#include "games/slus_20909_crash_twinsanity/tests/services/service_state_case.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/sound/mixer.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/service_state.h"

int RunServiceStateCase(title::Audio& audio, uint64_t& tick, bool playing,
                        ps2r::sound::AudioMixer* output) {
    using namespace title;
    int checks = 0, failures = 0;
    std::string error;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL ServiceState %s: %s\n", name, error.c_str()); }
        return value;
    };
    ServiceState::State captured, decoded;
    if (!check(ServiceState::Capture(audio, captured, &error), "capture production owners")) return failures;
    std::vector<uint8_t> bytes;
    if (!check(ServiceState::Encode(audio, captured, bytes, &error), "encode production owners") ||
        !check(ServiceState::Decode(audio, bytes, decoded, &error), "decode complete owners")) return failures;
    const auto saved_tick = tick;
    const auto saved_reads = StreamLibrary::Scheduling()->Count();
    const auto saved_music = MusicLibrary::Count();
    ps2r::app::SessionServices session(true, &audio, output);
    const auto saved_output = output ? output->Inspect() : ps2r::sound::AudioMixer::State{};
    ps2r::savestate::SectionRegistry registry;
    check(registry.Register(session.Section()), "register required application owner");
    const auto image = registry.Capture();
    check(session.CanCheckpoint(&error) && registry.Validate(image, &error) &&
          session.ValidateImage(image, saved_tick, &error), "application snapshot joins saved clock");
    check(!registry.Validate({}, &error) && !session.ValidateImage({}, saved_tick, &error),
          "missing selected owner refuses before reset");
    check(!session.ValidateImage(image, saved_tick + 1, &error),
          "application rejects different game and service clocks");
    auto wrong = image;
    wrong.push_back(image.front());
    check(!session.ValidateImage(wrong, saved_tick, &error), "duplicate application owner rejected");
    wrong = image; wrong.front().flags = 0;
    check(!registry.Validate(wrong, &error), "optional service state cannot replace required owner");
    wrong = image; wrong.front().payload[4] = 0;
    check(!registry.Validate(wrong, &error), "file-only payload cannot replace selected Audio");
    if (output) {
        ps2r::app::SessionServices shared_mixer(true, &audio);
        check(!shared_mixer.ValidateImage(image, saved_tick, &error), "owned output cannot load with shared mixer");
        std::vector<uint8_t> shared_bytes;
        shared_mixer.Section().serialize(shared_bytes);
        wrong = image;
        wrong.front().version = ps2r::app::SessionServices::kVersion;
        wrong.front().payload = std::move(shared_bytes);
        check(!session.ValidateImage(wrong, saved_tick, &error), "owned mixer cannot load missing output state");
        wrong = image;
        wrong.front().payload[8] = 0; wrong.front().payload[9] = 0;
        wrong.front().payload[10] = 0xc0; wrong.front().payload[11] = 0x7f; // NaN master
        check(!registry.Validate(wrong, &error), "invalid output rejected before service restore");
    }
    ps2r::app::SessionServices inactive(false, nullptr);
    check(!inactive.ValidateImage(image, saved_tick, &error) &&
          inactive.ValidateImage({}, saved_tick, &error), "unselected owner rejects selected payload");
    const auto after_preflight = registry.Capture();
    check(after_preflight.front().payload == image.front().payload &&
          tick == saved_tick && MusicLibrary::Count().reads == saved_music.reads &&
          MusicLibrary::Count().delivered == saved_music.delivered,
          "preflight and capture do not mutate commands read jobs or Audio");
    StreamBuffers::RenewedLoans renewed;
    check(ServiceState::ValidateMovie(decoded, {}, &error), "no external Movie claims");
    // OLD SOUND_P5: malformed or partially joined state must fail before
    // invalidating the live world. Exercise boundaries, not every byte of MBs.
    for (const auto size : {size_t{0}, size_t{3}, size_t{7}, bytes.size() / 2, bytes.size() - 1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(), bytes.begin() + size);
        ServiceState::State ignored;
        check(!ServiceState::Decode(audio, bad, ignored, &error), "reject truncated service state");
    }
    auto bad = decoded;
    bad.controls.words += 1;
    check(!ServiceState::Validate(audio, bad, &error), "reject unaccounted queued command");
    bad = decoded;
    ++bad.music.requested[2];
    check(!ServiceState::Validate(audio, bad, &error), "reject orphaned readiness request");
    bad = decoded;
    bad.files.streams.schedule.tick = 0;
    check(!ServiceState::Validate(audio, bad, &error), "reject owner newer than saved game clock");
    if (decoded.music.streams[2].owned) {
        bad = decoded;
        bad.audio.playback.voices[0].music_generation = decoded.music.generation + 1;
        check(!ServiceState::Validate(audio, bad, &error), "reject unmatched audible generation");
        for (const auto& read : decoded.files.streams.schedule.reads) {
            if (read.second.payload->kind != FileSchedule::Destination::Music) continue;
            bad = decoded;
            auto payload = std::make_shared<FileSchedule::Payload>(*read.second.payload);
            ++payload->sequence;
            bad.files.streams.schedule.reads.at(read.first).payload = payload;
            check(!ServiceState::Validate(audio, bad, &error), "reject mismatched pending music ticket");
        }
    }
    auto render = [&] {
        std::vector<float> pcm;
        for (unsigned frame = 0; frame < 8; ++frame) {
            ++tick;
            StreamLibrary::AdvanceBoundary(tick);
            MusicLibrary::AdvanceBoundary(tick);
            SoundControls::AdvanceBoundary(tick);
            if (output) {
                ps2r::sound::Mixer mixer(*output);
                mixer.Pump();
                pcm.insert(pcm.end(), mixer.Output(), mixer.Output() + mixer.OutputFrames() * 2);
            } else {
                const auto samples = audio.MixTick();
                pcm.insert(pcm.end(), samples.begin(), samples.end());
            }
        }
        return pcm;
    };
    std::vector<float> expected;
    if (playing) expected = render();
    if (output) check(output->SetMaster(0) && output->SetGroup(0, 0, 0), "perturb future output state");
    tick = saved_tick; // Session restores the game clock before service owners.
    error.clear();
    if (!check(registry.Restore(image, &error), "application restores joined owners"))
        return failures;
    if (output) check(output->Inspect().master == saved_output.master &&
        output->Inspect().left == saved_output.left && output->Inspect().right == saved_output.right,
        "application restores owned gains");
    check(renewed.empty() && tick == saved_tick, "restore neither creates loans nor advances time");
    if (playing) {
        const auto actual = render();
        check(actual == expected, "paired decoder/envelope/ramp resumes bit exactly");
        check(std::any_of(actual.begin(), actual.end(), [](float value) { return value != 0; }),
              "resumed owned playback is audible");
        tick = saved_tick;
        check(registry.Restore(image, &error), "application repeat restore invalidates old generation");
    }
    check(StreamLibrary::Scheduling()->Count().issued == saved_reads.issued &&
          MusicLibrary::Count().reads == saved_music.reads &&
          MusicLibrary::Count().delivered == saved_music.delivered,
          "restore performs no new file read or delivery");
    ServiceState::State after;
    check(ServiceState::Capture(audio, after, &error), "recapture renewed ownership");
    if (captured.music.streams[2].owned)
        check(after.music.streams[2].generation > captured.music.streams[2].generation,
              "old music callback cannot alias restored stream");
    check(after.controls.pending.size() == captured.controls.pending.size() &&
          after.files.streams.schedule.reads.size() == captured.files.streams.schedule.reads.size(),
          "accepted and pending work retained exactly once");
    check(after.files.streams.schedule.service_time &&
          after.files.streams.schedule.service.settled_ms == captured.files.streams.schedule.service.settled_ms &&
          after.files.streams.schedule.service.sequence == captured.files.streams.schedule.service.sequence,
          "composed restore preserves owned service instant and headroom progress");
    std::printf("ServiceState: checks=%d failures=%d phase=%u queued=%zu bytes=%zu resumed_frames=%u\n",
        checks, failures, static_cast<unsigned>(captured.music.streams[2].phase),
        captured.controls.pending.size(), bytes.size(), playing ? 6400u : 0u);
    return failures;
}

int RunServiceLoanStateCase(title::Audio& audio, title::StreamBuffers::Loan& sound,
                           title::StreamBuffers::Loan& disc) {
    using namespace title;
    int failures = 0, checks = 0;
    std::string error;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL ServiceLoans %s: %s\n", name, error.c_str()); }
        return value;
    };
    ServiceState::State captured, decoded;
    std::vector<uint8_t> bytes;
    StreamBuffers::RenewedLoans renewed;
    const std::vector<StreamBuffers::Loan> references{sound, disc};
    if (!check(ServiceState::Capture(audio, captured, &error), "capture both Movie loans") ||
        !check(ServiceState::Encode(audio, captured, bytes, &error), "encode loans and owners") ||
        !check(ServiceState::Decode(audio, bytes, decoded, &error), "decode loans and owners")) return failures;
    ps2r::app::SessionServices session(true, &audio);
    check(!session.CanCheckpoint(&error), "application requires separate active Movie consumer state");
    check(ServiceState::ValidateMovie(decoded, references, &error), "both external owners joined");
    check(!ServiceState::ValidateMovie(decoded, {sound}, &error), "missing OpenStream loan rejected");
    check(!ServiceState::ValidateMovie(decoded, {disc}, &error), "missing Prepare loan rejected");
    check(!ServiceState::ValidateMovie(decoded, {sound, sound}, &error), "duplicated owner rejected");
    auto forged = disc; ++forged.generation;
    check(!ServiceState::ValidateMovie(decoded, {sound, forged}, &error), "stale owner rejected");
    check(!ServiceState::Restore(audio, decoded, references, renewed, &error),
          "live Movie views refuse mutation");
    auto sound_view = MusicBuffers::Resolve(sound), disc_view = MusicBuffers::Resolve(disc);
    if (!check(sound_view && disc_view, "refused restore keeps both current views")) return failures;
    const auto sound_byte = sound_view.data[0], disc_byte = disc_view.data[0];
    check(MusicBuffers::ReturnMovie(sound) && MusicBuffers::ReturnMovie(disc), "Movie returns both old views");
    if (!check(ServiceState::Restore(audio, decoded, references, renewed, &error), "restore all service owners"))
        return failures;
    check(renewed.size() == 2 && !MusicBuffers::Resolve(sound) && !MusicBuffers::Resolve(disc),
          "old tokens invalidated without a fresh borrow");
    sound = renewed.at(sound.sequence); disc = renewed.at(disc.sequence);
    sound_view = MusicBuffers::Resolve(sound); disc_view = MusicBuffers::Resolve(disc);
    check(sound_view && disc_view && sound_view.data[0] == sound_byte && disc_view.data[0] == disc_byte,
          "reattached views retain saved bytes");
    check(!MusicBuffers::CanPlay(2), "music cannot reuse either live Movie range");
    std::printf("ServiceLoans: checks=%d failures=%d renewed=%zu\n", checks, failures, renewed.size());
    return failures;
}
