#include "games/slus_20909_crash_twinsanity/hle/input/input_save_checkpoint.h"

#include <unordered_set>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/input/input_legacy_import.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/input/snapshot_bytes.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_legacy_import.h"

namespace title {
namespace {
using Checkpoint = InputSaveCheckpoint;
using ps2r::savestate::Blob;
using namespace ps2r::savestate;

bool Fail(std::string* error, const char* why) {
    if (error) *error = std::string("Input/Save checkpoint: ") + why;
    return false;
}
const Blob* Find(const std::vector<Blob>& blobs, uint32_t tag) {
    for (const auto& blob : blobs) if (blob.tag == tag) return &blob;
    return nullptr;
}
uint32_t U32(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
        (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}
bool ServiceBlob(const Blob& blob, std::string* error) {
    if (blob.version != 1 || blob.flags != kSectionRequired)
        return Fail(error, "service sections require version 1 and REQUIRED flags");
    return true;
}
bool LegacyInput(const std::vector<Blob>& blobs, const Checkpoint::LegacyPolicy& policy,
                 Checkpoint::Plan& plan, std::string* error) {
    const auto* pads = Find(blobs, TAG_PADS);
    const auto* ram = Find(blobs, TAG_RAM);
    if (!pads || !ram) return Fail(error, "legacy Input requires original PADS and RAM0");
    if (ram->version != 1 || (ram->flags & ~kSectionRequired) || (pads->flags & ~kSectionRequired))
        return Fail(error, "unsupported legacy RAM0 version or section flags");
    uint64_t boundary = policy.boundary;
    if (policy.clock == Checkpoint::Clock::OldSyncV) {
        const auto* counters = Find(blobs, TAG_CNTR);
        if (!counters || counters->version != 1 || counters->payload.size() != 40 ||
            (counters->flags & ~kSectionRequired))
            return Fail(error, "OLD Input requires original CNTR v1 with 40 bytes");
        boundary = U32(counters->payload, 28);
    } else if (policy.clock != Checkpoint::Clock::NewBoundary) {
        return Fail(error, "legacy Input source clock is not established");
    }
    Input::Snapshot snapshot;
    if (!InputLegacyImport::Decode(pads->version, pads->payload, ram->payload, boundary, snapshot, error))
        return false;
    plan.input = std::move(snapshot);
    plan.imported_input = true;
    return true;
}
bool Marked(const std::vector<Blob>& blobs, uint32_t selection, Checkpoint::Plan& plan, std::string* error) {
    const auto* input = Find(blobs, InputStateCodec::kTag);
    const auto* saves = Find(blobs, SaveStateCodec::kTag);
    if (bool(selection & Checkpoint::kInput) != bool(input))
        return Fail(error, selection & Checkpoint::kInput ? "selected Input section INPT missing" :
                    "INPT present while Input is deselected");
    if (bool(selection & Checkpoint::kSaveStorage) != bool(saves))
        return Fail(error, selection & Checkpoint::kSaveStorage ? "selected SaveStorage section SVST missing" :
                    "SVST present while SaveStorage is deselected");
    if ((input && Find(blobs, TAG_PADS)) || (saves && Find(blobs, TAG_MCHL)))
        return Fail(error, "selected service and retired owner sections are both present");
    // Decoding all values before publishing the plan prevents partial
    // installation if the second service payload is malformed.
    if (input) {
        Input::Snapshot value;
        if (!InputStateCodec::DecodeSection(*input, value, error)) return false;
        plan.input = std::move(value);
    }
    if (saves) {
        SaveBindings::Snapshot value;
        if (!ServiceBlob(*saves, error) || !SaveStateCodec::Decode(saves->payload, value, error)) return false;
        plan.saves = std::move(value);
    }
    return true;
}
}

bool InputSaveCheckpoint::EncodeSelection(uint32_t mask, std::vector<uint8_t>& out, std::string* error) {
    if (mask & ~kAll) return Fail(error, "unknown Input/Save selection bits");
    std::vector<uint8_t> bytes;
    SnapshotBytes writer(bytes, kPayloadBytes);
    uint32_t tag = kTag, version = kVersion;
    if (!writer.U32(tag) || !writer.U32(version) || !writer.U32(mask))
        return Fail(error, "selection marker encoding failed");
    out = std::move(bytes);
    if (error) error->clear();
    return true;
}
bool InputSaveCheckpoint::DecodeSelection(const Blob& marker, uint32_t& out, std::string* error) {
    if (marker.tag != kTag || marker.version != kVersion || marker.flags != kSectionRequired ||
        marker.payload.size() != kPayloadBytes)
        return Fail(error, "ISCP requires version 1, REQUIRED flags and 12 bytes");
    uint32_t tag = 0, version = 0, mask = 0;
    SnapshotBytes reader(marker.payload);
    if (!reader.U32(tag) || !reader.U32(version) || !reader.U32(mask) || !reader.Done() ||
        tag != kTag || version != kVersion || (mask & ~kAll))
        return Fail(error, "invalid ISCP payload version, magic or selection bits");
    out = mask;
    if (error) error->clear();
    return true;
}
bool InputSaveCheckpoint::Prepare(const std::vector<Blob>& blobs, uint32_t selection,
                                 const LegacyPolicy& legacy, Plan& out, std::string* error) {
    if (selection & ~kAll) return Fail(error, "unknown Input/Save selection bits");
    // CRC-valid duplicate sections must not select whichever occurrence a
    // later owner happens to read. Check every tag, including unowned ones.
    std::unordered_set<uint32_t> seen;
    for (const auto& blob : blobs)
        if (!seen.insert(blob.tag).second) return Fail(error, "duplicate section tag");
    Plan plan;
    plan.selection = selection;
    const auto* marker = Find(blobs, kTag);
    if (marker) {
        uint32_t saved_selection = 0;
        if (!DecodeSelection(*marker, saved_selection, error)) return false;
        if (saved_selection != selection) return Fail(error, "saved and current Input/Save selections differ");
        if (!Marked(blobs, selection, plan, error)) return false;
    } else {
        if (Find(blobs, InputStateCodec::kTag) || Find(blobs, SaveStateCodec::kTag))
            return Fail(error, "service section present without required ISCP selection marker");
        if (!legacy.approved)
            return Fail(error, "ISCP missing; legacy file provenance has not been approved");
        plan.legacy_file = true;
        if (selection & kSaveStorage) {
            SaveBindings::Snapshot snapshot;
            if (!SaveLegacyImport::Decode(blobs, snapshot, error)) return false;
            plan.saves = std::move(snapshot);
            plan.imported_saves = true;
        }
        if ((selection & kInput) && !LegacyInput(blobs, legacy, plan, error)) return false;
    }
    out = std::move(plan);
    if (error) error->clear();
    return true;
}

}  // namespace title
