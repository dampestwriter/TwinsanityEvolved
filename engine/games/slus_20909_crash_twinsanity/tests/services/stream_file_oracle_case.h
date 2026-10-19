#pragma once

#include "games/slus_20909_crash_twinsanity/tests/services/stream_oracle_inputs.h"

int RunModelFileOracle(StreamOracleInputs& fixture, uint32_t kFs,
    const IsoFs::Entry& file, const std::vector<uint8_t>& oracle);
int RunOwnedFileOracle(StreamOracleInputs& fixture, uint32_t kFs, uint8_t kSlot,
    const IsoFs::Entry& file);
