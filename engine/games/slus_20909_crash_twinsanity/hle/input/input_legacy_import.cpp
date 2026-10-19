#include "games/slus_20909_crash_twinsanity/hle/input/input_legacy_import.h"

#include <algorithm>

namespace title {
namespace {
constexpr uint32_t kRamBytes = 32 * 1024 * 1024, kPortTable = 0x3c6e90, kStaticArea = 0x3c6f80;
uint32_t U32(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
        (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}
bool Fail(std::string* error, const char* why) {
    if (error) *error = std::string("Input legacy import: ") + why;
    return false;
}
bool Motors(const std::vector<uint8_t>& ram, uint32_t entry, unsigned port,
            Input::PadState& pad, std::string* error) {
    // NTSC scePadSetActDirect (0x2c30d8) retains the last motor intent here.
    // TR CreateGamePadInfo fixes alignment to {0,1,ff,ff,ff,ff}.
    const uint32_t buffer = kStaticArea + port * 0x80;
    if ((U32(ram, entry + 4) & 0x1fffffff) != buffer)
        return Fail(error, "unrecognised actuator buffer");
    const uint32_t command = U32(ram, buffer + 4), length = U32(ram, buffer + 8);
    if (command == 0 && length == 0 &&
        std::all_of(ram.begin() + buffer + 12, ram.begin() + buffer + 18,
                    [](uint8_t byte) { return byte == 0; })) return true;
    if (command != 1 || length != 6 || pad.connection != Input::State::Stable || ram[buffer + 12] > 1 ||
        !std::all_of(ram.begin() + buffer + 14, ram.begin() + buffer + 18,
                     [](uint8_t byte) { return byte == 0; }))
        return Fail(error, "unrecoverable actuator command");
    pad.motors = {ram[buffer + 12], ram[buffer + 13]};
    return true;
}
bool Port(const std::vector<uint8_t>& pads, const std::vector<uint8_t>& ram,
          unsigned port, Input::Snapshot& snapshot, bool& sequence_seen, std::string* error) {
    const uint32_t area = U32(pads, port * 4), physical = area & 0x1fffffff;
    const uint8_t stage = pads[16 + port], stamped = pads[20 + port];
    const uint32_t entry = kPortTable + port * 0x70;
    if (stage > 2 || stamped > 1) return Fail(error, "invalid stage or ready marker");
    for (unsigned slot = 1; slot < Input::kSlots; ++slot)
        if (U32(ram, entry + slot * 0x1c + 0x10) != 0)
            return Fail(error, "open multitap slot has no independent PADS state");
    const bool opened = U32(ram, entry + 0x10) != 0;
    if (!area) {
        if (opened || stage != 0 || stamped != 0)
            return Fail(error, "closed port disagrees with PADS or SDK ownership");
        return true;
    }
    if (!opened || (U32(ram, entry) & 0x1fffffff) != physical ||
        physical == 0 || physical > kRamBytes - 256 || (physical & 63) != 0)
        return Fail(error, "pad area is invalid or disagrees with SDK ownership");
    const uint32_t first = U32(ram, physical + 0x58), second = U32(ram, physical + 0xd8);
    if (first == 0 || second != first + uint32_t{1} ||
        (sequence_seen && snapshot.sequence != first))
        return Fail(error, "ambiguous or inconsistent report sequence");
    snapshot.sequence = first;  // BUG-074: same derivation as current NEW PADS restore.
    sequence_seen = true;
    // SDK's signed comparison; equal chooses the first. XOR maps signed order
    // into unsigned order without implementation-defined signed conversion.
    const uint32_t selected = physical + ((first ^ 0x80000000u) < (second ^ 0x80000000u) ? 128 : 0);
    auto& pad = snapshot.pads[port * Input::kSlots];
    pad.open = true;
    const uint64_t opened_at = U32(pads, 8 + port * 4), readiness = U32(pads, 22);
    pad.sample.connected = true;  // PADS staged a present DS2.
    pad.connection = stage == 2 ? Input::State::Stable :
        stage == 1 ? Input::State::Finding : Input::State::Disconnected;
    if (stamped != (stage == 2 ? 1 : 0)) return Fail(error, "ready marker disagrees with stage");
    if (stage == 2) {
        if (ram[selected + 0x71] != 0)
            return Fail(error, "busy or failed setup command has no recoverable intent");
        if (ram[selected + 0x70] != 6 || ram[selected + 0x72] != 1 ||
            U32(ram, selected + 0x60) != 32 || ram[selected + 1] != 0x79)
            return Fail(error, "unsupported ready report");
        if (!std::equal(ram.begin() + physical, ram.begin() + physical + 32, ram.begin() + physical + 128))
            return Fail(error, "torn or noncoherent report halves");
        pad.request = Input::Request::Complete;
        pad.pressure = pad.sample.has_pressure = true;
        pad.sample.buttons = static_cast<uint16_t>(~(uint16_t(ram[selected + 2]) |
            (uint16_t(ram[selected + 3]) << 8)));
        pad.sample.rx = ram[selected + 4];
        pad.sample.ry = ram[selected + 5];
        pad.sample.lx = ram[selected + 6];
        pad.sample.ly = ram[selected + 7];
        std::copy_n(ram.begin() + selected + 8, 12, pad.sample.pressure.begin());
        const auto report = Input::MakeReport(pad.sample, true, true);
        if (!std::equal(report.begin(), report.end(), ram.begin() + selected))
            return Fail(error, "report contains unsupported data");
    } else {
        if (opened_at > snapshot.boundary || readiness > snapshot.boundary)
            return Fail(error, "opening clock requires unsupported wrap or future rebase");
        const auto age = snapshot.boundary - opened_at;
        const auto expected = age < 8 ? 0 : (age < 214 &&
            (!readiness || snapshot.boundary - readiness < 2) ? 1 : 2);
        if (stage != expected || ram[selected + 0x70] != stage || ram[selected + 0x71] != 2 ||
            ram[selected + 0x72] != 0 || U32(ram, selected + 0x60) != 0)
            return Fail(error, "opening report disagrees with source clock or stage");
    }
    return Motors(ram, entry, port, pad, error);
}
}

bool InputLegacyImport::Decode(uint32_t pads_version, const std::vector<uint8_t>& pads,
                              const std::vector<uint8_t>& ram0, uint64_t source_boundary,
                              Input::Snapshot& out, std::string* error) {
    if (pads_version != 1 || pads.size() != 166 || ram0.size() != kRamBytes)
        return Fail(error, "requires PADS v1 (166 bytes) and complete RAM0 (32 MiB)");
    if (source_boundary > UINT32_MAX)
        return Fail(error, "source clock exceeds the legacy timestamp range");
    Input::Snapshot snapshot;
    snapshot.boundary = source_boundary;
    snapshot.sampled = true;
    bool sequence_seen = false;
    for (unsigned port = 0; port < Input::kPorts; ++port)
        if (!Port(pads, ram0, port, snapshot, sequence_seen, error)) return false;
    // I2: old read-count latch fields (+26..165) are intentionally ignored,
    // exactly as current NEW. Pending physical edges belong to the host.
    if (!Input::ValidateSnapshot(snapshot, error)) return false;
    out = snapshot;
    return true;
}

}  // namespace title
