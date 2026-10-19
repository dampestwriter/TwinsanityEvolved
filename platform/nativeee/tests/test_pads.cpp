// test_pads.cpp -- pads.h over title::Input: the proof stub's 33-check shape plus the game's
// DualShock 2 setup sequence (game/src/game/pads.cpp UpdatePad: InfoMode -> actuators -> align -> pressure) run
// against the façade with boundaries in between, and the report bytes the game reads (ReportByte).
#include "ps2e_seam.h"
#include "te_test.h"

namespace {
constexpr uint32_t kBuffer = 0x2000, kData = 0x3000, kAlign = 0x4000;
// Platform::Pads values
constexpr int32_t StateDisconnected = 0, StateStable = 6, RequestComplete = 0, RequestBusy = 2;
constexpr int32_t InfoCurrentId = 1, InfoCurrentExtendedId = 2, InfoModeTable = 4, ModeTableLength = -1;
constexpr int32_t ModeIdAnalog = 7, MainModeAnalog = 1, ModeLocked = 3, CountActuators = -1;
// ReportByte
constexpr uint32_t ReportMode = 1, ReportButtonsHigh = 2, ReportButtonsLow = 3, StickRightX = 4, StickLeftY = 7;
constexpr uint32_t PressureCross = 14, PressureUp = 10;
}  // namespace

