#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "engine/core/verify/memory_checkpointer.h"

namespace ps2r::app {

bool MovieCheckpoint::RegisterBoot(savestate::SectionRegistry& registry, const std::string& inventory,
                                   const std::string& load_file) {
    uint64_t expected = 0;
    if (!load_file.empty()) {
        const auto name = std::filesystem::path(load_file).filename();
        std::ifstream input(inventory);
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            std::string file;
            uint64_t value = 0;
            if (row >> file >> std::hex >> value)
                if (std::filesystem::path(file).filename() == name) { expected = value; break; }
        }
    }
    return Register(registry, [expected](const auto& blobs, std::string* error) {
        return ValidateLegacy(blobs, expected, error);
    });
}

bool MovieCheckpoint::ValidateLegacy(const std::vector<savestate::Blob>& blobs, uint64_t approved_hash,
                                     std::string* error) {
    using namespace savestate;
    // Frozen OLD schema; a missing MOVI in any image with new service-owned
    // sections is a defect, not permission to import boot-era Movie state.
    constexpr std::array<uint32_t, 29> legacy_tags{
        TAG_RAM, TAG_SPAD, TAG_META, TAG_VRAM, TAG_VU0C, TAG_VU0D, TAG_VU1C, TAG_VU1D,
        TAG_THRD, TAG_SEMA, TAG_EVFL, TAG_INTC, TAG_DMAC, TAG_TIMR, TAG_GSPR, TAG_GSST,
        TAG_VIFS, TAG_GIFS, TAG_PADS, TAG_FILE, TAG_CNTR, TAG_MISC, TAG_HEAP, TAG_STRM,
        TAG_SIFS, TAG_VU1R, TAG_SDRX, TAG_MCHL, TAG_IOPM};
    const auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!approved_hash) return fail("Movie boot import has no approved memory signature");
    std::set<uint32_t> found;
    std::array<const Blob*, 3> memory{};
    constexpr std::array<uint32_t, 3> tags{TAG_RAM, TAG_SPAD, TAG_META};
    for (const auto& blob : blobs) {
        if (!found.insert(blob.tag).second) return fail("Movie boot import contains duplicate owners");
        if (blob.tag != TAG_SNDC &&
            std::find(legacy_tags.begin(), legacy_tags.end(), blob.tag) == legacy_tags.end())
            return fail("Movie owner is missing from an image with new service state");
        for (size_t i = 0; i < tags.size(); ++i) if (blob.tag == tags[i]) memory[i] = &blob;
    }
    std::vector<verify::MemorySpan> spans;
    for (const auto* blob : memory) {
        if (!blob || blob->payload.empty()) return fail("Movie boot import requires all frozen memory sections");
        spans.push_back({blob->payload.data(), blob->payload.size()});
    }
    if (verify::MemoryCheckpointer::Hash(spans) != approved_hash)
        return fail("Movie boot import memory differs from its frozen inventory signature");
    return true;
}

bool MovieCheckpoint::PrepareBoot(const std::vector<savestate::Blob>& original,
                                  std::vector<savestate::Blob>& owned, bool& imported,
                                  std::string* error) const {
    if (!ValidateImage(original, error)) return false;
    auto next = original;
    const bool missing = std::none_of(original.begin(), original.end(), [](const auto& blob) {
        return blob.tag == kTag;
    });
    if (missing) {
        // ValidateImage has classified the *original* schema and exact frozen
        // memory signature. OLD recorded no software contexts, PCM or loans.
        // Build the same closed identity ResetAfterRestore formerly derived,
        // without resetting live services or losing new saved decoder clocks.
        Snapshot state;
        state.decoder_services = DecoderServices();
        state.stream_services = stream_.Services();
        if (state.decoder_services & 1u) {
            const auto ram = std::find_if(original.begin(), original.end(), [](const auto& blob) {
                return blob.tag == savestate::TAG_RAM;
            });
            constexpr uint32_t at = ::title::MovieDecoder::kDecoderPointer;
            if (ram == original.end() || uint64_t(at) + 4 > ram->payload.size()) {
                if (error) *error = "Movie boot import has no incoming decoder identity";
                return false;
            }
            for (unsigned i = 0; i < 4; ++i)
                state.stopped_decoder |= uint32_t(ram->payload[at + i]) << (i * 8);
        }
        savestate::Blob movie{kTag, kVersion, savestate::kSectionRequired, {}};
        if (!Encode(state, movie.payload, error)) return false;
        next.push_back(std::move(movie));
        if (!ValidateImage(next, error)) return false;
    }
    owned = std::move(next);
    imported = missing;
    return true;
}

}  // namespace ps2r::app
