#include "games/slus_20909_crash_twinsanity/composition/input_sample.h"

#include "games/slus_20909_crash_twinsanity/composition/input_save.h"
#include "app/host/platform_input.h"
#include "engine/core/triage/input_log.h"
#include "engine/pad/hle/pad_stamp_duty.h"

namespace ps2r::app {
bool SampleInput(InputSave& services, PlatformInput* live, PadProtocol& protocol, PadStampDuty& stamp,
                 const triage::InputLog& replay, uint64_t tick, uint32_t frame, ::title::Input::Sample& served,
                 PlatformInput* probe) {
    PadProtocol::Input sample;
    if (probe && probe != live) (void)probe->Sample();   // the live read's round trip; the replay below serves
    if (services.InputSelected() && !services.NeedsSample(tick)) {
        if (tick < services.Current().boundary) return false;
        served = services.Current().sample;
        return true;
    }
    const uint32_t script_key = replay.Key(tick, frame);
    if (live) {
        sample = live->Sample();
        if (!services.InputSelected()) protocol.Stamp(sample, tick);
    } else if (services.InputSelected()) {
        uint16_t wire = 0xffff;
        uint8_t axes[4] = {128, 128, 128, 128};
        replay.Apply(script_key, &wire, axes);
        sample = {static_cast<uint16_t>(~wire), axes[0], axes[1], axes[2], axes[3]};
    } else {
        sample = stamp.Run(tick, script_key);
    }
    served = {};
    served.connected = live ? live->Connected() : true;
    served.buttons = sample.buttons; served.lx = sample.lx; served.ly = sample.ly;
    served.rx = sample.rx; served.ry = sample.ry;
    if (services.InputSelected()) {
        if (!services.Sample(tick, served)) return false;
        served = services.Current().sample;
    } else {
        served.buttons = static_cast<uint16_t>(~protocol.LastWire());
    }
    return true;
}
}
