#include "games/slus_20909_crash_twinsanity/tests/services/music_oracle_case.h"

#include "engine/services/read_timing.h"
#include "games/slus_20909_crash_twinsanity/tests/services/buffer_state_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/sample_output.h"
#include "games/slus_20909_crash_twinsanity/tests/services/service_state_case.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/sound/mixer.h"
#include "engine/sound/audio_mixer.h"
#include "engine/sound/fmv_pcm_input.h"
#include "tests/sound/pcm_source_fixture.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"

using namespace ps2r::title::stream_irx;
namespace {
uint32_t Little32(const std::vector<uint8_t>& bytes, size_t at) {
    return bytes.at(at) | static_cast<uint32_t>(bytes.at(at + 1)) << 8 |
        static_cast<uint32_t>(bytes.at(at + 2)) << 16 | static_cast<uint32_t>(bytes.at(at + 3)) << 24;
}
}
namespace {
// Readiness at the buffer fill (TR multistream.h: the stream keys on once its
// buffers are full): the owned read charges the parent's attached 0x40000
// bytes, not the whole window, and is due within that fill's drive time.
int CheckBufferFillReadiness(uint32_t size, uint32_t offset, uint64_t tick, int& checks) {
    int failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL music %s\n", name); }
    };
    FileSchedule::State schedule;
    check(StreamLibrary::Scheduling()->Capture(schedule), "owned schedule snapshot");
    const auto read = schedule.reads.find(FileSchedule::MusicObject(2));
    check(read != schedule.reads.end(), "owned music read pending");
    if (read == schedule.reads.end()) return failures;
    const uint32_t fill = std::min<uint32_t>(0x40000, size); // the parent's attached buffer
    const uint32_t fill_sectors = (offset % 2048 + fill + 2047) / 2048;
    if (read->second.sectors != fill_sectors)
        std::printf("MUSIC_FILL size=%u offset=%u sectors=%u fill_sectors=%u buffer=%u due=%llu tick=%llu\n",
                    size, offset, read->second.sectors, fill_sectors, title::MusicBuffers::BufferBytes(2),
                    static_cast<unsigned long long>(read->second.due), static_cast<unsigned long long>(tick));
    check(read->second.sectors == fill_sectors, "readiness charges the buffer fill, not the window");
    const double fill_ms = ReadTiming::kLongSeekMs + fill_sectors / ReadTiming::kSectorsPerMs;
    const double backlog_ms = std::max(0.0, schedule.timing.busy_ms - fill_ms -
                                       StreamLibrary::Scheduling()->ServiceInstantMs());
    check(read->second.due >= tick &&
          static_cast<double>(read->second.due - tick) * ReadTiming::kTickMs <=
              fill_ms + backlog_ms + 2 * ReadTiming::kTickMs,
          "ready within the fill's drive time");
    check(size <= 0x40000 || title::MusicLibrary::Count().fills > 0, "fill-granularity read counted");
    return failures;
}
// The rest of the window is drive time charged as the voice consumes: one
// half-buffer refill per half consumed, on the shared read position. This
// fixture's instant boundary model charges no drive time (FileSchedule's
// ServiceReadCharge pins the charge itself).
int CheckWindowRefills(uint64_t& tick, ps2r::sound::Mixer& mixer, int& checks) {
    int failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL music %s\n", name); }
    };
    const auto refills_before = title::MusicLibrary::Count().refills;
    FileSchedule::State schedule;
    const auto busy_before = StreamLibrary::Scheduling()->Capture(schedule) ? schedule.timing.busy_ms : 0.0;
    for (unsigned extra = 0; extra < 4000 &&
         title::MusicLibrary::Count().refills == refills_before &&
         title::MusicLibrary::Inspect(2).phase == title::MusicLibrary::Phase::Playing; ++extra) {
        ++tick;
        StreamLibrary::AdvanceBoundary(tick);
        title::MusicLibrary::AdvanceBoundary(tick);
        title::SoundControls::AdvanceBoundary(tick);
        mixer.Pump();
    }
    const auto count = title::MusicLibrary::Count();
    check(count.refills > refills_before && count.refill_bytes > 0, "window remainder charged as refills");
    check(StreamLibrary::Scheduling()->Capture(schedule) && schedule.timing.busy_ms >= busy_before,
          "refill never rewinds the shared drive position");
    return failures;
}
}

