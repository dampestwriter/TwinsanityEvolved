#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/graphics_producers.h"
#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_import.h"

namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Explicit import of a FROZEN legacy image (the OLD schema: no Graphics
// section -- GXSL, TXAS, SKYA, GFRM, GSCN, GPRS, GDPL -- and no other new
// owner's) into the selected Graphics composition. Every published value is
// DERIVED from the image's own sections, never fabricated:
//   selection (GXSL): the active owners -- the section only ever records the
//     running configuration, and its validator requires equality with it.
//   converted textures (TXAS): TextureImport::Derive over the image's RAM0 and
//     META arena -- the TextureTable's live objects, each selected one's header
//     and the DiskManager block its transfer handle names, through the owned
//     TextureRead's own conversion. Publication writes the owned commit words
//     into the restored RAM (TextureImport::Migrate).
//   sky converted assets (SKYA): not derived by this import; a sky selection
//     refuses by name.
//   source frame/order (GFRM), shared surfaces (GSCN), presentation (GPRS) and
//     display plan (GDPL): the observational owners' boot state -- nothing seen
//     yet; the title's next operations re-establish them, exactly as the
//     retained arm restores the same anchors (the owners publish that reset at
//     their absent positions while this import is prepared).
// Admission is the frozen schema plus the title's inventoried memory
// signature (restore_mhash_expected.tsv), the policy M0 applies to GCLK and
// ServiceImport to SVCS/FILS.
class GraphicsImport {
public:
    using Options = GraphicsProducers::Options;
    struct Census {
        uint64_t admitted = 0, refused = 0, published = 0;
        uint64_t assets = 0, migrated_objects = 0, retained_textures = 0;
    };
    explicit GraphicsImport(Options selected);
    // The inventoried signature of `load_file` (basename match; 0 = none).
    void Approve(const std::string& inventory, const std::string& load_file);
    void Approve(uint64_t signature) { approved_ = signature; }
    // The frozen OLD schema: no Graphics section and no other new owner's.
    static bool LegacySchema(const std::vector<savestate::Blob>& image, std::string* why);
    // Legacy schema with the inventoried memory signature: the only images
    // this import admits. A legacy image it does not admit records the named
    // refusal (LastRefusal) for the owner's error.
    bool Admits(const std::vector<savestate::Blob>& image, std::string* why) const;
    // Derive without mutation (the preflight). False names what the image lacks.
    bool Prepare(const std::vector<savestate::Blob>& image, std::string* error);
    void Discard();
    bool Prepared() const { return prepared_; }
    const ::title::TextureAssets::Snapshot& Textures() const { return derived_.snapshot; }
    const std::vector<uint32_t>& Objects() const { return derived_.objects; }
    // Publication (RAM0 already restored): the owned commit words.
    bool PublishMemory(uint8_t* ram, size_t size, std::string* error);
    const Census& Count() const { return census_; }
    const std::string& LastRefusal() const { return refusal_; }
    void Record(verify::RunMeta& meta) const;

private:
    bool Refuse(std::string* error, const std::string& why);
    Options options_;
    uint64_t approved_ = 0;
    bool prepared_ = false;
    ::title::TextureImport::Derived derived_;
    Census census_;
    mutable std::string refusal_;
};

}  // namespace ps2r::app
