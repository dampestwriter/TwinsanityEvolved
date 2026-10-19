#include <algorithm>
#include <cstdio>

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL %s\n", name); }
    };
    using title::StreamBuffers;
    using Part = StreamBuffers::MoviePart;
    StreamBuffers buffers(0x8003f);
    check(buffers.Available() == 0x80000 && buffers.Empty(), "real capacity aligned down");
    check(!buffers.Allocate(48, 4096, 0) && !buffers.Close(UINT32_MAX), "invalid slots fail");
    check(!buffers.Allocate(0, UINT32_MAX, 0) && buffers.Empty(), "overflow/exhaustion leave pool intact");
    check(buffers.Allocate(3, 0, 0x1c000), "paired child without separate staging allocation");
    check(buffers.CanPlay(3) && !buffers.Resize(3, 0x4000), "source empty-child resize failure");
    check(buffers.Used() == 0 && buffers.Inspect(3).handle == 0 && !buffers.Empty(), "zero child never invents address");
    check(buffers.Close(3) && buffers.Empty(), "zero child closes");
    uint32_t previous_handle = 0;
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        // SOUND_P8: actual bytes must return to the pool across repeat
        // open/free cycles; reporting a successful memory gate is not a fix.
        const auto baseline = buffers.Available();
        check(buffers.Allocate(2, 0x40000, 0x18000), "music parent allocated");
        const auto slot = buffers.Inspect(2);
        check(slot.handle && slot.handle != previous_handle && buffers.Used() == 0x40000,
              "fresh handle and real capacity charge");
        previous_handle = slot.handle;
        check(buffers.Allocate(2, 0x70000, 0x12345) && buffers.Inspect(2).handle == slot.handle &&
            buffers.Inspect(2).bytes == 0x40000 && buffers.Available() == baseline - 0x40000,
            "duplicate attach preserves owned allocation");
        check(!buffers.Resize(2, 1) && !buffers.Resize(2, 1023) && !buffers.Resize(2, UINT32_MAX),
              "invalid window size rejected without wrap");
        check(buffers.Resize(2, 1025) && buffers.Inspect(2).window == 1088, "source window rounding");
        check(buffers.Resize(2, 0x4000), "source music window");
        auto sound = buffers.BorrowMovie(slot.handle, Part::SoundRing);
        auto disc = buffers.BorrowMovie(slot.handle, Part::DiscStream);
        check(sound && disc && sound.sequence != disc.sequence && buffers.ActiveLoans() == 2,
              "Prepare and OpenStream own two distinct loans");
        auto sound_view = buffers.Resolve(sound);
        auto disc_view = buffers.Resolve(disc);
        check(sound_view && disc_view && sound_view.bytes == StreamBuffers::kSoundBytes &&
            disc_view.bytes == StreamBuffers::kDiscBytes &&
            sound_view.data == disc_view.data + StreamBuffers::kDiscBytes, "source nonoverlapping Movie ranges");
        if (sound_view && disc_view) {
            std::fill_n(sound_view.data, sound_view.bytes, uint8_t{0x53});
            std::fill_n(disc_view.data, disc_view.bytes, uint8_t{0xd1});
            check(sound_view.data[0] == 0x53 && sound_view.data[sound_view.bytes - 1] == 0x53 &&
                disc_view.data[disc_view.bytes - 1] == 0xd1, "both Movie users have real independent writable bytes");
        }
        check(!buffers.BorrowMovie(slot.handle, Part::SoundRing) &&
            !buffers.BorrowMovie(slot.handle, Part::DiscStream), "same part cannot double-borrow");
        check(!buffers.CanPlay(2) && !buffers.Resize(2, 0x8000) && !buffers.Reset(), "loans prevent reuse/reset");
        auto forged = sound;
        ++forged.offset;
        check(!buffers.Resolve(forged) && !buffers.ReturnMovie(forged), "altered range cannot resolve or return");
        ++forged.generation;
        check(!buffers.ReturnMovie(forged) && buffers.ActiveLoans() == 2, "altered generation leaves loans intact");
        check(buffers.Close(2) && buffers.Used() == 0x40000 && !buffers.CanPlay(2),
              "close defers freeing until Movie returns both");
        check(!buffers.BorrowMovie(slot.handle, Part::DiscStream), "closed allocation cannot gain new loan");
        check(buffers.Allocate(2, 0x40000, 0x18000) && buffers.Inspect(2).handle != slot.handle,
              "reopen has a different owner while old Movie finishes");
        check(buffers.Available() == 0 && !buffers.Allocate(4, 64, 0x20000), "exhaustion is a real failure");
        check(buffers.Resolve(sound) && buffers.Resolve(disc), "close/reopen does not dangle old views");
        check(buffers.ReturnMovie(sound) && buffers.Used() == 0x80000 && buffers.ActiveLoans() == 1,
              "first return cannot free shared allocation");
        check(!buffers.ReturnMovie(sound) && !buffers.Resolve(sound), "duplicate return/stale view fail");
        check(buffers.ReturnMovie(disc) && buffers.Used() == 0x40000 && buffers.CanPlay(2),
              "second return frees only retired allocation");
        check(!buffers.ReturnMovie(disc) && buffers.Inspect(2).handle != slot.handle,
              "stale return cannot release new owner");
        check(buffers.Close(2) && buffers.Available() == baseline && buffers.Empty(),
              "cycle returns exactly to baseline");
        check(buffers.Reset() && !buffers.ReturnMovie(sound), "reset does not revive an old lease");
    }
    check(buffers.Allocate(1, 1024, 0x18000), "small source buffer");
    check(!buffers.BorrowMovie(buffers.Inspect(1).handle, Part::SoundRing) &&
        !buffers.BorrowMovie(buffers.Inspect(1).handle, Part::DiscStream), "undersized lender rejected");
    check(!buffers.BorrowMovie(123, Part::SoundRing), "unknown nonzero lender never falls back");
    check(buffers.Close(1), "small buffer released");
    const auto fallback_sound = buffers.BorrowMovie(0, Part::SoundRing);
    const auto fallback_disc = buffers.BorrowMovie(0, Part::DiscStream);
    check(fallback_sound && fallback_disc && fallback_sound.handle != fallback_disc.handle &&
        fallback_sound.offset == 0 && buffers.Used() == StreamBuffers::kSoundBytes + StreamBuffers::kDiscBytes,
        "no lender allocates two actual scratch buffers");
    check(buffers.ReturnMovie(fallback_disc) && buffers.Used() == StreamBuffers::kSoundBytes,
          "scratch disc can return independently");
    check(buffers.ReturnMovie(fallback_sound) && buffers.Empty(), "scratch sound returns remaining capacity");
    StreamBuffers tiny(0x1000);
    check(!tiny.BorrowMovie(0, Part::SoundRing) && tiny.Empty(), "scratch exhaustion publishes no loan");
    std::printf("StreamBuffers: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
