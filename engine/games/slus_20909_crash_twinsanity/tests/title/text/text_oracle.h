#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"
#include "games/slus_20909_crash_twinsanity/tests/title/text/text_fixture.h"

// Current NEW generated producer -> existing NEW typesetter -> NEW vertex
// assembly. Used only in this isolated test target.
class TextOracle {
public:
    struct Page {
        uint32_t material = 0;
        std::vector<ps2r::gs::DrawIr> draws;
    };
    static std::vector<Page> Run(TextFixture&, const title::MaterialState::Environment&);
    static uint64_t Calls();
    // Accumulated model-mode TextService census of every Run (the model arm).
    static title::TextService::Counters ModelCensus();

private:
    static title::TextService::Counters model_census_;
};
