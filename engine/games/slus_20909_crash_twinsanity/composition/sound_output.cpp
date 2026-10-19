#include "games/slus_20909_crash_twinsanity/composition/sound_output.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/sound/mixer.h"
#include "engine/sound/spu_core.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace ps2r::app {
namespace {
const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
const auto install = [](uint32_t address, DispatchTable::Body body) {
    return DispatchTable::Install(address, std::move(body));
};
}

SoundOutput::SoundOutput(bool samples, bool music, bool output)
    : selected_(samples || music || output), music_(music), mix_(output) {
    // Title InitSpu intent: both output groups start at the same fixed gain.
    // Game group/channel volumes remain in SoundControls.
    constexpr float gain = 16383.0f / 32767.0f;
    for (uint32_t group = 0; group < sound::AudioMixer::kGroups; ++group)
        output_.SetGroup(group, gain, gain);
}

SoundOutput::~SoundOutput() {
    if (!attached_) return;
    if (!title::SoundControls::Reset()) std::terminate();
    if (music_) title::MusicLibrary::Reset();
    StreamLibrary::Reset();
    title::SoundBanks::Reset();
    if (music_ && !title::MusicLibrary::Install(capture, install, nullptr)) std::terminate();
    if (!title::SoundControls::Install(capture, install, nullptr) ||
        !title::SoundBanks::Install(capture, install, nullptr)) std::terminate();
}

bool SoundOutput::Install(title::SoundControls::RemainingVolume remaining_volume) {
    if (!selected_) { installed_ = true; return true; }
    if (mix_ && !music_) return false;
    if (attached_ || !remaining_volume || !StreamLibrary::Enabled()) return false;
    if (!title::SoundBanks::Install(capture, install, &audio_)) return false;
    attached_ = true;
    installed_ = title::SoundControls::Install(capture, install, &audio_, std::move(remaining_volume));
    if (installed_ && music_)
        installed_ = title::MusicLibrary::Install(capture, install, &audio_);
    return installed_;
}

bool SoundOutput::AdvanceBoundary(uint64_t tick) {
    if (installed_ && music_) title::MusicLibrary::AdvanceBoundary(tick);
    return !selected_ || (installed_ && title::SoundControls::AdvanceBoundary(tick));
}

void SoundOutput::BindMixer(sound::Mixer& mixer) {
    mixer_ = &mixer;
    if (mix_) mixer.SetMixSource(&output_);
    else if (selected_) mixer.SetVoiceSource(this);
}

void SoundOutput::BindMovie(sound::PcmSource* source) {
    if (!mixer_) throw std::logic_error("Movie PCM sink requires the bound Mixer");
    // Exactly one consumer: the owned output when selected (the common Mixer
    // then never pumps the retained core), otherwise the common Mixer's own
    // PcmSource seat. The Movie owner keeps gain and FIFO lifetime.
    if (mix_) output_.SetMovieSource(source);
    else mixer_->SetPcmSource(source);
}

uint64_t SoundOutput::MovieFrames(const sound::SpuCore& comparison) const {
    return mix_ ? output_.Count().movie_frames : comparison.FmvFramesMixed();
}

uint64_t SoundOutput::AudibleMovieFrames(const sound::SpuCore& comparison) const {
    return mix_ ? output_.Count().audible_movie_frames : comparison.FmvFramesMixedAudible();
}

void SoundOutput::BeginMixTick() {
    audio_.BeginMixTick();
    ++mix_ticks_;
}

bool SoundOutput::StepVoice(uint32_t channel, float& left, float& right) {
    if (!audio_.StepVoice(channel, left, right)) return false;
    ++owned_samples_;
    const bool nonzero = left != 0.0f || right != 0.0f;
    if (audio_.IsMusicVoice(channel)) { ++music_samples_; if (nonzero) ++music_nonzero_; }
    else { ++sample_samples_; if (nonzero) ++sample_nonzero_; }
    return true;
}