int main() {
    using namespace te_test;
    Reset(DefaultSettings());
    te::Services& services = te::Services::Get();
    te::Clock& clock = services.clock();
    te::Pads& pads = services.pads();

    // A scripted sampler: connected, cross + up held, left stick right, right stick up (the proof's fixed sample)
    te::Pads::Sample sample;
    sample.connected = true;
    sample.buttons = (1u << 14) | (1u << 4);   // CROSS (bit 14) + UP (bit 4), wire order active high
    sample.lx = 0xFF; sample.ly = 0x80; sample.rx = 0x80; sample.ry = 0x00;
    pads.SetSampler([&](unsigned port, unsigned slot) { return port == 0 && slot == 0 ? sample : te::Pads::Sample{}; });

    CHECK_EQ(ps2e_pads_initialise(), 1);
    CHECK_EQ(ps2e_pads_open(0, 0, 0), 0);           // no buffer: refused
    CHECK_EQ(ps2e_pads_open(5, 0, kBuffer), 0);     // no such port
    CHECK_EQ(ps2e_pads_open(0, 0, kBuffer), 1);     // port*2+slot+1
    CHECK_EQ(pads.BufferOf(0, 0), kBuffer);
    // Before a boundary: nothing sampled yet, disconnected, no report
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateDisconnected);
    CHECK_EQ(ps2e_pads_read(0, 0, kData), 0);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoCurrentId, 0), 0);

    // The first boundary samples the pad: stable, analog DualShock 2
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateStable);
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestComplete);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoCurrentId, 0), ModeIdAnalog);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoCurrentExtendedId, 0), ModeIdAnalog);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoModeTable, ModeTableLength), 2);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoModeTable, 0), 4);
    CHECK_EQ(ps2e_pads_info_mode(0, 0, InfoModeTable, 1), 7);
    CHECK_EQ(ps2e_pads_info_actuator(0, 0, CountActuators, 0), 2);
    CHECK_EQ(ps2e_pads_info_pressure_mode(0, 0), 1);

    // The game's setup: SetMainMode(analog, locked) -> busy one boundary -> complete
    CHECK_EQ(ps2e_pads_set_main_mode(0, 0, MainModeAnalog, ModeLocked), 1);
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestBusy);
    CHECK_EQ(ps2e_pads_set_main_mode(0, 0, MainModeAnalog, ModeLocked), 0);   // one command at a time
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestComplete);
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateStable);
    // Actuator alignment {0, 1, 255, 255, 255, 255}
    const uint8_t align[6] = {0, 1, 255, 255, 255, 255};
    std::memcpy(te::GuestPointer(kAlign, 6), align, 6);
    CHECK_EQ(ps2e_pads_set_actuator_align(0, 0, kAlign), 1);
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestBusy);
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestComplete);
    // Pressure mode
    CHECK_EQ(ps2e_pads_enter_pressure_mode(0, 0), 1);
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_request_state(0, 0), RequestComplete);

    // The report: 32 bytes into the game's data; mode 0x79 (analog + pressure); buttons active low, high byte first
    CHECK_EQ(ps2e_pads_read(0, 0, kData), 32);
    const uint8_t* r = te::GuestPointer(kData, 32);
    CHECK_EQ(r[ReportMode], 0x79);
    const uint32_t word = (uint32_t(r[ReportButtonsHigh]) << 8) | r[ReportButtonsLow];   // the game's button word
    // The game's word: 0x1000 up, 0x40 cross (pressed 0): both bits clear, everything else set
    CHECK_EQ(word & 0x1000u, 0u);
    CHECK_EQ(word & 0x40u, 0u);
    CHECK_EQ(word | 0x1040u, 0xFFFFu);
    CHECK_EQ(r[StickLeftY], 0x80);
    CHECK_EQ(te::GuestPointer(kData, 32)[6], 0xFF);   // StickLeftX
    CHECK_EQ(r[StickRightX], 0x80);
    CHECK_EQ(r[5], 0x00);                               // StickRightY
    CHECK_EQ(r[PressureCross], 0xFF);
    CHECK_EQ(r[PressureUp], 0xFF);
    CHECK_EQ(r[13], 0x00);                              // PressureCircle: not pressed
    // Read into unmapped memory: 0, nothing written
    CHECK_EQ(ps2e_pads_read(0, 0, 0x7FFFFFF0u), 0);

    // Test buttons (the decomp's g_TestPadButtons word): 0x800 start -> report byte 2 bit 3 clear
    pads.HoldTestButtons(0x800);
    clock.Boundary();
    ps2e_pads_read(0, 0, kData);
    CHECK_EQ(r[ReportButtonsHigh] & 0x08u, 0u);
    CHECK_EQ(r[ReportButtonsLow] & 0x40u, 0u);   // cross still held by the sampler
    pads.HoldTestButtons(0);

    // Rumble: SetActuatorDirect reaches the motor output with the aligned bytes
    uint8_t small = 9, big = 9; unsigned calls = 0;
    pads.SetMotorOutput([&](unsigned, unsigned, uint8_t s, uint8_t b) { small = s; big = b; ++calls; return true; });
    const uint8_t values[6] = {1, 200, 0, 0, 0, 0};
    std::memcpy(te::GuestPointer(kAlign, 6), values, 6);
    CHECK_EQ(ps2e_pads_set_actuator_direct(0, 0, kAlign), 1);
    CHECK_EQ(calls, 1u); CHECK_EQ(small, 1); CHECK_EQ(big, 200);

    // Unplug: the sampler says disconnected -> state 0, request failed, no report; replug -> stable again
    sample.connected = false;
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateDisconnected);
    CHECK_EQ(ps2e_pads_read(0, 0, kData), 0);
    sample.connected = true;
    clock.Boundary();
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateStable);

    // Close
    CHECK_EQ(ps2e_pads_close(0, 0), 1);
    CHECK_EQ(ps2e_pads_get_state(0, 0), StateDisconnected);
    CHECK_EQ(ps2e_pads_close(0, 0), 0);

    // The neutral default sampler (no SDL): connected, nothing pressed, 0x79 once set up
    Reset(DefaultSettings());
    {
        te::Clock& c = te::Services::Get().clock();
        CHECK_EQ(ps2e_pads_open(0, 0, kBuffer), 1);
        c.Boundary();
        CHECK_EQ(ps2e_pads_get_state(0, 0), StateStable);
        CHECK_EQ(ps2e_pads_read(0, 0, kData), 32);
        const uint8_t* n = te::GuestPointer(kData, 32);
        CHECK_EQ(n[ReportButtonsHigh], 0xFF); CHECK_EQ(n[ReportButtonsLow], 0xFF);
        CHECK_EQ(n[ReportMode], 0x73);   // analog, pressure not entered yet
    }
    te::Services::ResetForTest();
    return Finish("test_pads");
}