int RunMusicOracle(CpuContext& ctx, StreamIrxPlugin& plugin, ps2r::sound::SpuCore& spu,
    ps2r::iop::IopMemory& iop, StreamRpcServer& face, const LoaderFileIo::Deps& deps,
    uint64_t& tick, IsoFs& iso, ReadEngine& reader) {
    int failures = 0, checks = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL music %s\n", name); }
    };
    constexpr uint32_t gp = 0x3114f0, sp = 0x700000, status = 0x510000, path = 0x500000;
    auto call = [&](uint32_t address, std::initializer_list<uint32_t> args) {
        ctx.cpuRegs.GPR.r[28].UD[0] = gp; ctx.cpuRegs.GPR.r[29].UD[0] = sp;
        ctx.cpuRegs.GPR.r[31].UD[0] = 0x100100;
        unsigned reg = 4;
        for (const auto arg : args) {
            if (reg < 12) ctx.cpuRegs.GPR.r[reg].UD[0] = arg;
            else memory::write<uint32_t>(sp + (reg - 12) * 4, arg);
            ++reg;
        }
        DispatchTable::Invoke(address, ctx);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    auto send = [&] { check(call(0x1e2958, {0}) >= 0, "accepted ordered batch"); };
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, StreamLibrary::Body body) { return DispatchTable::Install(address, std::move(body)); };
    auto remaining = [&](uint32_t channel, uint16_t l, uint16_t r) {
        const auto entry = channel / 24 | (channel % 24) << 1;
        spu.SetParam(entry, l); spu.SetParam(entry | 0x100, r);
    };
    title::Audio audio;
    title::StreamBuffers buffers(2 * 1024 * 1024);
    uint64_t original_music = 0;
    uint64_t original_buffers = 0;
    auto buffer_capture = [&](uint32_t address) {
        const auto body = capture(address);
        return StreamLibrary::Body{[&, address, body](CpuContext& context, uint32_t entry) {
            if (address != 0x1e3308 && address != 0x1e1f48) ++original_buffers;
            body(context, entry);
        }};
    };
    auto music_capture = [&](uint32_t address) {
        const auto body = capture(address);
        return StreamLibrary::Body{[&, address, body](CpuContext& context, uint32_t entry) {
            if (address != 0x1e3308 && address != 0x1e1f48) ++original_music;
            body(context, entry);
        }};
    };
    // Load only metadata directly. Both music playback arms below issue the
    // same source file-window/play/prepare/start service calls.
    IsoFs::Entry header{};
    check(iso.Find("CRASH6/MUSIC.MH", header), "source music header exists");
    if (!header.size) return failures + 1;
    std::vector<uint8_t> metadata((header.size + 2047) / 2048 * 2048);
    reader.ReadSectors(header.lba, static_cast<uint32_t>(metadata.size() / 2048), metadata.data());
    const auto block_bytes = Little32(metadata, 4);
    uint64_t compared_frames = 0, shared_frames = 0, host_frames = 0;
    for (uint32_t scenario = 0; scenario < 4; ++scenario) {
        const uint32_t track = scenario % 2 ? 7 : 0;
        const bool looping = scenario >= 2;
        const size_t at = 8 + track * 20;
        const bool stereo = Little32(metadata, at) == 1;
        const uint32_t size = looping ? (stereo ? block_bytes * 4 : 0x6030) : Little32(metadata, at + 4);
        const uint32_t offset = Little32(metadata, at + 8),
                       rate = Little32(metadata, at + 12), end = Little32(metadata, at + 16);
        std::vector<float> model_pcm;
        // A test-only middle arm isolates the production shared-control
        // adapters while music/voices remain with their original owner.
        // No sample play or bank operation is issued in that arm.
        for (unsigned arm : {0u, 1u, 2u, 3u}) {
            const bool selected = arm >= 2, controls = arm != 0, host_mix = arm == 3;
            LoaderFileIo::ResetState();
            check(StreamLibrary::Install(capture, install, true), "file owner selected in every arm");
            LoaderFileIo::Bind(deps);
            check(StreamLibrary::Scheduling()->SetServiceTime(selected),
                  "composed selected music uses owned logical deadlines");
            check(title::SoundControls::Reset(), "clear previous controls");
            audio.Reset();
            SampleOutput sample_output(audio);
            ps2r::sound::AudioMixer audio_mixer(sample_output);
            auto* output_owner = host_mix ? &audio_mixer : nullptr;
            check(title::SoundBanks::Install(capture, install, selected ? &audio : nullptr),
                  "compose production bank owner");
            check(title::SoundControls::Install(capture, install, controls ? &audio : nullptr, remaining),
                  "sound arm selection");
            check(title::MusicLibrary::Install(music_capture, install, selected ? &audio : nullptr),
                  "music arm selection");
            check(title::MusicBuffers::Install(buffer_capture, install, selected ? &buffers : nullptr),
                  "buffer arm selection");
            const auto buffer_calls_before = original_buffers;
            const auto control_before = title::SoundControls::Count();
            call(0x1e6e60, {}); // same source reset in all three arms
            for (uint32_t repeat = 0; repeat < 3; ++repeat)
                for (uint32_t group = 1; group <= 4; ++group)
                    check(call(0x1e7cb8, {group << 16, 0x1000, 0x1000}) == 0, "shared group initialization");
            send();
            const auto heap_blocks_before = plugin.Sysmem().Heap().LiveBlocks();
            const auto heap_pointer_before = plugin.Sysmem().Heap().HeapPtr();
            // Source AttachBuffer calls the allocation and resize service for
            // the parent only. The title opens the paired second stream with
            // a zero buffer size, which returns before any service call, and
            // never closes a buffer for it; the child channel streams through
            // the parent's buffer and carries only the paired sound address
            // on AddStreamChannel (TR game/sound.cpp CreateMusicBuffers,
            // game/filestream.cpp FileStreamAttachBuffer/DetachBuffer).
            check(call(0x1e1ca8, {2, 0x18000, 0x40000}) == 0, "parent buffer");
            check(call(0x1e1e10, {2, 0x4000}) == 0, "parent window resize");
            call(0x1e77b8, {});
            send();
            // The source reply consumer ignores a repeated maintenance
            // counter. Cross the same service boundary before observing
            // the completed allocation, without pumping IOP in the owned arm.
            if (selected) {
                ++tick;
                StreamLibrary::AdvanceBoundary(tick);
                title::MusicLibrary::AdvanceBoundary(tick);
                title::SoundControls::AdvanceBoundary(tick);
            } else deps.pump_module(ctx);
            send();
            call(0x1e3308, {2, status});
            check(memory::read<uint32_t>(status + 0x38) != 0, "source ChannelBufferAddress has a lender");
            check(memory::read<uint32_t>(status) > 0x40000, "real capacity satisfies next music-player gate");
            if (selected) {
                check(memory::read<uint32_t>(status + 0x38) == buffers.Inspect(2).handle &&
                    buffers.Used() == 0x40000 && memory::read<uint32_t>(status) == buffers.Available(),
                    "owned handle and real capacity published through source status");
                check(plugin.Sysmem().Heap().LiveBlocks() == heap_blocks_before &&
                    plugin.Sysmem().Heap().HeapPtr() == heap_pointer_before &&
                    original_buffers == buffer_calls_before, "allocation/query execute no old producer or heap allocation");
            } else {
                check(original_buffers > buffer_calls_before &&
                    plugin.Sysmem().Heap().LiveBlocks() > heap_blocks_before,
                    "comparison arm really allocates through STREAM/sysmem");
            }
            const char* name = "crash6/music.mb";
            for (size_t i = 0; i <= std::strlen(name); ++i) memory::write<uint8_t>(path + i, name[i]);
            memory::write<int32_t>(gp - 0x74d8, 3);
            ctx.cpuRegs.GPR.r[4].UD[0] = path;
            LoaderFileIo::Open(ctx);
            check(ctx.cpuRegs.GPR.r[2].SL[0] == 3, "open music file");
            send();
            for (int vi = 0; vi < 48; ++vi) spu.VoiceAt(vi).Silence();
            spu.ApplyQueued();
            spu.SetMasterVolume(1);
            for (int core = 0; core < 2; ++core) { spu.SetMvolL(core, 1); spu.SetMvolR(core, 1); }
            spu.RepublishReadbacks();
            const auto replaced_before = original_music, kon_before = spu.KonCount();
            const auto cd_before = plugin.Cdvdman().ReadsIssued();
            const auto bytes_before = plugin.Libsd().VoiceTransBytes();
            call(0x1e2ee0, {3, offset, size});
            check(call(0x1e1500, {3, 2, 0, 0, 0, static_cast<uint32_t>(title::Audio::PitchOfRate(rate)),
                                looping ? 0u : 1u, 0, 0}) == 2,
                  "parent play staged");
            if (stereo) {
                call(0x1e2fd0, {2, 0, block_bytes});
                call(0x1e30d0, {3, 2, 1, 1, 0, 0, 0x1c000});
                check(call(0x1e7800, {2, end}) == 0, "final interleave extent");
            }
            check(call(0x1e7990, {2}) == 0, "prepare before start");
            if (selected) check(RunServiceStateCase(audio, tick, false, output_owner) == 0, "queued music snapshot");
            const auto refusals_before = title::MusicLibrary::Count();
            send();
            if (selected) {
                // The buffer-less child must not be refused by the buffer
                // owner: its Reserve checks the parent's owned storage.
                const auto refusals = title::MusicLibrary::Count();
                check(refusals.refused_buffers == refusals_before.refused_buffers &&
                      refusals.refused_window == refusals_before.refused_window &&
                      refusals.refused_voice == refusals_before.refused_voice &&
                      refusals.errors == refusals_before.errors,
                      "title-shaped play/child pair is not refused");
                check(!stereo || title::MusicLibrary::Inspect(3).phase == title::MusicLibrary::Phase::Reading,
                      "child shares the parent's reading buffer");
                check(title::MusicLibrary::Inspect(2).phase == title::MusicLibrary::Phase::Reading,
                      "no early readiness");
                check(RunServiceStateCase(audio, tick, false, output_owner) == 0, "pending read snapshot");
                failures += CheckBufferFillReadiness(size, offset, tick, checks);
                check(StreamLibrary::Scheduling()->Wait(FileSchedule::MusicObject(2)), "owned file deadline");
            } else {
                for (unsigned lap = 0; lap < 128; ++lap) {
                    deps.pump_module(ctx);
                    send();
                    call(0x1e3308, {2, status});
                    if (memory::read<uint8_t>(status + 0x12)) break;
                }
            }
            call(0x1e3308, {2, status});
            check(memory::read<uint8_t>(status + 0x12) != 0, "ready after bytes");
            if (selected) {
                check(title::MusicLibrary::Inspect(2).phase == title::MusicLibrary::Phase::Prepared,
                      "prepare parks decoder until start");
                check(audio.IsVoiceFree(0) == 0 && !audio.Inspect().voices[0].player.Playing(),
                      "prepared channel reserved and silent");
                if (stereo) check(audio.IsVoiceFree(1) == 0, "child reserved together");
                check(RunServiceStateCase(audio, tick, false, output_owner) == 0, "prepared pair snapshot");
            }
            check(call(0x1e3e80, {0, 11000, stereo ? 0u : 9000u}) == 0, "parent volume");
            if (stereo) check(call(0x1e3e80, {1, 0, 10000}) == 0, "child volume");
            check(call(0x1e78f0, {2}) == 0, "start prepared pair");
            send();
            std::vector<float> output;
            ps2r::sound::Mixer mixer(spu);
            if (controls) mixer.SetVoiceSource(&sample_output);
            if (host_mix) mixer.SetMixSource(&audio_mixer);
            ps2r::sound::FmvPcmInput movie_pcm(4096);
            // The Movie owner (gain + FIFO) sits outside the sink in both
            // arms: the retained core reads its BVOL shadows, the owned sink
            // reads the owner's Gains() once per pump.
            PcmSourceFixture movie_owner(movie_pcm);
            if (host_mix) audio_mixer.SetMovieSource(&movie_owner);
            else spu.SetFmvInput(&movie_pcm);
            const auto mix_calls = spu.MixCalls();
            const unsigned pulses = looping ? (stereo ? 512 : 128) : 64;
            std::array<int16_t, title::Audio::kFramesPerTick * 2> external{};
            for (unsigned pulse = 0; pulse < pulses; ++pulse) {
                if (selected && pulse == 4)
                    check(RunServiceStateCase(audio, tick, true, output_owner) == 0, "playing pair snapshot");
                if (pulse == 8) { call(0x1e3e80, {0, 8000, stereo ? 0u : 7000u}); send(); }
                if (pulse == 12) { call(0x1e6dd0, {}); send(); }
                if (pulse == 15) { call(0x1e6e18, {}); send(); }
                // OLD SOUND_P5: external PCM is added after voice/core gain
                // and before master/clamp; muting must still consume frames.
                // Exercise those real root stages with identical inputs,
                // including concurrent music and a paused music interval.
                if (pulse == 5) {
                    spu.SetMasterVolume(.8f); spu.SetMvolL(0, .75f); spu.SetMvolR(0, .25f);
                    check(audio_mixer.SetMaster(.8f) && audio_mixer.SetGroup(0, .75f, .25f), "owned gain intent");
                }
                if (pulse >= 9) {
                    for (size_t i = 0; i < external.size(); ++i)
                        external[i] = static_cast<int16_t>(int((i * 43 + pulse * 211) % 64001) - 32000);
                    movie_pcm.Push(external.data(), title::Audio::kFramesPerTick);
                    movie_pcm.GateOpen();
                    spu.SetParam(0xf80, pulse == 10 ? 0 : 0x7fff);
                    spu.SetParam(0x1080, pulse == 10 ? 0 : 0x5fff);
                    movie_owner.gains = {pulse == 10 ? 0.0f : 1.0f, pulse == 10 ? 0.0f : 24575.0f / 32767};
                }
                if (selected) {
                    ++tick;
                    StreamLibrary::AdvanceBoundary(tick);
                    title::MusicLibrary::AdvanceBoundary(tick);
                    title::SoundControls::AdvanceBoundary(tick);
                } else {
                    deps.pump_module(ctx);
                    auto iop_ctx = ps2r::iop::IopContext::ForEntry(0x180000, ModuleLayout::kGp);
                    StreamIrxPlugin::Resolve(0x10ab8)(iop_ctx);
                    if (controls) title::SoundControls::AdvanceBoundary(tick + pulse);
                }
                mixer.Pump();
                output.insert(output.end(), mixer.Output(), mixer.Output() + mixer.OutputFrames() * 2);
                check(movie_pcm.Queued() == 0, "common mixer consumes PCM even while muted");
            }
            check(movie_pcm.Consumed() == (pulses - 9) * title::Audio::kFramesPerTick,
                  "music arms consume identical Movie PCM frames");
            check(spu.MixCalls() == mix_calls + (host_mix ? 0 : pulses),
                  "host output has zero SpuCore mixing calls");
            if (host_mix) {
                check(audio_mixer.Count().channel_samples > 0 && audio_mixer.Count().movie_frames > 0,
                      "host output owns audible music and Movie PCM");
                host_frames += output.size() / 2;
            }
            spu.SetFmvInput(nullptr);
            if (arm == 0) {
                model_pcm = std::move(output);
                check(spu.KonCount() > kon_before && original_music > replaced_before &&
                    plugin.Libsd().VoiceTransBytes() > bytes_before, "model executes original music and uploads");
                std::printf("MUSIC_MODEL track=%u loop=%d slot=%x ready=%u state=%u nax=%x,%x\n",
                    track, looping, iop.Read<uint32_t>(0x15980 + 2 * 0x278 + 0x18),
                    memory::read<uint8_t>(status + 0x12), memory::read<uint8_t>(status + 0x11),
                    spu.VoiceAt(0).Nax(), spu.VoiceAt(1).Nax());
            } else {
                size_t different = 0;
                for (size_t i = 0; i < output.size(); ++i) if (output[i] != model_pcm[i]) {
                    if (different++ < 4) std::printf("MUSIC_DIFF track=%u frame=%zu side=%zu model=%a owned=%a\n",
                                                   track, i / 2, i % 2, model_pcm[i], output[i]);
                }
                check(different == 0, "owned PCM equals original stream pipeline");
                if (selected) {
                    check(original_music == replaced_before && spu.KonCount() == kon_before &&
                        plugin.Cdvdman().ReadsIssued() == cd_before && plugin.Libsd().VoiceTransBytes() == bytes_before,
                        "zero replaced emitters/CD worker/libsd uploads/key-on");
                    if (!looping && size > 0x40000) failures += CheckWindowRefills(tick, mixer, checks);
                    if (!host_mix) compared_frames += output.size() / 2;
                    std::printf("MusicSelfOracle track=%u loop=%d frames=%zu mismatched_samples=%zu bytes=%llu\n",
                        track, looping, output.size() / 2, different,
                        static_cast<unsigned long long>(title::MusicLibrary::Inspect(2).bytes));
                } else {
                    bool owned = false;
                    for (uint32_t voice = 0; voice < 48; ++voice) owned |= audio.OwnsVoice(voice);
                    check(!owned && title::SoundControls::Count().plays == control_before.plays &&
                        title::SoundControls::Count().handoffs > control_before.handoffs &&
                        spu.KonCount() > kon_before, "shared control oracle has retained music and zero owned voices");
                    shared_frames += output.size() / 2;
                    std::printf("SharedControlSelfOracle track=%u loop=%d frames=%zu mismatched_samples=%zu\n",
                        track, looping, output.size() / 2, different);
                }
            }
            check(call(0x1e6c28, {2}) == 0, "stop parent");
            if (!selected) send();
            if (selected) {
                const auto handle = buffers.Inspect(2).handle;
                const auto saved = ctx.cpuRegs.GPR.r[4].UD[0];
                auto sound_loan = title::MusicBuffers::BorrowMovie(ctx, handle,
                    title::StreamBuffers::MoviePart::SoundRing);
                auto disc_loan = title::MusicBuffers::BorrowMovie(ctx, handle,
                    title::StreamBuffers::MoviePart::DiscStream);
                check(sound_loan && disc_loan && sound_loan.sequence != disc_loan.sequence &&
                    ctx.cpuRegs.GPR.r[4].UD[0] == saved, "two production Movie loans preserve caller ABI");
                check(audio.IsVoiceFree(0) == 1 && (!stereo || audio.IsVoiceFree(1) == 1),
                      "loan accepts queued stop before borrowing either range");
                check(!title::MusicBuffers::CanPlay(2) && !title::MusicBuffers::Reset(),
                      "Movie ownership prevents music reuse/reset");
                check(RunBufferStateCase(sound_loan, disc_loan) == 0, "production Movie buffer snapshot");
                check(RunServiceLoanStateCase(audio, sound_loan, disc_loan) == 0,
                      "composed services join both Movie owners");
                check(title::MusicBuffers::ReturnMovie(sound_loan) && !title::MusicBuffers::CanPlay(2),
                      "Prepare return alone cannot reclaim disc loan");
                check(title::MusicBuffers::ReturnMovie(disc_loan) && title::MusicBuffers::CanPlay(2),
                      "both Movie returns restore music ownership");
                // OLD SOUND_P8: stop/reopen generations must never retire a
                // fresh request. Cancel while pending, immediately replace,
                // then close while pending before a late boundary arrives.
                const auto first_generation = title::MusicLibrary::Inspect(2).generation;
                for (unsigned reopen = 0; reopen < 4; ++reopen) {
                    call(0x1e2ee0, {3, offset, size});
                    check(call(0x1e1500, {3, 2, 0, 0, 0, 0xaaa, 1, 0, 0}) == 2, "reopen parent");
                    if (stereo) {
                        call(0x1e2fd0, {2, 0, block_bytes});
                        call(0x1e30d0, {3, 2, 1, 1, 0, 0, 0x1c000});
                        call(0x1e7800, {2, end});
                    }
                    call(0x1e7990, {2});
                    send();
                    check(title::MusicLibrary::Inspect(2).generation > first_generation &&
                        StreamLibrary::Scheduling()->InFlight(FileSchedule::MusicObject(2)),
                        "reopen owns new pending generation");
                    if (reopen % 2) {
                        check(StreamLibrary::Scheduling()->Wait(FileSchedule::MusicObject(2)), "reopen delivery");
                        check(title::MusicLibrary::Inspect(2).phase == title::MusicLibrary::Phase::Prepared &&
                              (!stereo || title::MusicLibrary::Inspect(3).phase == title::MusicLibrary::Phase::Prepared),
                              "old stop cannot retire new ready pair");
                    }
                    const bool close_buffer = reopen == 2;
                    call(close_buffer ? 0x1e6ea8 : 0x1e6c28, {2});
                    send();
                    check(!StreamLibrary::Scheduling()->InFlight(FileSchedule::MusicObject(2)) &&
                        audio.IsVoiceFree(0) && (!stereo || audio.IsVoiceFree(1)), "stop/close cancels pending or ready pair");
                    if (close_buffer) {
                        check(!buffers.Inspect(2).allocated && buffers.Used() == 0,
                              "buffer close frees storage after canceling the read");
                        check(call(0x1e1ca8, {2, 0x18000, 0x40000}) == 0 &&
                            call(0x1e1e10, {2, 0x4000}) == 0, "new allocation after pending close");
                        send();
                        check(buffers.Inspect(2).handle != handle, "pending close cannot alias a reopened buffer");
                    }
                }
                call(0x1e2ee0, {3, offset, size});
                check(call(0x1e1500, {3, 2, 0, 0, 0, 0xaaa, 1, 0, 0}) == 2, "close test starts read");
                call(0x1e7990, {2});
                send();
                check(StreamLibrary::Scheduling()->HasMusicReads() && !title::MusicLibrary::CanCheckpoint(),
                      "pending music requires composed snapshot");
                const auto delivered = title::MusicLibrary::Count().delivered;
                ctx.cpuRegs.GPR.r[4].UD[0] = 3;
                LoaderFileIo::Close(ctx);
                StreamLibrary::AdvanceBoundary(tick + 1000000);
                check(title::MusicLibrary::Count().delivered == delivered &&
                    !StreamLibrary::Scheduling()->HasMusicReads() &&
                    title::MusicLibrary::Inspect(2).phase == title::MusicLibrary::Phase::Failed &&
                    audio.IsVoiceFree(0), "closed file cannot start a late music voice");
                check(title::SoundControls::Reset(), "reset music batch");
                title::MusicLibrary::Reset();
            }
            if (!selected) {
                ctx.cpuRegs.GPR.r[4].UD[0] = 3;
                LoaderFileIo::Close(ctx);
            }
            check(call(0x1e6ea8, {2}) == 0, "close parent buffer service");
            call(0x1e77b8, {});
            send();
            if (selected) {
                check(buffers.Empty() && buffers.Available() == 2 * 1024 * 1024 &&
                    original_buffers == buffer_calls_before, "real release returns pool and zero replaced producers");
                check(title::MusicBuffers::Install(capture, install, nullptr), "detach empty buffer owner");
                check(title::MusicLibrary::Install(capture, install, nullptr), "detach music after file close");
                for (uint32_t channel = 0; channel < 48; ++channel) audio.HandOffVoice(channel);
                check(title::SoundControls::Install(capture, install, nullptr), "detach Audio");
                check(title::SoundBanks::Install(capture, install, nullptr), "detach bank owner");
            } else if (controls) {
                check(title::SoundControls::Reset() && title::SoundControls::Install(capture, install, nullptr),
                      "detach shared-control observation arm");
            }
        }
    }
    LoaderFileIo::ResetState();
    check(StreamLibrary::Scheduling()->SetServiceTime(false), "detach music fixture time");
    std::printf("MusicSelfOracle: checks=%d failures=%d compared_frames=%llu rpc=%u\n",
                checks, failures, static_cast<unsigned long long>(compared_frames), face.Calls());
    std::printf("SharedControlSelfOracle: compared_frames=%llu\n", static_cast<unsigned long long>(shared_frames));
    std::printf("AudioMixerSelfOracle: compared_frames=%llu SpuCore_mix_calls=0\n",
                static_cast<unsigned long long>(host_frames));
    return failures;
}
