#pragma once

#include <cstdint>

namespace title {

// A volume target controlled on the service clock and advanced by output
// samples. State contains gain arithmetic only, without libsd commands,
// register shadows, an IOP callback, or a hardware address.
class VolumeRamp {
public:
    struct State {
        uint16_t target = 0;
        int8_t direction = 0;
        bool active = false, crossing_zero = false;
        float step = 0;
    };
    void Target(uint16_t volume, float current);
    void Control(float& gain);
    void Step(float& gain) const;
    void Reset();
    const State& Capture() const;
    static bool Valid(const State& state);
    bool Restore(const State& state);

private:
    State state_;
};

} // namespace title