void SoundOutput::Record(verify::RunMeta& meta, const sound::SpuCore& comparison) const {
    const auto& output = output_.Count();
    meta.Set("audio_mix", std::string("selected=") + (mix_ ? "on" : "off") +
        ",ticks=" + std::to_string(output.ticks) + ",frames=" + std::to_string(output.frames) +
        ",owned_channel_samples=" + std::to_string(output.channel_samples) +
        ",unowned_channel_samples=" + std::to_string(output.unowned_samples) +
        ",movie_frames=" + std::to_string(output.movie_frames) +
        ",spu_mix_calls=" + std::to_string(comparison.MixCalls()));
    meta.Set("sound_samples_binding", selected_ ? "on" : "off");
    meta.Set("music_binding", music_ ? "on" : "off");
    meta.Set("sound_output", "installed=" + std::to_string(installed_) +
             ",mix_ticks=" + std::to_string(mix_ticks_) +
             ",owned_channel_samples=" + std::to_string(owned_samples_) +
             ",sample_channel_samples=" + std::to_string(sample_samples_) +
             ",sample_nonzero_samples=" + std::to_string(sample_nonzero_) +
             ",music_channel_samples=" + std::to_string(music_samples_) +
             ",music_nonzero_samples=" + std::to_string(music_nonzero_));
    const auto bank = title::SoundBanks::Count();
    meta.Set("sound_banks", "issued=" + std::to_string(bank.issued) +
             ",delivered=" + std::to_string(bank.delivered) +
             ",cancelled=" + std::to_string(bank.cancelled) +
             ",stale=" + std::to_string(bank.stale) +
             ",reserved=" + std::to_string(bank.reserved) +
             ",freed=" + std::to_string(bank.freed) +
             ",acknowledged=" + std::to_string(bank.acknowledged) +
             ",errors=" + std::to_string(bank.errors) +
             ",fallbacks=" + std::to_string(bank.fallbacks));
    const auto controls = title::SoundControls::Count();
    meta.Set("sound_controls", "staged=" + std::to_string(controls.staged) +
             ",applied=" + std::to_string(controls.applied) +
             ",plays=" + std::to_string(controls.plays) +
             ",volume=" + std::to_string(controls.volume) +
             ",pitch=" + std::to_string(controls.pitch) +
             ",release=" + std::to_string(controls.release) +
             ",key_off=" + std::to_string(controls.key_off) +
             ",batches=" + std::to_string(controls.batches) +
             ",busy=" + std::to_string(controls.busy) +
             ",replies=" + std::to_string(controls.replies) +
             ",handoffs=" + std::to_string(controls.handoffs) +
             ",shared=" + std::to_string(controls.shared) +
             ",remaining_volumes=" + std::to_string(controls.remaining_volumes) +
             ",fallback=" + std::to_string(controls.fallback) +
             ",errors=" + std::to_string(controls.errors) +
             ",boundaries=" + std::to_string(controls.boundaries));
    std::string head;
    for (const auto& play : title::SoundControls::PlayHead()) {
        if (!head.empty()) head += ";";
        head += "b" + std::to_string(play.boundary) + ":sound=" + std::to_string(play.sound) +
                ",ch=" + std::to_string(play.channel) + ",grp=" + std::to_string(play.group) +
                ",raw=" + std::to_string(play.raw_left) + "/" + std::to_string(play.raw_right) +
                ",lvl=" + std::to_string(play.left) + "/" + std::to_string(play.right) +
                ",pitch=" + std::to_string(play.pitch) + ",atk=" + std::to_string(play.attack) +
                ",rel=" + std::to_string(play.release) + ",loops=" + std::to_string(play.loops) +
                ",bytes=" + std::to_string(play.bytes) + ",started=" + std::to_string(play.started);
    }
    meta.Set("sound_plays_head", head.empty() ? "none" : head);
    const auto music = title::MusicLibrary::Count();
    meta.Set("music_library", "windows=" + std::to_string(music.windows) +
             ",plays=" + std::to_string(music.plays) +
             ",children=" + std::to_string(music.children) +
             ",interleaves=" + std::to_string(music.interleaves) +
             ",ends=" + std::to_string(music.ends) +
             ",prepares=" + std::to_string(music.prepares) +
             ",starts=" + std::to_string(music.starts) +
             ",stops=" + std::to_string(music.stops) +
             ",reads=" + std::to_string(music.reads) +
             ",delivered=" + std::to_string(music.delivered) +
             ",bytes=" + std::to_string(music.bytes) +
             ",stale=" + std::to_string(music.stale) +
             ",errors=" + std::to_string(music.errors) +
             ",refused_window=" + std::to_string(music.refused_window) +
             ",refused_buffers=" + std::to_string(music.refused_buffers) +
             ",refused_voice=" + std::to_string(music.refused_voice) +
             ",fallback=" + std::to_string(music.fallback) +
             ",statuses=" + std::to_string(music.statuses) +
             ",fills=" + std::to_string(music.fills) +
             ",refills=" + std::to_string(music.refills) +
             ",refill_bytes=" + std::to_string(music.refill_bytes) +
             ",first_play_tick=" + std::to_string(music.first_play_tick) +
             ",first_start_tick=" + std::to_string(music.first_start_tick));
}

}  // namespace ps2r::app
