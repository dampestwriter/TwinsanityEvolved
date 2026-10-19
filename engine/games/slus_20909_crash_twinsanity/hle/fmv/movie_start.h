#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"

namespace ps2r::verify { class RunMeta; }
namespace title {
// Checked NTSC Start composition: keep the source's game/service decisions,
// prime owned software pictures without a copied scratchpad stack.
class MovieStart {
public:
    static constexpr uint32_t kAddress = 0x002ae318;
    MovieStart(ps2r::ipu::GuestMemory&, MovieDecoder&, MovieGraphics&, MovieDecoder::Body call);
    bool Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected,
                 bool direct_presentation = false);
    bool CanCheckpoint(std::string* reason = nullptr) const;
    std::string Census() const;
    void Record(ps2r::verify::RunMeta&) const;

private:
    void Run(CpuContext&, uint32_t entry);
    bool Open(CpuContext&, uint32_t player);
    uint32_t Call(CpuContext&, uint32_t address, uint32_t caller,
                  std::initializer_list<uint32_t> arguments);
    [[noreturn]] void Fail(const char*);
    ps2r::ipu::GuestMemory& memory_;
    MovieDecoder& decoder_;
    MovieGraphics& pictures_;
    MovieDecoder::Body call_, original_;
    bool installed_ = false, selected_ = false, active_ = false, failed_ = false;
    bool direct_presentation_ = false;
    uint64_t original_calls_ = 0, starts_ = 0, opened_ = 0, open_failed_ = 0, primed_ = 0;
};
}  // namespace title
