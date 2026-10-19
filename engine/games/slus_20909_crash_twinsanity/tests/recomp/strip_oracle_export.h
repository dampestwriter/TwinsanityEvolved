#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/graphics_strip.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

namespace strip_oracle {

// Self-oracle artifacts for StripSelfOracle (verify/retirement_artifacts.py
// check_oracle): per recorded run the CONSUMED fixture image (sparse non-zero
// 4 KiB pages of the generated body's RAM, the environment, the call registers)
// and the observable output (canonical DrawRec + out-of-record raster/texture
// fields + vertices), for the model arm (`hle.graphics_strips=model`) and the
// native arm (`hle.graphics_strips=all`), with the actual Strip counters.
// Receipts are written only when the whole oracle passed; an existing export
// directory is refused so prior evidence is never overwritten. Without
// --export-dir nothing is recorded and the test behaves exactly as before.
class Export {
public:
    struct Registers { uint32_t a0 = 0, a1 = 0, a2 = 0; };
    // argv[first..] may hold `--export-dir DIR --source-manifest FILE` (both or
    // neither). Returns false on any other shape.
    bool Parse(int argc, char** argv, int first);
    bool Enabled() const { return artifacts_.Enabled(); }
    // The consumed input, immediately before the recorded body executes.
    void Input(bool native, size_t variant, const Registers& registers,
               const title::MaterialState::Environment& environment,
               const uint8_t* ram, size_t size);
    // The run's observable output: zero or one draw.
    void Output(bool native, const ps2r::gs::DrawIr* draw);
    // The run's actual execution census.
    void Counters(bool native, const title::GraphicsService::Counters& service,
                  const ps2r::app::GraphicsStrip::Counters& owned);
    // Writes model/ and native/ with input.bin, draw.bin, vertices.bin,
    // receipt.json, then artifacts.json. Call once, after every REQUIRE passed.
    void Finish(int argc, char** argv);
    size_t Recorded(bool native) const { return arms_[native].runs; }

private:
    struct Arm {
        size_t runs = 0, draws = 0;
        title::GraphicsService::Counters service{};
        ps2r::app::GraphicsStrip::Counters owned{};
    };
    retirement::OracleArtifacts artifacts_;
    Arm arms_[2];
};

}  // namespace strip_oracle
