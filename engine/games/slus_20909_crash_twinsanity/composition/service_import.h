#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {
class FileServices;
class SessionServices;

// Explicit import of a FROZEN legacy image (the OLD schema: no GCLK, SVCS or
// FILS) into the selected file composition. Every published value is DERIVED
// from the image's own sections; nothing is fabricated and nothing is taken
// from the retained hardware records (IOPM, STRM, FILE):
//   files (SVCS, file-only arm): the title's own StreamSystem/FileStream objects
//     in the image's RAM0 (the manager pointer the title stores at
//     kStreamManagerPointer) -> StreamState::MigrateFiles: open ordinary and
//     archive handles plus the one pending-read class the EE side records (a
//     staged ring fill). An unidentifiable file, a direct pending read or an
//     Audio-composed selection refuses by name.
//   disc files (FILS): the title holds synchronous descriptors only in stack-
//     local File objects for one load step (open/read/close inside the same
//     function; TR src/game/shadows.cpp:803-845, src/platform/ps2/stream.cpp:
//     99-105) and OLD persisted none, so the derived descriptor set is empty
//     with the mounted medium's identity; a saved thread chain inside the
//     synchronous file family refuses ("step in flight").
//   clock (GCLK): M0's historical boot policy -- the clockless import the
//     retained arm restores these anchors under; the derived schedule carries
//     no deadline and a re-issued ring fill takes the restore tick.
// Admission is the frozen schema plus the title's inventoried memory
// signature (restore_mhash_expected.tsv), the policy M0 applies to GCLK.
class ServiceImport {
public:
    static constexpr uint32_t kStreamManagerPointer = 0x0030a748;   // FUN_002ad560 stores it (gp-0x6da8)
    struct Census {
        uint64_t admitted = 0, refused = 0, files_published = 0, disc_published = 0;
        uint64_t files = 0, pending_windows = 0;
    };
    ServiceImport(SessionServices& services, FileServices& files, bool audio_selected,
                  StreamState::ReadDisc read_disc, std::function<uint64_t()> tick);
    // The inventoried signature of `load_file` (basename match; 0 = none).
    void Approve(const std::string& inventory, const std::string& load_file);
    void Approve(uint64_t signature) { approved_ = signature; }
    // The frozen OLD schema: no GCLK/SVCS/FILS and no other new owner's
    // section. Pure; works on the original image and the owned subset.
    static bool LegacySchema(const std::vector<savestate::Blob>& image, std::string* why);
    // Legacy schema with the inventoried memory signature: the only images
    // this import admits. A legacy-schema image it does not admit records the
    // named refusal (LastRefusal) for the owner's error.
    bool Admits(const std::vector<savestate::Blob>& image, std::string* why) const;
    // Derive and validate without mutation (the preflight); the pending ring
    // fills' bytes are read from the medium here so publication cannot fail
    // on I/O. False names what the image lacks.
    bool Prepare(const std::vector<savestate::Blob>& image, std::string* error);
    void Discard();
    bool Prepared() const { return prepared_; }
    // Publication at the owners' absent positions (RAM already restored).
    bool PublishFiles(std::string* error);
    bool PublishDisc(std::string* error);
    const Census& Count() const { return census_; }
    const std::string& LastRefusal() const { return refusal_; }
    void Record(verify::RunMeta& meta) const;

private:
    bool Derive(const std::vector<savestate::Blob>& image, LoaderFileIo::State& state,
                std::vector<StreamState::PendingWindow>& pending, std::string* error) const;
    bool Refuse(std::string* error, const std::string& why);

    SessionServices& services_;
    FileServices& files_;
    bool audio_selected_ = false;
    StreamState::ReadDisc read_disc_;
    std::function<uint64_t()> tick_;
    uint64_t approved_ = 0;
    bool prepared_ = false;
    LoaderFileIo::State state_;
    std::vector<StreamState::PendingWindow> pending_;
    struct Key {   // a staged fill's identity on the medium
        uint32_t lba = 0; uint64_t position = 0; uint32_t fill = 0;
        bool operator<(const Key& o) const {
            return lba != o.lba ? lba < o.lba : position != o.position ? position < o.position : fill < o.fill;
        }
    };
    std::map<Key, std::vector<uint8_t>> staged_;
    Census census_;
    mutable std::string refusal_;
};

} // namespace ps2r::app
