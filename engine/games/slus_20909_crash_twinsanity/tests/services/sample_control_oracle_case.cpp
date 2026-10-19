#include "games/slus_20909_crash_twinsanity/tests/services/sample_control_oracle_case.h"
#include "games/slus_20909_crash_twinsanity/tests/services/sample_output.h"
#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_helpers.h"

#include <cstdio>
#include <vector>
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/sound/mixer.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"

using namespace stream_oracle;
using namespace ps2r::iop;
using ps2r::title::stream_irx::ModuleLayout;
using ps2r::title::stream_irx::StreamIrxPlugin;
static uint64_t g_original_sample_controls = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

int RunSampleControlOracle(CpuContext& ctx, ps2r::sound::SpuCore& spu,
    title::Audio& audio, uint32_t sample) {
    int g_fail = 0;
    const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
    const auto install = [](uint32_t address, StreamLibrary::Body body) {
        return DispatchTable::Install(address, std::move(body));
    };
    std::vector<float> model_pcm, model_paused_pcm;
    SampleOutput sample_output(audio);
    for (bool selected : {false, true}) {
        const auto control_capture = [](uint32_t address) {
            const auto original = DispatchTable::Capture(address);
            return StreamLibrary::Body{[address, original](CpuContext& call, uint32_t entry) {
                if (address == 0x1e18e8 || address == 0x1e3e80 || address == 0x1e6b50 ||
                    address == 0x1e4038 || address == 0x1e6d30) ++g_original_sample_controls;
                original(call, entry);
            }};
        };
        CHECK(title::SoundControls::Install(control_capture, install, selected ? &audio : nullptr,
            [&spu](uint32_t channel, uint16_t left, uint16_t right) {
                const uint32_t entry = channel / 24 | (channel % 24) << 1;
                spu.SetParam(entry, left); spu.SetParam(entry | 0x100, right);
            }), "controls_selects_same_input_arm");
        spu.ApplyQueued();
        for (int vi = 0; vi < 48; ++vi) spu.VoiceAt(vi).Silence();
        spu.SetMasterVolume(1);
        for (int core = 0; core < 2; ++core) { spu.SetMvolL(core, 1); spu.SetMvolR(core, 1); }
        spu.RepublishReadbacks();
        // Fresh source client channel state for each arm.
        memory::write<uint8_t>(0x2e79a8, 0);
        auto control = [&](uint32_t address, std::initializer_list<uint32_t> arguments) {
            Prep(ctx);
            unsigned reg = 4;
            for (uint32_t argument : arguments) SetArg(ctx, reg++, argument);
            DispatchTable::Invoke(address, ctx);
            return ctx.cpuRegs.GPR.r[2].SL[0];
        };
        CHECK(control(0x1e7cb8, {0x10000, 0x1000, 0x1000}) == 0, "controls_group_full");
        CHECK(control(0x1e2958, {0}) >= 0, "controls_group_commit");
        const auto records_before = memory::read<uint32_t>(kBatchRecords);
        const auto kon_before = spu.KonCount();
        const auto original_calls = g_original_sample_controls;
        CHECK(control(0x1e18e8, {sample, 0x10000, 12000, 10000, 0x1000, 0, 13, 1}) == 0,
              "controls_stages_sample_play");
        CHECK(control(0x1e8128, {0}) == 0, "controls_requested_channel_is_not_free");
        if (selected) {
            CHECK(!audio.OwnsVoice(0) && memory::read<uint32_t>(kBatchRecords) == records_before,
                  "controls_lease_staged_without_packet_or_early_playback");
            CHECK(!title::SoundControls::CanCheckpoint(), "controls_guard_requires_composed_snapshot");
        }
        CHECK(control(0x1e2958, {0}) >= 0, "controls_play_accepted_at_send");
        spu.ApplyQueued();
        std::vector<float> output;
        ps2r::sound::Mixer mixer(spu);
        mixer.SetVoiceSource(selected ? &sample_output : nullptr);
        for (uint32_t pulse = 0; pulse < 24; ++pulse) {
            if (pulse == 1) CHECK(control(0x1e3e80, {0, 2300, 13000}) == 0, "controls_volume_target");
            if (pulse == 2) CHECK(control(0x1e6b50, {0, 0xaaa}) == 0, "controls_pitch_change");
            if (pulse == 3) CHECK(control(0x1e7cb8, {0x10000, 0x800, 0xc00}) == 0, "controls_group_immediate");
            if (pulse == 4) CHECK(control(0x1e4038, {0, 7}) == 0, "controls_release_rate_only");
            if (pulse == 5) CHECK(control(0x1e3e80, {0, 12000, 5000}) == 0, "controls_new_ramp");
            if (pulse == 6) control(0x1e6dd0, {});
            if (pulse == 8) control(0x1e6e18, {});
            if (pulse == 11) CHECK(control(0x1e6d30, {0}) == 0, "controls_explicit_key_off");
            CHECK(control(0x1e2958, {0}) >= 0, "controls_batch_accepted");
            if (selected) {
                CHECK(title::SoundControls::AdvanceBoundary(pulse) &&
                          title::SoundControls::AdvanceBoundary(pulse),
                      "controls_boundary_pulse_is_idempotent");
            } else {
                auto iop_ctx = IopContext::ForEntry(0x180000, ModuleLayout::kGp);
                StreamIrxPlugin::Resolve(0x10ab8)(iop_ctx);
            }
            mixer.Pump();
            output.insert(output.end(), mixer.Output(),
                          mixer.Output() + mixer.OutputFrames() * 2);
        }
        if (!selected) {
            model_pcm = std::move(output);
            CHECK(spu.KonCount() > kon_before && g_original_sample_controls > original_calls,
                  "controls_model_arm_really_executes_sample_commands_and_keys_retained_voice");
        } else {
            size_t different = 0;
            for (size_t sample = 0; sample < output.size(); ++sample) if (output[sample] != model_pcm[sample]) {
                if (different++ < 3)
                    std::printf("CONTROL_DIFF sample=%zu model=%a owned=%a\n",
                                sample, model_pcm[sample], output[sample]);
            }
            CHECK(different == 0, "controls_owned_pcm_equals_original_command_pipeline");
            CHECK(spu.KonCount() == kon_before && audio.IsVoiceFree(0) == 1,
                  "controls_owned_voice_retires_without_retained_key_on");
            CHECK(g_original_sample_controls == original_calls,
                  "controls_owned_arm_executes_zero_replaced_sample_service_bodies");
            std::printf("SoundControlSelfOracle: stereo_frames=%zu mismatched_samples=%zu\n",
                        output.size() / 2, different);
            audio.HandOffVoice(0);
        }
        // The title's menu: StopChunks -> PauseGame -> PauseSound (MsPauseAll)
        // on the title->menu transition, then MenuSounds::Play for every
        // navigation press WHILE paused (TR game/context.cpp BeginFrame,
        // game/menus.cpp). The module's pause holds the voices it finds; a
        // sample keyed on afterwards plays. Same inputs through both arms.
        {
            const auto kon_before_paused = spu.KonCount();
            control(0x1e6dd0, {});
            CHECK(control(0x1e2958, {0}) >= 0, "controls_pause_accepted");
            CHECK(control(0x1e18e8, {sample, 0x40001, 16383, 16383, 1881, 32, 13, 1}) == 0,
                  "controls_stages_play_while_paused");
            CHECK(control(0x1e2958, {0}) >= 0, "controls_paused_play_accepted_at_send");
            spu.ApplyQueued();
            std::vector<float> paused_output;
            ps2r::sound::Mixer paused_mixer(spu);
            paused_mixer.SetVoiceSource(selected ? &sample_output : nullptr);
            for (uint32_t pulse = 0; pulse < 16; ++pulse) {
                if (pulse == 10) control(0x1e6e18, {});
                CHECK(control(0x1e2958, {0}) >= 0, "controls_paused_batch_accepted");
                if (selected) {
                    title::SoundControls::AdvanceBoundary(100 + pulse);
                } else {
                    auto iop_ctx = IopContext::ForEntry(0x180000, ModuleLayout::kGp);
                    StreamIrxPlugin::Resolve(0x10ab8)(iop_ctx);
                }
                paused_mixer.Pump();
                paused_output.insert(paused_output.end(), paused_mixer.Output(),
                                     paused_mixer.Output() + paused_mixer.OutputFrames() * 2);
            }
            const size_t before_resume = static_cast<size_t>(paused_mixer.OutputFrames()) * 2 * 10;
            size_t nonzero_paused = 0;
            for (size_t i = 0; i < before_resume && i < paused_output.size(); ++i)
                if (paused_output[i] != 0) ++nonzero_paused;
            if (!selected) {
                CHECK(spu.KonCount() > kon_before_paused, "controls_model_keys_the_play_issued_while_paused");
                CHECK(nonzero_paused > 0, "controls_model_play_while_paused_is_audible");
                model_paused_pcm = std::move(paused_output);
            } else {
                size_t different = 0;
                for (size_t i = 0; i < paused_output.size(); ++i) if (paused_output[i] != model_paused_pcm[i]) {
                    if (different++ < 3)
                        std::printf("PAUSED_PLAY_DIFF sample=%zu model=%a owned=%a\n",
                                    i, model_paused_pcm[i], paused_output[i]);
                }
                CHECK(nonzero_paused > 0, "controls_owned_play_while_paused_is_audible");
                CHECK(different == 0, "controls_owned_paused_play_pcm_equals_original_pipeline");
                std::printf("SoundControlPausedPlayOracle: stereo_frames=%zu mismatched_samples=%zu nonzero_before_resume=%zu\n",
                            paused_output.size() / 2, different, nonzero_paused);
                CHECK(control(0x1e6d30, {1}) == 0 && control(0x1e2958, {0}) >= 0, "controls_paused_play_key_off");
                for (uint32_t pulse = 0; pulse < 64 && !audio.IsVoiceFree(1); ++pulse) {
                    title::SoundControls::AdvanceBoundary(200 + pulse);
                    paused_mixer.Pump();
                }
                audio.HandOffVoice(1);
            }
        }
        if (selected) {
            CHECK(title::SoundControls::Reset() &&
                      title::SoundControls::Install(capture, install, nullptr),
                  "controls_detaches_after_owner_reset");
        }
    }
    return g_fail;
}
