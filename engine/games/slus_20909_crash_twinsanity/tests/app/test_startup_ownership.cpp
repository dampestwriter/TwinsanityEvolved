#include <cstdio>
#include <string>

#include "games/slus_20909_crash_twinsanity/composition/startup_ownership.h"
#include "engine/core/verify/run_meta.h"

// The startup owner's reading of title::ServiceStartup. Pins: the default
// composition retains everything and names every retained module and consumer
// in the live-restore refusal (never the generic sentence); run.meta carries
// the selection-aware census; a complete release empties the refusal and
// reports both startup halves owned; partial release reports each half alone.
int main() {
    using S = title::ServiceStartup;
    using ps2r::app::StartupOwnership;
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL StartupOwnership %s\n", label); }
    };
    const auto has = [](const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; };

    const StartupOwnership retained(S::Inspect(S::Inputs{}));
    check(!retained.Owned() && !retained.StreamOwned() && !retained.SoundOwned(), "default retains both halves");
    const std::string refusal = retained.Refusal();
    check(has(refusal, "Live restore requires service-only startup; retained: STREAM.IRX+CDVDSTM.IRX+LIBSD.IRX+SDRDRV.IRX+IOP heap."),
          "refusal names every retained module");
    check(!has(refusal, "STREAM still owns running state"), "refusal is the contract's list, not the generic sentence");
    for (const auto& consumer : retained.Contract().consumers)
        check(has(refusal, consumer.what), "refusal names each consumer with its root");
    check(has(refusal, "0x1e7f80") && has(refusal, "0x2c4c30") && has(refusal, "filestream.cpp:634"),
          "refusal carries the title roots of the three unowned phases");
    check(retained.Census() == "stream=retained,sound=retained,retained=STREAM.IRX+CDVDSTM.IRX+LIBSD.IRX+SDRDRV.IRX+IOP heap,consumers=" +
          std::to_string(retained.Contract().consumers.size()), "census is the selection-aware line");
    ps2r::verify::RunMeta meta("", {});
    retained.Record(meta);
    check(has(meta.Render(), ("service_startup=" + retained.Census()).c_str()), "run.meta service_startup= is the census");

    S::Inputs all;
    all.stream_files = all.sound_banks = all.sound_controls = all.music = all.music_buffers = true;
    all.disc_files = all.output = all.movie_pcm = all.movie_cursor = true;
    all.transport = all.effects = all.bank_residency = true;
    const StartupOwnership released(S::Inspect(all));
    check(released.Owned() && released.StreamOwned() && released.SoundOwned(), "complete ownership releases both halves");
    check(released.Refusal().empty(), "no refusal once startup is service-only");
    check(released.Census() == "stream=native,sound=native,retained=none,consumers=0", "owned census");

    S::Inputs stream_only = all;
    stream_only.effects = false;
    const StartupOwnership sound_retained(S::Inspect(stream_only));
    check(sound_retained.StreamOwned() && !sound_retained.SoundOwned() && !sound_retained.Owned(), "LIBSD retained by M5 effects alone");
    check(has(sound_retained.Refusal(), "retained: LIBSD.IRX+SDRDRV.IRX.") && !has(sound_retained.Refusal(), "STREAM.IRX:"),
          "partial refusal names only the retained half");
    check(has(sound_retained.Census(), "stream=native,sound=retained,retained=LIBSD.IRX+SDRDRV.IRX,consumers=2"), "partial census");

    S::Inputs sound_only = all;
    sound_only.transport = false;
    const StartupOwnership stream_retained(S::Inspect(sound_only));
    check(!stream_retained.StreamOwned() && stream_retained.SoundOwned(), "STREAM retained by M4 transport alone");
    check(has(stream_retained.Refusal(), "retained: STREAM.IRX+CDVDSTM.IRX."), "CDVDSTM follows STREAM in the refusal");

    std::printf("StartupOwnership: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
