#include "games/slus_20909_crash_twinsanity/hle/audio/sample_voice.h"

#include <cmath>
#include <utility>

namespace title {
namespace {
float Ramp(uint8_t rate, float divisor) {
    return rate == 0 ? 1.0f : 1.0f / (4.0f * std::pow(2.0f, static_cast<float>(rate) / divisor));
}
}

float SampleVoice::Volume(uint16_t value) {
    // Source audio.h: bit 14 selects inverted phase; the payload measures from
    // the other endpoint. The mixer oracle's ordinary positive-volume arm is
    // unchanged; signed phase is a title intent not an unsigned host gain.
    const int32_t level = value & 0x7fff;
    const int32_t signed_level = level > 0x3fff ? level - 0x7fff : level;
    return static_cast<float>(signed_level) / 32767.0f;
}

bool SampleVoice::Start(Samples samples, Parameters parameters) {
    if (!samples || samples->empty() || samples->size() % VagDecoder::kBlockBytes != 0 ||
        samples->size() > UINT32_MAX || parameters.attack > 127 ||
        parameters.decay > 15 || parameters.sustain > 15 || parameters.release > 31 ||
        (parameters.left & 0x8000) || (parameters.right & 0x8000)) return false;
    state_ = {};
    state_.samples = std::move(samples);
    state_.parameters = parameters;
    state_.phase = Phase::Attack;
    state_.left = Volume(parameters.left);
    state_.right = Volume(parameters.right);
    // Decode at start, before later controls can change a loop/cursor. This
    // eliminates OLD SOUND_P9's delayed-first-block overwrite by construction.
    Decode();
    return true;
}

void SampleVoice::SetPitch(uint16_t pitch) { state_.parameters.pitch = pitch; }
void SampleVoice::SetVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    state_.left = Volume(left);
    state_.right = Volume(right);
    state_.left_ramp.Reset();
    state_.right_ramp.Reset();
}
void SampleVoice::RampVolumes(uint16_t left, uint16_t right) {
    state_.parameters.left = left;
    state_.parameters.right = right;
    state_.left_ramp.Target(left, state_.left);
    state_.right_ramp.Target(right, state_.right);
}
void SampleVoice::AdvanceControls() {
    state_.left_ramp.Control(state_.left);
    state_.right_ramp.Control(state_.right);
}
void SampleVoice::SetRelease(uint8_t rate) {
    if (rate <= 31) state_.parameters.release = rate;
}
void SampleVoice::Release(uint8_t rate) {
    if (rate > 31 || !Playing()) return;
    SetRelease(rate);
    state_.phase = Phase::Release;
}
void SampleVoice::Stop() {
    state_.phase = Phase::Off;
    state_.envelope = 0;
    state_.samples.reset();
    state_.sample_index = state_.fraction = 0;
}
bool SampleVoice::Playing() const { return state_.phase != Phase::Off; }
uint64_t SampleVoice::Ends() const { return state_.ends; }
const SampleVoice::State& SampleVoice::Capture() const { return state_; }

bool SampleVoice::Valid(const State& s) {
    if (s.phase > Phase::Release || !std::isfinite(s.envelope) ||
        s.envelope < 0 || s.envelope > 1 || !std::isfinite(s.left) ||
        !std::isfinite(s.right) || s.sample_index >= VagDecoder::kBlockSamples ||
        s.fraction >= 0x1000 || s.parameters.attack > 127 || s.parameters.release > 31 ||
        s.parameters.decay > 15 || s.parameters.sustain > 15) return false;
    if (s.history.h1 < -32768 || s.history.h1 > 32767 ||
        s.history.h2 < -32768 || s.history.h2 > 32767 ||
        (s.parameters.left & 0x8000) || (s.parameters.right & 0x8000)) return false;
    if (!VolumeRamp::Valid(s.left_ramp.Capture()) || !VolumeRamp::Valid(s.right_ramp.Capture())) return false;
    if (s.phase == Phase::Off) return true;
    if (!s.samples || s.samples->empty() || s.samples->size() % VagDecoder::kBlockBytes ||
        s.samples->size() > UINT32_MAX || s.block_offset % VagDecoder::kBlockBytes ||
        s.loop_offset % VagDecoder::kBlockBytes ||
        s.block_offset >= s.samples->size() || s.loop_offset >= s.samples->size()) return false;
    return true;
}
bool SampleVoice::Restore(const State& state) {
    if (!Valid(state)) return false;
    state_ = state;
    return true;
}

void SampleVoice::Decode() {
    state_.flags = VagDecoder::DecodeBlock(state_.samples->data() + state_.block_offset,
                                          state_.history, state_.block.data());
    state_.sample_index = 0;
    if (state_.flags & VagDecoder::kFlagLoopStart) state_.loop_offset = state_.block_offset;
}

void SampleVoice::Envelope() {
    const auto& p = state_.parameters;
    const float sustain = static_cast<float>(p.sustain) / 15.0f;
    switch (state_.phase) {
    case Phase::Attack:
        state_.envelope += Ramp(p.attack, 4.0f);
        if (state_.envelope >= 1.0f) {
            state_.envelope = 1.0f;
            state_.phase = sustain >= 1.0f ? Phase::Sustain : Phase::Decay;
        }
        break;
    case Phase::Decay:
        state_.envelope -= Ramp(p.decay, 1.0f);
        if (state_.envelope <= sustain) {
            state_.envelope = sustain;
            state_.phase = Phase::Sustain;
        }
        break;
    case Phase::Release:
        state_.envelope -= p.release == 0 ? 0.25f : Ramp(p.release, 2.0f);
        if (state_.envelope <= 0) Stop();
        break;
    case Phase::Sustain:
    case Phase::Off: break;
    }
}

void SampleVoice::Step(float& left, float& right) {
    left = right = 0;
    if (!Playing()) return;
    state_.fraction += state_.parameters.pitch;
    while (state_.fraction >= 0x1000) {
        state_.fraction -= 0x1000;
        state_.previous = state_.block[state_.sample_index++];
        if (state_.sample_index < VagDecoder::kBlockSamples) continue;
        if (state_.flags & VagDecoder::kFlagLoopEnd) {
            ++state_.ends;
            if (!(state_.flags & VagDecoder::kFlagLoopRepeat)) {
                Stop();
                return;
            }
            state_.block_offset = state_.loop_offset;
        } else {
            state_.block_offset += VagDecoder::kBlockBytes;
            if (state_.block_offset >= state_.samples->size()) {
                Stop(); // an unterminated asset ends at its owned extent
                return;
            }
        }
        Decode();
    }
    const int32_t current = state_.block[state_.sample_index];
    const int32_t sample = state_.previous +
        (((current - state_.previous) * static_cast<int32_t>(state_.fraction)) >> 12);
    Envelope();
    state_.left_ramp.Step(state_.left);
    state_.right_ramp.Step(state_.right);
    const float value = static_cast<float>(sample) / 32768.0f * state_.envelope;
    left = value * state_.left;
    right = value * state_.right;
}
void SampleVoice::StepPaused() {
    if (Playing()) Envelope();
}
} // namespace title
