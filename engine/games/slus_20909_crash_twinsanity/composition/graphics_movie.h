#pragma once

#include <string>

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_step.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_start.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_wait.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stop.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"

namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {

// The movie player supplies picture/lifecycle/audio; the graphics output owns these ordered hooks.
// Source presentation wrappers preserve the complete source call and cadence
// while the remaining frame transport is retired separately.
class GraphicsMovie {
public:
    static constexpr uint32_t kInterruptPresent = 0x1a00d0, kPresent = 0x1a0150;
    using Frame = ::title::FrameEnvironment::SceneFrame;
    struct Hooks {
        bool selected = false;
        bool step = false;
        bool start = false;
        bool wait = false;
        bool stop = false;
        ::title::MovieDecoder::Body call;
        std::function<void(CpuContext&, ipu::DecodedPicture&)> picture;
        std::function<bool(Frame&)> submitted;
        std::function<bool(const Frame&)> select;
        std::function<bool()> clear;
    };
    GraphicsMovie(ipu::GuestMemory&, ::title::MovieDecoder&, Hooks,
                  ::title::MovieGraphics::OutputServices);
    ~GraphicsMovie();
    // Pictures, Step and the two presentation transports. A refusal names its
    // reason (the root prints it; exit 2 is never silent).
    bool Install(const ::title::MovieDecoder::Capture&, const ::title::MovieDecoder::Installer&,
                 std::string* reason = nullptr);
    // Start/WaitFrame/Stop. Their selected arms require the complete Movie
    // service mask (MovieDecoder::Services() == 127), which the root completes
    // only after MovieStorage composes the two buffer loans: install after it.
    // Both arms install here (the control arm counts the actual bodies).
    bool InstallPlayer(const ::title::MovieDecoder::Capture&, const ::title::MovieDecoder::Installer&,
                       std::string* reason = nullptr);
    static std::string MissingServices(uint32_t mask);
    // The session's selected display ratio for the picture placement (A1).
    void SetAspectSource(std::function<double()> ratio) { movie_.SetAspectSource(std::move(ratio)); }
    void Record(verify::RunMeta&) const;
    bool CanCheckpoint(std::string* reason) const;

private:
    void Dispatch(size_t, CpuContext&, uint32_t);
    Hooks hooks_;
    ::title::MovieDecoder& decoder_;
    ::title::MovieGraphics movie_;
    ::title::MovieStep step_;
    ::title::MovieStart start_;
    ::title::MovieWait wait_;
    ::title::MovieStop stop_;
    std::array<::title::MovieDecoder::Body, 2> originals_;
    std::array<uint64_t, 2> transport_calls_{};
    std::array<uint64_t, 2> interior_resumes_{}; // restored sessions resuming inside the transport
    uint64_t release_refused_ = 0;
    Frame presenting_;
    bool installed_ = false, player_installed_ = false, presenting = false, failed_ = false;
};

}  // namespace ps2r::app
