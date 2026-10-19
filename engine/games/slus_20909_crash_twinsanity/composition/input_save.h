#pragma once

#include <functional>
#include <string>
#include <vector>

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_save_checkpoint.h"

namespace ps2r::verify { class RunMeta; }
class MainMemory;
class Scratchpad;
namespace ps2r::app {
class SessionClock;

// Root composition for controller and durable game-save services. Reproduction
// files carry their owned state; they never roll back the game-save directory.
class InputSave {
public:
    using Checkpoint = ::title::InputSaveCheckpoint;
    using Memory = ::title::InputBindings::Memory;
    // Guest memory the owned entries read and stamp: main RAM in any segment
    // and the scratchpad (the title's main-thread stack lives there).
    static Memory RamAccess(MainMemory&, Scratchpad&);
    InputSave(uint32_t selection, Memory memory, ::title::SaveStorage::Config storage,
              std::function<uint64_t()> boundary, ::title::Input::MotorOutput motors);
    bool Install(const ::title::InputBindings::Installer&);
    // Register the clock first. A restored clock retains owner polling keys;
    // only an explicitly admitted clockless import rebases them.
    bool Register(savestate::SectionRegistry&, const SessionClock* clock = nullptr);
    bool Sample(uint64_t boundary, const ::title::Input::Sample&);
    bool NeedsSample(uint64_t boundary) const;
    void Pause(bool paused);
    bool CanCheckpoint(std::string* error) const;
    bool InputSelected() const { return selection_ & Checkpoint::kInput; }
    bool SavesSelected() const { return selection_ & Checkpoint::kSaveStorage; }
    uint32_t Selection() const { return selection_; }
    ::title::Input::Reading Current() const { return input_.Current(0, 0); }
    void Record(verify::RunMeta&) const;
    // Pure migration of an explicitly classified boot image. New marked
    // images never fall back to a legacy import or reset. The original blobs
    // are validated before replacing only approved legacy Input/Save sections.
    bool PrepareBoot(const std::vector<savestate::Blob>& original,
                     const Checkpoint::LegacyPolicy&, std::vector<savestate::Blob>& owned,
                     std::string& error) const;
    // Checks original metadata/sections before any migration is published.
    // clockless_import is true only for an explicitly approved unmarked file.
    using ImageCheck = std::function<bool(const savestate::FileMeta&,
        const std::vector<savestate::Blob>&, bool clockless_import, std::string&)>;
    bool PrepareBootFile(const std::string& path, const Checkpoint::LegacyPolicy&,
                         savestate::FileMeta&, std::vector<uint8_t>& image, std::string& error,
                         const ImageCheck&) const;

private:
    bool Validate(const std::vector<savestate::Blob>&, const SessionClock*, std::string* error) const;
    const uint32_t selection_;
    std::function<uint64_t()> boundary_;
    ::title::Input input_;
    ::title::SaveStorage storage_;
    ::title::InputBindings input_bindings_;
    ::title::SaveBindings save_bindings_;
    uint64_t samples_ = 0;
};

} // namespace ps2r::app
