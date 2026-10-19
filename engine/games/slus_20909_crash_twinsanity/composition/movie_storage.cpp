#include "games/slus_20909_crash_twinsanity/composition/movie_storage.h"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "engine/cdvd/iso_fs.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/game_libraries.h"

namespace ps2r::app {
namespace {
const auto capture = [](uint32_t address) { return DispatchTable::Capture(address); };
const auto install = [](uint32_t address, DispatchTable::Body body) {
    return DispatchTable::Install(address, std::move(body));
};
title::MovieStream::BufferLoan Borrow(CpuContext& context, uint32_t lender,
                                     title::MovieStream::BufferPart part) {
    const auto token = title::MusicBuffers::BorrowMovie(context, lender,
        part == title::MovieStream::BufferPart::SoundRing ?
        title::StreamBuffers::MoviePart::SoundRing : title::StreamBuffers::MoviePart::DiscStream);
    if (!token) return {};
    const auto view = title::MusicBuffers::Resolve(token);
    // Even a failed Resolve must return its acquired token exactly once.
    return {token.handle, view.bytes, view.data, [token] { return title::MusicBuffers::ReturnMovie(token); }};
}
} // namespace
MovieStorage::MovieStorage(title::GameLibraries& libraries, title::MovieStream& movie,
                           IsoFs& files, bool selected)
    : libraries_(libraries), movie_(movie), files_(files), selected_(selected) {}
MovieStorage::~MovieStorage() {
    if (!selected_ || !installed_) return;
    // Supervisor teardown only. Stop software playback/readers before either
    // loan returns, then stop producers before releasing their backing pool.
    libraries_.ResetAfterRestore();
    movie_.ResetAfterRestore();
    if (!title::SoundControls::Reset()) std::terminate();
    title::MusicLibrary::Reset();
    if (!title::MusicBuffers::Reset() || !title::MusicBuffers::Install(capture, install, nullptr))
        std::terminate();
}
bool MovieStorage::Install() {
    if (installed_) return false;
    if (!selected_) {
        // Observe actual retained Movie file dispatch in the comparison arm.
        installed_ = movie_.InstallLookup(capture, install) == 4;
        return installed_;
    }
    if (!title::MusicLibrary::Enabled() || !title::SoundControls::Enabled()) {
        std::fprintf(stderr, "music_buffers requires music and movie_lifecycle selections\n");
        return false;
    }
    title::MovieStream::BufferServices providers{
        Borrow, [](CpuContext& context, uint32_t address) { DispatchTable::Invoke(address, context); }};
    const auto lookup = [this](std::string_view path, title::MovieStream::File& out) {
        if (!files_.IsOpen()) return title::MovieStream::LookupResult::Error;
        IsoFs::Entry file{};
        if (!files_.Find(std::string(path), file)) return title::MovieStream::LookupResult::Missing;
        out = {file.lba, file.size};
        return title::MovieStream::LookupResult::Found;
    };
    if (!libraries_.InstallMovieBuffers(movie_, std::move(providers), lookup)) {
        std::fprintf(stderr, "music_buffers requires installed Movie lifecycle and file consumers\n");
        return false;
    }
    installed_ = title::MusicBuffers::Install(capture, install, &storage_);
    return installed_;
}
void MovieStorage::Record(verify::RunMeta& meta) const {
    const auto count = title::MusicBuffers::Count();
    meta.Set("music_buffers_binding", selected_ ? "on" : "off");
    meta.Set("music_buffers", "installed=" + std::to_string(installed_) +
        ",allocations=" + std::to_string(count.allocations) + ",resizes=" + std::to_string(count.resizes) +
        ",closes=" + std::to_string(count.closes) + ",queries=" + std::to_string(count.queries) +
        ",sound_loans=" + std::to_string(count.sound_loans) + ",disc_loans=" + std::to_string(count.disc_loans) +
        ",returns=" + std::to_string(count.returns) + ",errors=" + std::to_string(count.errors) +
        ",fallback=" + std::to_string(count.fallback) + ",statuses=" + std::to_string(count.statuses) +
        ",used=" + std::to_string(storage_.Used()) + ",available=" + std::to_string(storage_.Available()) +
        ",active_loans=" + std::to_string(storage_.ActiveLoans()));
}
} // namespace ps2r::app
