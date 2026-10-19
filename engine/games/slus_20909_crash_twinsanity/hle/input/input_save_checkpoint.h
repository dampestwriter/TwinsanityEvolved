#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"

namespace title {

class InputSaveCheckpoint {
public:
    static constexpr uint32_t kTag = 0x50435349; // ISCP
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kInput = 1, kSaveStorage = 2, kAll = kInput | kSaveStorage;
    static constexpr size_t kPayloadBytes = 12;
    enum class Clock { Unavailable, OldSyncV, NewBoundary };
    struct LegacyPolicy {
        // The parent identifies a known legacy producer/file. Never infer
        // approval just because ISCP/INPT/SVST are absent.
        bool approved = false;
        Clock clock = Clock::Unavailable;
        uint64_t boundary = 0; // NewBoundary only; OldSyncV reads original CNTR.
    };
    struct Plan {
        uint32_t selection = 0;
        bool legacy_file = false;
        bool imported_input = false;
        bool imported_saves = false;
        std::optional<Input::Snapshot> input;
        std::optional<SaveBindings::Snapshot> saves;
    };

    // Root writes a REQUIRED ISCP v1 even for mask 0 (both models selected).
    // Decode checks the whole marker Blob, including version/required flags.
    // On failure, every output argument remains unchanged.
    static bool EncodeSelection(uint32_t mask, std::vector<uint8_t>& out, std::string* error = nullptr);
    static bool DecodeSelection(const ps2r::savestate::Blob& marker, uint32_t& out, std::string* error = nullptr);

    // Input is the CRC-validated, UNFILTERED original blob list. This only
    // prepares values: no registration, restore, model, callbacks, or I/O.
    // Unknown tags remain the parent's strict ownership/validation concern.
    // Before applying a plan, root must also validate all other sections,
    // title identity, THRD continuations, pending SIF work and owner readiness.
    static bool Prepare(const std::vector<ps2r::savestate::Blob>& blobs, uint32_t selection,
                        const LegacyPolicy& legacy, Plan& out, std::string* error = nullptr);
};

}  // namespace title
