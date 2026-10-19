#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"

namespace ps2r::verify { class RunMeta; }
namespace title {
// Complete Stop: balanced Movie services and the unchanged game renderer
// recovery sequence. Paired selected Start/Wait own presentation directly;
// no Movie VBlank handler is installed or completed by this owner.
class MovieStop {
public:
    static constexpr uint32_t kAddress = 0x2ae658, kCallback = 0x2af568;
    MovieStop(ps2r::ipu::GuestMemory&, MovieDecoder&, MovieGraphics&, MovieDecoder::Body call);
    bool Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected);
    bool CanCheckpoint(std::string* reason = nullptr) const;
    std::string Census() const;
    void Record(ps2r::verify::RunMeta&) const;

private:
    void Run(CpuContext&, uint32_t entry);
    void Close(CpuContext&, uint32_t player);
    void Recover(CpuContext&, uint32_t player, uint32_t renderer, uint32_t target);
    void Call(CpuContext&, uint32_t address, uint32_t caller, std::initializer_list<uint32_t> args);
    void Virtual(CpuContext&, uint32_t object, uint32_t table_offset, uint32_t slot,
                 uint32_t caller, std::initializer_list<uint32_t> args);
    void CheckVirtual(uint32_t object, uint32_t table_offset, uint32_t slot);
    [[noreturn]] void Fail(const char*);
    ps2r::ipu::GuestMemory& memory_;
    MovieDecoder& decoder_;
    MovieGraphics& pictures_;
    MovieDecoder::Capture capture_;
    MovieDecoder::Body call_, original_, callback_;
    bool installed_ = false, selected_ = false, active_ = false, failed_ = false;
    uint64_t original_calls_ = 0, callback_calls_ = 0, stops_ = 0, recoveries_ = 0;
};
} // namespace title
