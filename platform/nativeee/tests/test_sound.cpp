// test_sound.cpp -- sound.h's one function: Platform::Sound::InitialiseRemote connects to the sound driver = the
// audio owner opens its output device when configured to. The tests never open a device (no SDL audio in CI):
// the call must still report success (0, the SDRDRV init result the game checks) and be repeatable.
#include "audio_service.h"
#include "ps2e_seam.h"
#include "te_test.h"

int main() {
    te_test::Reset(te_test::DefaultSettings());
    auto& a = te::audio::Service();
    a.Configure(false);
    CHECK_EQ(a.InitialiseRemote(), 0);
    CHECK(!a.DeviceOpen());
    CHECK_EQ(ps2e_sound_initialise_remote(), 0);
    CHECK_EQ(ps2e_sound_initialise_remote(), 0);   // the game may re-initialise after a reset
    CHECK(!a.DeviceOpen());
    a.CloseDevice();                                 // harmless without a device
    CHECK(!a.DeviceOpen());
    // The mix runs without a device: a boundary is one tick, nothing reaches the (closed) ring.
    a.OnBoundary();
    CHECK_EQ(a.Count().ticks, 1u);
    CHECK_EQ(a.Count().ring_pushed, 0u);
    return te_test::Finish("test_sound");
}
