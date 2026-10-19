// platform/nativeee (host side) -- pads.h: Platform::Pads over the engine's title::Input (the game's controller
// contract: libpad's states, request states, modes, actuators, the 32-byte DualShock 2 report), sampled once per frame
// boundary from the host sampler (host/te_input.cpp: the engine's SDL InputSystem -- gamepad + keyboard/mouse -- or
// the neutral pad). The proof stub's shape (feasibility/proof/ps2e_pad_stub.c) with the engine as the body:
// Open stores the game's 256-byte buffer address (the PS2's libpad DMA area; natively only Read's destination is
// written), Read copies the report into the game's `data`.
#include <array>
#include <cstdio>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/input/input.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_services.h"

namespace te {

namespace {
bool Valid(int32_t port, int32_t slot) {
    return port >= 0 && port < int32_t(Pads::kPorts) && slot >= 0 && slot < int32_t(Pads::kSlots);
}
}  // namespace

Pads::Pads(Clock& clock, const Settings& settings) : clock_(clock) {
    core_.reset(new title::Input([this](unsigned port, unsigned slot, title::Input::Motors m) {
        return motors_ ? motors_(port, slot, m.small, m.big) : true;   // no device: accepted, nothing shakes
    }));
    const bool connected = settings.pad_connected;
    sampler_ = [connected](unsigned, unsigned) { Sample s; s.connected = connected; return s; };
    hook_ = clock_.AddBoundaryHook("pads", [this](uint64_t boundary) { SampleBoundary(boundary); });
    // title::Input's snapshot (InputStateCodec) + the game's per-port buffer addresses and the held test buttons.
    savestate::Register(savestate::kTagPads, "te::Pads", false,
        [](std::vector<uint8_t>& out) { Services::Get().pads().Serialize(out); },
        [](const savestate::Blob& blob, std::string* err) { return Services::Get().pads().Restore(blob, err); });
}

void Pads::Serialize(std::vector<uint8_t>& out) const {
    std::vector<uint8_t> input;
    std::string error;
    if (!title::InputStateCodec::Encode(core_->Capture(), input, &error))
        std::fprintf(stderr, "[twinsanityevolved] pads: state not captured: %s\n", error.c_str());
    savestate::Put(out, uint32_t(1));
    for (uint32_t b : buffers_) savestate::Put(out, b);
    savestate::Put(out, test_buttons_);
    savestate::Put(out, uint32_t(input.size()));
    savestate::PutBytes(out, input.data(), input.size());
}

bool Pads::Restore(const savestate::Blob& blob, std::string* err) {
    savestate::PayloadReader in(blob.payload);
    uint32_t version = 0;
    auto fail = [&](const std::string& why) { if (err) *err = "te::Pads: " + why; return false; };
    if (!in.Get(&version) || version != 1) return fail("payload version");
    std::array<uint32_t, kPorts * kSlots> buffers{};
    for (uint32_t& b : buffers) if (!in.Get(&b)) return fail("payload truncated");
    uint32_t test = 0;
    std::vector<uint8_t> input;
    if (!in.Get(&test) || !savestate::GetVector(in, input, 1 << 16)) return fail("payload truncated");
    title::Input::Snapshot snapshot;
    std::string error;
    if (!title::InputStateCodec::Decode(input, snapshot, &error)) return fail(error);
    // The saved clock IS the clock we load under (the clock section restores the same boundary count).
    if (!core_->RestoreSaved(snapshot)) return fail("title::Input rejected the snapshot");
    buffers_ = buffers;
    test_buttons_ = test;
    return true;
}

Pads::~Pads() { clock_.RemoveBoundaryHook(hook_); }

void Pads::SetSampler(Sampler sampler) { if (sampler) sampler_ = std::move(sampler); }

int32_t Pads::Initialise() {
    // padInit(0): 1 when ready. The engine's Input is ready at construction.
    return 1;
}

int32_t Pads::Open(int32_t port, int32_t slot, uint32_t buffer) {
    if (!Valid(port, slot) || buffer == 0) return 0;
    buffers_[unsigned(port) * kSlots + unsigned(slot)] = buffer;
    if (uint8_t* area = GuestPointer(buffer, 256)) std::memset(area, 0, 256);
    return core_->Open(unsigned(port), unsigned(slot));
}

int32_t Pads::Close(int32_t port, int32_t slot) {
    if (!Valid(port, slot)) return 0;
    buffers_[unsigned(port) * kSlots + unsigned(slot)] = 0;
    return core_->Close(unsigned(port), unsigned(slot));
}

int32_t Pads::Read(int32_t port, int32_t slot, uint32_t data) {
    if (!Valid(port, slot)) return 0;
    title::Input::Report report{};
    const int32_t n = core_->Read(unsigned(port), unsigned(slot), report);
    if (n <= 0) return 0;
    uint8_t* out = GuestPointer(data, report.size());
    if (!out) return 0;
    std::memcpy(out, report.data(), report.size());
    return n;
}

int32_t Pads::GetState(int32_t port, int32_t slot) const {
    if (!Valid(port, slot)) return 0;
    return static_cast<int32_t>(core_->GetState(unsigned(port), unsigned(slot)));
}

int32_t Pads::GetRequestState(int32_t port, int32_t slot) const {
    if (!Valid(port, slot)) return 1;   // RequestFailed
    return static_cast<int32_t>(core_->GetRequest(unsigned(port), unsigned(slot)));
}

int32_t Pads::InfoMode(int32_t port, int32_t slot, int32_t info, int32_t index) const {
    if (!Valid(port, slot)) return 0;
    return core_->InfoMode(unsigned(port), unsigned(slot), info, index);
}

int32_t Pads::SetMainMode(int32_t port, int32_t slot, int32_t mode, int32_t lock) {
    if (!Valid(port, slot)) return 0;
    return core_->SetMode(unsigned(port), unsigned(slot), mode, lock);
}

int32_t Pads::InfoActuator(int32_t port, int32_t slot, int32_t actuator, int32_t term) const {
    if (!Valid(port, slot)) return 0;
    return core_->InfoActuator(unsigned(port), unsigned(slot), actuator, term);
}

int32_t Pads::SetActuatorAlign(int32_t port, int32_t slot, uint32_t align) {
    if (!Valid(port, slot)) return 0;
    const uint8_t* p = GuestPointer(align, 6);
    if (!p) return 0;
    std::array<uint8_t, 6> a{};
    std::memcpy(a.data(), p, 6);
    return core_->SetAlignment(unsigned(port), unsigned(slot), a);
}

int32_t Pads::SetActuatorDirect(int32_t port, int32_t slot, uint32_t values) {
    if (!Valid(port, slot)) return 0;
    const uint8_t* p = GuestPointer(values, 6);
    if (!p) return 0;
    std::array<uint8_t, 6> v{};
    std::memcpy(v.data(), p, 6);
    return core_->SetMotors(unsigned(port), unsigned(slot), v);
}

int32_t Pads::InfoPressureMode(int32_t port, int32_t slot) const {
    // padInfoPressMode: a DualShock 2 has the pressure sensitive mode (1) once the pad is up (the engine's
    // InputBindings answered the same; title::Input has EnterPressure but no info query).
    if (!Valid(port, slot)) return 0;
    const auto state = core_->GetState(unsigned(port), unsigned(slot));
    return state == title::Input::State::Stable || state == title::Input::State::Busy ? 1 : 0;
}

int32_t Pads::EnterPressureMode(int32_t port, int32_t slot) {
    if (!Valid(port, slot)) return 0;
    return core_->EnterPressure(unsigned(port), unsigned(slot));
}

void Pads::SampleBoundary(uint64_t boundary) {
    std::array<title::Input::Sample, kPorts * kSlots> samples{};
    for (unsigned port = 0; port < kPorts; ++port) {
        for (unsigned slot = 0; slot < kSlots; ++slot) {
            if (!core_->IsOpen(port, slot)) continue;
            const Sample s = override_ ? override_(port, slot) : sampler_(port, slot);
            if (tap_) tap_(port, slot, s);
            title::Input::Sample& out = samples[port * kSlots + slot];
            out.connected = s.connected;
            out.buttons = s.buttons;
            out.lx = s.lx; out.ly = s.ly; out.rx = s.rx; out.ry = s.ry;
            if (port == 0 && slot == 0 && test_buttons_ != 0) {
                // The game's button word (high byte first in the report) -> wire order (SELECT bit 0 ... SQUARE bit 15)
                const uint32_t t = test_buttons_;
                out.buttons |= static_cast<uint16_t>(((t >> 8) & 0xFF) | ((t & 0xFF) << 8));
            }
        }
    }
    core_->SampleBoundary(boundary, samples);
}

uint32_t Pads::BufferOf(int32_t port, int32_t slot) const {
    return Valid(port, slot) ? buffers_[unsigned(port) * kSlots + unsigned(slot)] : 0;
}

}  // namespace te

