#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"

#include <utility>

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/input/snapshot_bytes.h"

namespace title {
namespace {
constexpr size_t kLegacyBytes = 47 + Input::kPorts * Input::kSlots * 71;
bool ValidAge(uint64_t now, uint64_t then, uint64_t bias) {
    return bias < UINT64_MAX - (now >= then ? now - then : 0);
}
bool Fields(SnapshotBytes& io, Input::Snapshot& state) {
    uint32_t tag = InputStateCodec::kTag, version = InputStateCodec::kVersion;
    uint32_t count = static_cast<uint32_t>(state.pads.size());
    if (!io.U32(tag) || tag != InputStateCodec::kTag || !io.U32(version) ||
        (version != 1 && version != InputStateCodec::kVersion) ||
        !io.U64(state.boundary) || !io.U64(state.sequence)) return false;
    const bool legacy = version == 1;
    uint64_t readiness = 0, readiness_bias = 0;
    bool readiness_seen = false;
    if (legacy && (!io.U64(readiness) || !io.U64(readiness_bias) ||
        !ValidAge(state.boundary, readiness, readiness_bias))) return false;
    if (!io.Boolean(state.sampled) || (legacy && !io.Boolean(readiness_seen)) || !io.Boolean(state.paused) ||
        !io.U32(count) || count != state.pads.size()) return false;
    for (auto& pad : state.pads) {
        int32_t request = static_cast<int32_t>(pad.request);
        int32_t connection = static_cast<int32_t>(pad.connection);
        uint8_t command = static_cast<uint8_t>(pad.pending);
        uint64_t opened = 0, opening_bias = 0;
        bool ready = false, connected = false;
        auto& sample = pad.sample;
        if (!io.Boolean(pad.open)) return false;
        if (legacy ? (!io.Boolean(ready) || !io.Boolean(connected)) : !io.I32(connection)) return false;
        if (!io.Boolean(pad.analog) || !io.Boolean(pad.pressure) || !io.Boolean(pad.locked) ||
            (legacy && !io.U64(opened)) || !io.U64(pad.command_at) || (legacy && !io.U64(opening_bias)) ||
            !io.I32(request) || !io.U8(command) || !io.Boolean(pad.next_analog) || !io.Boolean(pad.next_lock) ||
            !io.Raw(pad.alignment.data(), pad.alignment.size()) ||
            !io.Raw(pad.next_alignment.data(), pad.next_alignment.size()) ||
            !io.Boolean(sample.connected) || !io.U16(sample.buttons) ||
            !io.U8(sample.lx) || !io.U8(sample.ly) || !io.U8(sample.rx) || !io.U8(sample.ry) ||
            !io.Boolean(sample.has_pressure) || !io.Raw(sample.pressure.data(), sample.pressure.size()) ||
            !io.U8(pad.motors.small) || !io.U8(pad.motors.big)) return false;
        pad.request = static_cast<Input::Request>(request);
        pad.pending = static_cast<Input::Command>(command);
        pad.connection = static_cast<Input::State>(connection);
        if (legacy) {
            if (!ValidAge(state.boundary, opened, opening_bias) || connected != sample.connected ||
                (ready && (!pad.open || !connected)) || (command && !ready)) return false;
            const uint64_t age = (state.boundary >= opened ? state.boundary - opened : 0) + opening_bias;
            pad.connection = !pad.open || !connected ? Input::State::Disconnected :
                ready ? (pad.request == Input::Request::Busy ? Input::State::Busy : Input::State::Stable) :
                age < 8 ? Input::State::Disconnected : Input::State::Finding;
        }
    }
    return io.Done();
}
bool BadEncoding(std::string* error) {
    if (error) *error = "Input: invalid snapshot payload length, version or field encoding";
    return false;
}
}

bool InputStateCodec::Encode(const Input::Snapshot& snapshot, std::vector<uint8_t>& out, std::string* error) {
    if (!Input::ValidateSnapshot(snapshot, error)) return false;
    auto value = snapshot;
    std::vector<uint8_t> bytes;
    bytes.reserve(kPayloadBytes);
    SnapshotBytes writer(bytes, kPayloadBytes);
    if (!Fields(writer, value) || bytes.size() != kPayloadBytes) return BadEncoding(error);
    out = std::move(bytes);
    return true;
}
bool InputStateCodec::Decode(const std::vector<uint8_t>& bytes, Input::Snapshot& out, std::string* error) {
    if (bytes.size() != kPayloadBytes && bytes.size() != kLegacyBytes) return BadEncoding(error);
    Input::Snapshot value;
    SnapshotBytes reader(bytes);
    if (!Fields(reader, value)) return BadEncoding(error);
    if (!Input::ValidateSnapshot(value, error)) return false;
    out = std::move(value);
    return true;
}
bool InputStateCodec::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    Input::Snapshot value;
    return Decode(bytes, value, error);
}
bool InputStateCodec::DecodeSection(const ps2r::savestate::Blob& blob, Input::Snapshot& out, std::string* error) {
    if (blob.tag != kTag || blob.flags != ps2r::savestate::kSectionRequired) {
        if (error) *error = "Input: INPT requires its own tag and REQUIRED flags";
        return false;
    }
    uint32_t version = 0;
    if (blob.payload.size() >= 8)
        for (unsigned i = 0; i < 4; ++i) version |= uint32_t(blob.payload[4 + i]) << (8 * i);
    if (blob.version != version ||
        (version != 1 && version != kVersion)) return BadEncoding(error);
    return Decode(blob.payload, out, error);
}

}  // namespace title
