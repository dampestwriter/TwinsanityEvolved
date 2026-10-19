#include <cstdio>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_startup.h"

// The startup owner's contract: a module is released only when every
// consumer named for it has an owner. Pins: nothing is released by default;
// each input releases exactly the consumers it names; the two "not yet owned"
// owners (StreamTransport, SoundEffects, the owned bank lease) keep
// STREAM.IRX / LIBSD.IRX / SDRDRV.IRX / the heap retained while uninstalled
// even with every library selected; and every owner field comes from the
// actual installed owners, not from inputs.
int main() {
    using S = title::ServiceStartup;
    using M = S::Module;
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL ServiceStartup %s\n", label); }
    };
    const auto count = [](const S::Report& r, M module) {
        int n = 0;
        for (const auto& c : r.consumers) n += c.module == module;
        return n;
    };

    const S::Report none = S::Inspect(S::Inputs{});
    for (size_t i = 0; i < S::kModules; ++i) check(none.retained[i], "default selection retains every module");
    check(!none.StreamStartupOwned() && !none.SoundStartupOwned(), "default releases no startup");
    check(count(none, M::StreamIrx) == 6 && count(none, M::Cdvdstm) == 2 && count(none, M::Libsd) == 5 &&
          count(none, M::Sdrdrv) == 3 && count(none, M::IopHeap) == 2, "every consumer is named once");
    for (const auto& c : none.consumers) check(c.what && std::strlen(c.what) > 10, "each consumer names its root");

    S::Inputs all;
    all.stream_files = all.sound_banks = all.sound_controls = all.music = all.music_buffers = true;
    all.disc_files = all.output = all.movie_pcm = all.movie_cursor = true;
    const S::Report libraries = S::Inspect(all);
    check(libraries.Retained(M::StreamIrx) && count(libraries, M::StreamIrx) == 1 &&
          std::strstr(libraries.consumers.front().what, "M4") != nullptr,
          "all libraries selected: STREAM.IRX is retained by the M4 transport alone");
    check(libraries.Retained(M::Cdvdstm) && count(libraries, M::Cdvdstm) == 1, "CDVDSTM follows STREAM.IRX");
    check(libraries.Retained(M::Libsd) && count(libraries, M::Libsd) == 1, "LIBSD retained by M5 effects alone");
    check(libraries.Retained(M::Sdrdrv) && count(libraries, M::Sdrdrv) == 2, "SDRDRV retained by libsd and residency");
    check(libraries.Retained(M::IopHeap) && count(libraries, M::IopHeap) == 1, "heap retained by bank residency alone");
    check(!libraries.StreamStartupOwned() && !libraries.SoundStartupOwned(), "selection alone never releases startup");

    S::Inputs future = all;
    future.transport = future.effects = future.bank_residency = true;
    const S::Report released = S::Inspect(future);
    for (size_t i = 0; i < S::kModules; ++i) check(!released.retained[i], "every named owner present releases the module");
    check(released.consumers.empty() && released.StreamStartupOwned() && released.SoundStartupOwned(),
          "complete ownership releases both startups");

    S::Inputs partial = future;
    partial.movie_cursor = false;
    check(S::Inspect(partial).Retained(M::Cdvdstm) && !S::Inspect(partial).Retained(M::StreamIrx),
          "the Movie cursor alone keeps CDVDSTM without STREAM.IRX");
    partial = future; partial.output = false;
    check(S::Inspect(partial).Retained(M::Libsd) && S::Inspect(partial).Retained(M::Sdrdrv) &&
          !S::Inspect(partial).Retained(M::IopHeap), "the common Mixer keeps libsd/sdr, not the heap");
    partial = future; partial.music_buffers = false;
    check(S::Inspect(partial).Retained(M::IopHeap) && S::Inspect(partial).StreamStartupOwned() == false &&
          S::Inspect(partial).SoundStartupOwned(), "Movie buffers keep only the heap");

    // Library fields are read from the installed owners: nothing is installed
    // here, so every library field is false regardless of what the root claims.
    const S::Inputs current = S::Current(future);
    check(!current.stream_files && !current.sound_banks && !current.sound_controls && !current.music &&
          !current.music_buffers && !current.transport && !current.effects && !current.bank_residency &&
          current.output && current.disc_files, "Current reads owners (libraries, transport, effects, residency), keeps root fields");
    check(S::Inspect(current).Retained(M::StreamIrx) && S::Inspect(current).Retained(M::Libsd) &&
          S::Inspect(current).Retained(M::IopHeap), "uninstalled owners retain their modules");
    check(std::strcmp(S::Name(M::StreamIrx), "STREAM.IRX") == 0 && std::strcmp(S::Name(M::IopHeap), "IOP heap") == 0,
          "module names");

    std::printf("ServiceStartup %d checks %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
