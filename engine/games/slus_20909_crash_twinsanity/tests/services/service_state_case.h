#pragma once

#include <cstdint>

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

namespace title { class Audio; }
namespace ps2r::sound { class AudioMixer; }

// Called at real source music boundaries by the original-body music oracle.
int RunServiceStateCase(title::Audio& audio, uint64_t& tick, bool playing,
                        ps2r::sound::AudioMixer* output = nullptr);
int RunServiceLoanStateCase(title::Audio& audio, title::StreamBuffers::Loan& sound,
                           title::StreamBuffers::Loan& disc);
