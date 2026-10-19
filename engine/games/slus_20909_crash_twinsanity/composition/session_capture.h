#pragma once

#include <optional>
#include <string>

#include "app/host/session_checkpoint.h"
#include "app/host/session_runtime.h"
#include "engine/core/savestate/quiescence.h"

class GameBoundary;
class MtvuStack;
namespace title { class GameLibraries; class MovieStream; }
namespace ps2r {
class Config;
namespace ipu { class MpegApi; }
namespace app {
class GraphicsOutput;
class ServiceRestore;

// Composes the same capture conditions for automatic and interactive requests.
// All referenced providers outlive this owner and its joined writer.
// F5 (play) and `--set harness.save_at_frame=N` (harness) share Capture/Poll:
// the slot path (SavestateSlots), the fresh-boundary thread records, every
// registered owner's section, the atomic writer and the `.meta` sidecar.
class SessionCapture {
public:
    SessionCapture(const savestate::SectionRegistry& sections, GameBoundary& boundary,
                   const Config& config, ::title::GameLibraries& libraries,
                   ::title::MovieStream& movie, ipu::MpegApi& decoder, MtvuStack* mtvu,
                   GraphicsOutput& graphics, ServiceRestore& services);
    savestate::Quiescence& Conditions();
    void Bind(SessionRuntime::Hooks& hooks);
    // Game owner, inside the completed game-boundary callback. An empty path
    // is the configured slot (<save root>/savestates/slot<save.slot>.ps2st).
    // Waiting = an owner refused this boundary (detail names it); Working =
    // queued for the writer; Refused/Failed = terminal, detail names the owner.
    SessionCheckpoint::Result Capture(uint64_t request, uint64_t frame, std::string path = {});
    // Any host owner. The writer's terminal result; on success the `.meta`
    // sidecar (frame, build id, composition, sections) is written beside the slot.
    std::optional<SessionCheckpoint::Result> Poll();
    void Join();
    // The folder interactive F5 saves land in and the browser lists:
    // <save root>/savestates (SavestateSlots::Directory).
    static std::string ReproductionsDirectory(const Config& config);

private:
    const Config& config_;
    savestate::Quiescence conditions_;
    SessionCheckpoint checkpoint_;
};

}  // namespace app
}  // namespace ps2r
