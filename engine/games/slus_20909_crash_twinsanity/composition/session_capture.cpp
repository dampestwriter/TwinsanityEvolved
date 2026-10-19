#include "games/slus_20909_crash_twinsanity/composition/session_capture.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include "app/host/build_id.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "app/host/mtvu_stack.h"
#include "app/host/savestate_slots.h"
#include "games/slus_20909_crash_twinsanity/composition/service_restore.h"
#include "app/host/session_clock.h"
#include "engine/core/config/config.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/sched/ee_run_token.h"
#include "engine/ee/timing/game_boundary.h"
#include "engine/ipu/hle/mpeg_api.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "games/slus_20909_crash_twinsanity/hle/game_libraries.h"

namespace ps2r::app {

SessionCapture::SessionCapture(const savestate::SectionRegistry& sections, GameBoundary& boundary,
                               const Config& config, ::title::GameLibraries& libraries,
                               ::title::MovieStream& movie, ipu::MpegApi& decoder, MtvuStack* mtvu,
                               GraphicsOutput& graphics, ServiceRestore& services)
    : config_(config), checkpoint_(sections, {
        [] { return EeRunToken::Instance().CanCaptureCurrentThread(); },
        [this](std::string& error) {
            std::string owner, reason;
            if (conditions_.Check(&owner, &reason)) return true;
            error = owner + ": " + reason;
            return false;
        },
        [&boundary](SessionCheckpoint::Records& records, std::string& error) {
            return boundary.FreshThreads(records, error);
        },
        DispatchTable::CanResumeAt
    }) {
    conditions_.Register("movie_player", [&libraries](std::string* why) {
        return libraries.CanCheckpoint(why);
    });
    conditions_.Register("movie_file", [&movie](std::string* why) {
        if (movie.CanCheckpoint()) return true;
        if (why) *why = "Movie still owns its file session";
        return false;
    });
    conditions_.Register("fmv_active", [&decoder](std::string* why) {
        if (!decoder.FmvActive()) return true;
        if (why) *why = "FMV actively decoding: no savestate during FMV playback";
        return false;
    });
    if (mtvu) conditions_.Register("mtvu", [mtvu](std::string* why) { return mtvu->Quiesce(why); });
    conditions_.Register("graphics", [&graphics](std::string* why) { return graphics.CanCheckpoint(why); });
    conditions_.Register("services", [&services](std::string* why) { return services.CanCheckpoint(why); });
}

savestate::Quiescence& SessionCapture::Conditions() { return conditions_; }

SessionCheckpoint::Result SessionCapture::Capture(uint64_t request, uint64_t frame, std::string path) {
    savestate::FileMeta meta;
    std::string frame_error;
    if (!SessionClock::SetFrame(meta, frame, frame_error))
        return {request, SessionControls::Phase::Refused, std::move(path), frame_error, 0};
    meta.wall_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (path.empty()) {
        const std::filesystem::path directory = SavestateSlots::Directory(config_);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return {request, SessionControls::Phase::Failed, {}, "savestate directory " + directory.string() +
                ": " + error.message(), 0};
        path = SavestateSlots::SlotPath(directory.string(), config_.save.slot);
    }
    return checkpoint_.Capture(request, std::move(path), meta);
}

std::optional<SessionCheckpoint::Result> SessionCapture::Poll() {
    auto result = checkpoint_.Poll();
    if (!result || result->phase != SessionControls::Phase::Succeeded) return result;
    SavestateSlots::Meta meta;
    meta.frame = result->frame;
    meta.wall_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    meta.build_id = g_build_id;
    meta.composition = SavestateSlots::Composition(config_);
    std::string error;
    if (!SavestateSlots::WriteMeta(result->path, std::move(meta), &error)) {
        result->phase = SessionControls::Phase::Failed;
        result->detail = "savestate written but its .meta sidecar failed: " + error;
        return result;
    }
    result->detail = "Savestate written: " + result->path + " (frame " + std::to_string(result->frame) +
                     ", sidecar " + SavestateSlots::MetaPath(result->path) + ")";
    return result;
}

void SessionCapture::Bind(SessionRuntime::Hooks& hooks) {
    hooks.capture = [this](const SessionControls::Request& request, uint64_t frame) {
        const auto result = Capture(request.id, frame, request.path);
        return SessionRuntime::CaptureResult{result.phase, result.detail};
    };
    hooks.poll_capture = [this]() -> std::optional<SessionRuntime::CaptureCompletion> {
        const auto result = Poll();
        if (!result) return {};
        return SessionRuntime::CaptureCompletion{
            result->request, {result->phase, result->detail}, result->path};
    };
}

void SessionCapture::Join() { checkpoint_.Join(); }

std::string SessionCapture::ReproductionsDirectory(const Config& config) {
    return SavestateSlots::Directory(config);
}

}  // namespace ps2r::app
