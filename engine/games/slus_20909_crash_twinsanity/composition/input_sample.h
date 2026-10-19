#pragma once

#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

class PlatformInput;
class PadProtocol;
class PadStampDuty;
namespace ps2r::triage { class InputLog; }
namespace ps2r::app {
class InputSave;
// One serialized boundary sample. The selected arm never stamps pad areas;
// repeated logical keys retain the accepted sample without consuming edges.
// `tick` is the boundary's logical input tick (the Input/Save services' key);
// `frame` is the boundary's game frame (the frames-lane key). The replay
// script is indexed by whichever its clock names (InputLog::Key).
// `probe` (harness.live_pad_probe): a live PlatformInput read at the boundary
// exactly as `live` would be -- the pad's round trip to its owner, the product
// path -- with its sample discarded: the replay serves. Null = off.
bool SampleInput(InputSave&, PlatformInput*, PadProtocol&, PadStampDuty&,
                 const triage::InputLog&, uint64_t tick, uint32_t frame, ::title::Input::Sample& served,
                 PlatformInput* probe = nullptr);
}
