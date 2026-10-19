#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"

namespace ps2r::verify { class RunMeta; }
namespace title {
// NTSC's complete forwarding MoviePlayer::Step contains Platform::Movie::Step
// inline. Preserve its queue/decode/feed/frame algorithm at this service
// boundary. Software picture delivery has no scratchpad stack to relocate.
class MovieStep {
public:
    static constexpr uint32_t kAddress = 0x002aea08;
    MovieStep(ps2r::ipu::GuestMemory&, MovieDecoder&, MovieGraphics&);
    bool Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected);
    bool CanCheckpoint(std::string* reason = nullptr) const;
    std::string Census() const;
    void Record(ps2r::verify::RunMeta&) const;

private:
    void Run(CpuContext&, uint32_t entry);
    [[noreturn]] void Fail(const char*);
    ps2r::ipu::GuestMemory& memory_;
    MovieDecoder& decoder_;
    MovieGraphics& pictures_;
    MovieDecoder::Body original_;
    bool installed_ = false, selected_ = false, active_ = false, failed_ = false;
    uint64_t original_calls_ = 0, steps_ = 0, advanced_ = 0, full_ = 0, finished_ = 0;
};
}  // namespace title
