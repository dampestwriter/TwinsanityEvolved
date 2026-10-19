#include "games/slus_20909_crash_twinsanity/hle/math/culling_checkpoint.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/math/culling_state_codec.h"

namespace title {
namespace {
bool Envelope(const ps2r::savestate::Blob& blob, std::string* error) {
    if (blob.version == 1 && blob.flags == ps2r::savestate::kSectionRequired) return true;
    if (error) *error = "Culling: incompatible CULL section version or ownership flags";
    return false;
}
bool Refuse(std::string* reason, const std::string& text) {
    if (reason) *reason = text;
    return false;
}
bool CullingFrame(uint32_t function) {
    if (function == CullingCheckpoint::kDrawScene) return true;
    for (const auto& entry : CullingLibrary::Entries())
        if (entry.address == function) return true;
    return false;
}
}

bool CullingCheckpoint::LegacyImportAdmissible(const std::vector<ps2r::savestate::Blob>& sections,
                                               const ChainParser& chains, std::string* reason) {
    const ps2r::savestate::Blob* threads = nullptr;
    for (const auto& blob : sections) {
        if (blob.tag == CullingStateCodec::kTag) return true;  // Owned state present: no import.
        if (blob.tag == ps2r::savestate::TAG_THRD) threads = &blob;
    }
    if (!threads) return Refuse(reason, "Culling: legacy image without CULL carries no THRD to prove no live chunk view");
    if (!chains) return Refuse(reason, "Culling: legacy image without CULL needs the application's thread chain codec");
    std::vector<ThreadChain> parsed;
    std::string error;
    if (!chains(threads->payload, parsed, &error))
        return Refuse(reason, "Culling: legacy image THRD unreadable: " + error);
    for (const auto& chain : parsed) {
        if (!chain.live) continue;
        for (size_t level = 0; level < chain.functions.size(); ++level) {
            if (!CullingFrame(chain.functions[level])) continue;
            char detail[96];
            std::snprintf(detail, sizeof(detail), "tid=%d level=%zu function=0x%x", chain.tid, level,
                          chain.functions[level]);
            return Refuse(reason, std::string("Culling: legacy image holds a live chunk view (") + detail +
                "); no migration from the register model");
        }
    }
    return true;
}

bool CullingCheckpoint::Register(ps2r::savestate::SectionRegistry& registry, CullingLibrary& library,
                                 ChainParser chains) {
    ps2r::savestate::SectionSpec spec;
    spec.tag = CullingStateCodec::kTag;
    spec.owner = "CullingLibrary";
    spec.required = false;
    spec.capture_required = true;
    spec.serialize = [&library](std::vector<uint8_t>& output) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!CullingStateCodec::Encode(library.Capture(), bytes, &error))
            throw std::runtime_error(error);
        output.insert(output.end(), bytes.begin(), bytes.end());
    };
    spec.validate = [](const ps2r::savestate::Blob& blob, std::string* error) {
        return Envelope(blob, error) && CullingStateCodec::Validate(blob.payload, error);
    };
    spec.restore = [&library](const ps2r::savestate::Blob& blob, std::string* error) {
        CullingState::Snapshot snapshot;
        return Envelope(blob, error) && CullingStateCodec::Decode(blob.payload, snapshot, error) &&
            library.Restore(snapshot, error);
    };
    // Reached only for an image the whole-candidate validation admitted below.
    spec.absent = [&library]() { library.ImportLegacy(); };
    if (!registry.Register(spec)) return false;
    return registry.RegisterValidation(spec.owner,
        [chains = std::move(chains)](const std::vector<ps2r::savestate::Blob>& sections, std::string* error) {
            return LegacyImportAdmissible(sections, chains, error);
        });
}

}  // namespace title
