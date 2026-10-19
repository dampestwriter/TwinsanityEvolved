#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"

namespace ps2r::verify { class RunMeta; }
namespace title {
// Complete player WaitFrame service. The shipped NEW registration already
// dispatches one game presentation after bounded refill; decoder delivery owns
// pacing. Do not add a second field wait or execute the dormant spin tail.
class MovieWait {
public:
    static constexpr uint32_t kAddress = 0x2af8f0, kTail = 0x2af8f4;
    MovieWait(ps2r::ipu::GuestMemory&, MovieDecoder&, MovieGraphics&, MovieDecoder::Body call);
    bool Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected);
    bool CanCheckpoint(std::string* reason = nullptr) const;
    std::string Census() const;
    void Record(ps2r::verify::RunMeta&) const;

private:
    void Run(CpuContext&, uint32_t entry);
    void Call(CpuContext&, uint32_t address, uint32_t caller, bool presenting);
    [[noreturn]] void Fail(const char*);
    ps2r::ipu::GuestMemory& memory_;
    MovieDecoder& decoder_;
    MovieGraphics& pictures_;
    MovieDecoder::Body call_, original_, tail_;
    bool installed_ = false, selected_ = false, active_ = false, failed_ = false;
    uint64_t original_calls_ = 0, tail_calls_ = 0, waits_ = 0, refills_ = 0, presents_ = 0;
};
} // namespace title