extern "C" {

ps2e_s32 ps2e_pads_initialise(void) { return te::Services::Get().pads().Initialise(); }
ps2e_s32 ps2e_pads_open(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 buffer) { return te::Services::Get().pads().Open(port, slot, buffer); }
ps2e_s32 ps2e_pads_close(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().Close(port, slot); }
ps2e_s32 ps2e_pads_read(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 data) { return te::Services::Get().pads().Read(port, slot, data); }
ps2e_s32 ps2e_pads_get_state(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().GetState(port, slot); }
ps2e_s32 ps2e_pads_get_request_state(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().GetRequestState(port, slot); }
ps2e_s32 ps2e_pads_info_mode(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 info, ps2e_s32 index) { return te::Services::Get().pads().InfoMode(port, slot, info, index); }
ps2e_s32 ps2e_pads_set_main_mode(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 mode, ps2e_s32 lock) { return te::Services::Get().pads().SetMainMode(port, slot, mode, lock); }
ps2e_s32 ps2e_pads_info_actuator(ps2e_s32 port, ps2e_s32 slot, ps2e_s32 actuator, ps2e_s32 term) { return te::Services::Get().pads().InfoActuator(port, slot, actuator, term); }
ps2e_s32 ps2e_pads_set_actuator_align(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 align) { return te::Services::Get().pads().SetActuatorAlign(port, slot, align); }
ps2e_s32 ps2e_pads_set_actuator_direct(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 values) { return te::Services::Get().pads().SetActuatorDirect(port, slot, values); }
ps2e_s32 ps2e_pads_info_pressure_mode(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().InfoPressureMode(port, slot); }
ps2e_s32 ps2e_pads_enter_pressure_mode(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().pads().EnterPressureMode(port, slot); }

}  // extern "C"
