#include "games/slus_20909_crash_twinsanity/composition/graphics_import.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/core/verify/memory_checkpointer.h"
#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/presentation_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/scene_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace ps2r::app {
namespace {
using savestate::Blob;

// The frozen OLD schema (SNDC optional). Any other tag is a new owner's
// record: an image carrying one is a NEW image missing a section, never a
// legacy import candidate. (The same list ServiceImport admits.)
constexpr std::array<uint32_t, 30> kFrozenTags{
    savestate::TAG_RAM,  savestate::TAG_SPAD, savestate::TAG_META, savestate::TAG_VRAM, savestate::TAG_VU0C,
    savestate::TAG_VU0D, savestate::TAG_VU1C, savestate::TAG_VU1D, savestate::TAG_THRD, savestate::TAG_SEMA,
    savestate::TAG_EVFL, savestate::TAG_INTC, savestate::TAG_DMAC, savestate::TAG_TIMR, savestate::TAG_GSPR,
    savestate::TAG_GSST, savestate::TAG_VIFS, savestate::TAG_GIFS, savestate::TAG_PADS, savestate::TAG_FILE,
    savestate::TAG_CNTR, savestate::TAG_MISC, savestate::TAG_HEAP, savestate::TAG_STRM, savestate::TAG_SIFS,
    savestate::TAG_VU1R, savestate::TAG_SDRX, savestate::TAG_MCHL, savestate::TAG_IOPM, savestate::TAG_SNDC};
constexpr uint32_t kSelectionTag = 0x4c535847;   // GXSL (GraphicsRuntime)
constexpr uint32_t kPlanTag = 0x4e4c5046;        // FPLN (GraphicsFrame)

const Blob* Find(const std::vector<Blob>& image, uint32_t tag) {
    for (const auto& blob : image) if (blob.tag == tag) return &blob;
    return nullptr;
}
}  // namespace

GraphicsImport::GraphicsImport(Options selected) : options_(selected) {}

void GraphicsImport::Approve(const std::string& inventory, const std::string& load_file) {
    approved_ = 0;
    if (load_file.empty()) return;
    const auto name = std::filesystem::path(load_file).filename();
    std::ifstream input(inventory);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string file;
        uint64_t value = 0;
        if (row >> file >> std::hex >> value && std::filesystem::path(file).filename() == name) {
            approved_ = value;
            return;
        }
    }
}

bool GraphicsImport::Refuse(std::string* error, const std::string& why) {
    refusal_ = "legacy Graphics import: " + why;
    ++census_.refused;
    if (error) *error = refusal_;
    return false;
}

bool GraphicsImport::LegacySchema(const std::vector<Blob>& image, std::string* why) {
    const auto no = [&](const char* reason) { if (why) *why = reason; return false; };
    for (const auto tag : {kSelectionTag, ::title::TextureStateCodec::kTag, ::title::SkyState::kTag,
                           ::title::FrameState::kTag, ::title::SceneState::kTag, ::title::PresentationState::kTag, kPlanTag})
        if (Find(image, tag)) return no("image carries new Graphics state");
    for (const auto& blob : image)
        if (std::find(kFrozenTags.begin(), kFrozenTags.end(), blob.tag) == kFrozenTags.end())
            return no("image carries a new owner's section");
    return true;
}

bool GraphicsImport::Admits(const std::vector<Blob>& image, std::string* why) const {
    if (!LegacySchema(image, why)) return false;
    const auto no = [&](const char* reason) {
        refusal_ = std::string("legacy Graphics import: ") + reason;
        if (why) *why = reason;
        return false;
    };
    const Blob* ram = Find(image, savestate::TAG_RAM);
    const Blob* spad = Find(image, savestate::TAG_SPAD);
    const Blob* meta = Find(image, savestate::TAG_META);
    if (!ram || !spad || !meta || ram->payload.empty() || spad->payload.empty() || meta->payload.empty())
        return no("image lacks the frozen memory sections");
    if (!approved_) return no("image is not an inventoried frozen anchor");
    const std::vector<verify::MemorySpan> spans{{ram->payload.data(), ram->payload.size()},
                                                {spad->payload.data(), spad->payload.size()},
                                                {meta->payload.data(), meta->payload.size()}};
    if (verify::MemoryCheckpointer::Hash(spans) != approved_)
        return no("image memory differs from its inventoried frozen signature");
    return true;
}

bool GraphicsImport::Prepare(const std::vector<Blob>& image, std::string* error) {
    Discard();
    std::string why;
    if (!Admits(image, &why)) return Refuse(error, why);
    if (options_.sky) return Refuse(error, "sky converted assets are not derived from a frozen image");
    ::title::TextureImport::Derived derived;
    if (options_.sprites || options_.text) {
        const Blob* ram = Find(image, savestate::TAG_RAM);
        const Blob* meta = Find(image, savestate::TAG_META);
        const std::vector<uint8_t>& bytes = ram->payload;
        const std::vector<uint8_t>& arena = meta->payload;
        // Physical RAM0 at 0, the META arena at its guest base (the DiskManager
        // nodes of an image written with hle.meta_arena routing live there).
        const ::title::TextureImport::Read read = [&bytes, &arena](uint32_t address, void* out, size_t size) {
            const uint32_t at = address & 0x1fffffff;
            const std::vector<uint8_t>* span = &bytes;
            uint32_t base = 0;
            if (at >= MetaArena::kBase) { span = &arena; base = MetaArena::kBase; }
            const uint64_t offset = uint64_t(at) - base;
            if (offset + size > span->size()) return false;
            std::memcpy(out, span->data() + offset, size);
            return true;
        };
        if (!::title::TextureImport::Derive(read, {options_.text, options_.sprites}, derived, &why))
            return Refuse(error, "textures: " + why);
    }
    derived_ = std::move(derived);
    prepared_ = true;
    ++census_.admitted;
    census_.assets = derived_.snapshot.assets.size();
    census_.migrated_objects = derived_.objects.size();
    census_.retained_textures = derived_.retained;
    if (error) error->clear();
    return true;
}

void GraphicsImport::Discard() {
    prepared_ = false;
    derived_ = {};
}

bool GraphicsImport::PublishMemory(uint8_t* ram, size_t size, std::string* error) {
    if (!prepared_) return Refuse(error, "no prepared import to publish");
    if (!::title::TextureImport::Migrate(ram, size, derived_.objects))
        return Refuse(error, "restored RAM does not hold the derived texture objects");
    ++census_.published;
    return true;
}

void GraphicsImport::Record(verify::RunMeta& meta) const {
    meta.Set("graphics_import", "admitted=" + std::to_string(census_.admitted) +
             ",refused=" + std::to_string(census_.refused) +
             ",published=" + std::to_string(census_.published) +
             ",assets=" + std::to_string(census_.assets) +
             ",migrated_objects=" + std::to_string(census_.migrated_objects) +
             ",retained_textures=" + std::to_string(census_.retained_textures) +
             ",last_refusal=" + (refusal_.empty() ? std::string("none") : refusal_));
}

}  // namespace ps2r::app
