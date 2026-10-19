#include <algorithm>
#include <cstdio>

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
}
int main() {
    using title::StreamBuffers;
    using Part = StreamBuffers::MoviePart;
    StreamBuffers buffers(0x100000);
    CHECK(buffers.Allocate(2, 0x40000, 0x18000));
    CHECK(buffers.Allocate(3, 0, 0x1c000) && buffers.Resize(2, 1025));
    const auto handle = buffers.Inspect(2).handle;
    const auto sound = buffers.BorrowMovie(handle, Part::SoundRing);
    const auto disc = buffers.BorrowMovie(handle, Part::DiscStream);
    const auto scratch = buffers.BorrowMovie(0, Part::SoundRing);
    CHECK(sound && disc && scratch);
    auto sound_view = buffers.Resolve(sound), disc_view = buffers.Resolve(disc), scratch_view = buffers.Resolve(scratch);
    if (!sound_view || !disc_view || !scratch_view) return 1;
    std::fill_n(sound_view.data, sound_view.bytes, uint8_t{0x98});
    std::fill_n(disc_view.data, disc_view.bytes, uint8_t{0x76});
    std::fill_n(scratch_view.data, scratch_view.bytes, uint8_t{0x54});
    CHECK(buffers.Close(2)); // detached storage remains alive for both Movie users
    CHECK(buffers.Allocate(2, 0x40000, 0x20000));
    const auto reopened = buffers.Inspect(2).handle;
    CHECK(reopened != handle);
    StreamBuffers::Snapshot saved;
    CHECK(buffers.Capture(saved));
    const auto used = buffers.Used();
    StreamBuffers::RenewedLoans renewed;
    CHECK(!buffers.Restore(saved, renewed) && renewed.empty());
    CHECK(buffers.ReturnMovie(sound));
    CHECK(!buffers.Restore(saved, renewed) && buffers.Resolve(disc));
    CHECK(buffers.ReturnMovie(disc) && buffers.ReturnMovie(scratch));
    CHECK(buffers.Used() == 0x40000);
    CHECK(buffers.Restore(saved, renewed));
    CHECK(renewed.size() == 3 && buffers.Used() == used);
    CHECK(buffers.Inspect(2).handle == reopened && buffers.CanPlay(2));
    CHECK(buffers.Inspect(3).allocated && buffers.CanPlay(3) && buffers.Inspect(3).handle == 0);
    CHECK(!buffers.Resolve(sound) && !buffers.ReturnMovie(disc) && !buffers.ReturnMovie(scratch));
    const auto new_sound = renewed.at(sound.sequence), new_disc = renewed.at(disc.sequence);
    const auto new_scratch = renewed.at(scratch.sequence);
    CHECK(new_sound.generation > sound.generation && new_sound.handle == handle);
    sound_view = buffers.Resolve(new_sound);
    disc_view = buffers.Resolve(new_disc);
    scratch_view = buffers.Resolve(new_scratch);
    CHECK(sound_view && disc_view && scratch_view);
    if (sound_view && disc_view && scratch_view) {
        CHECK(std::all_of(sound_view.data, sound_view.data + sound_view.bytes, [](uint8_t v) { return v == 0x98; }));
        CHECK(std::all_of(disc_view.data, disc_view.data + disc_view.bytes, [](uint8_t v) { return v == 0x76; }));
        CHECK(std::all_of(scratch_view.data, scratch_view.data + scratch_view.bytes, [](uint8_t v) { return v == 0x54; }));
    }
    CHECK(buffers.ReturnMovie(new_disc) && buffers.Used() == used);
    CHECK(buffers.ReturnMovie(new_sound) && buffers.Used() == used - 0x40000);
    CHECK(buffers.ReturnMovie(new_scratch) && buffers.Used() == 0x40000);
    CHECK(buffers.Close(2) && buffers.Close(3) && buffers.Empty());

    // Corrupt owner/loan relationships cannot free or replace live storage.
    auto bad = saved;
    bad.capacity += 64;
    CHECK(!buffers.Restore(bad, renewed) && buffers.Empty());
    bad = saved; bad.storage.at(handle).loans = 1;
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.loans.at(sound.sequence).offset = 0;
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.loans.at(sound.sequence).generation = 0;
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.storage.at(reopened).owned = false;
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.slots[3] = bad.slots[2];
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.next_handle = 0;
    CHECK(!buffers.Validate(bad));
    bad = saved; bad.storage.at(handle).bytes.pop_back();
    CHECK(!buffers.Validate(bad));

    // Restoring an older save never rewinds handle allocation or lets an old
    // Movie token release the same saved sequence in a later restore.
    CHECK(buffers.Allocate(0, 4096, 0));
    const auto future_handle = buffers.Inspect(0).handle;
    CHECK(buffers.Close(0) && buffers.Restore(saved, renewed));
    CHECK(!buffers.ReturnMovie(new_sound) && !buffers.ReturnMovie(new_disc));
    for (const auto& entry : renewed) CHECK(buffers.ReturnMovie(entry.second));
    CHECK(buffers.Close(2) && buffers.Close(3));
    CHECK(buffers.Allocate(0, 4096, 0) && buffers.Inspect(0).handle > future_handle);
    CHECK(buffers.Close(0) && buffers.Reset() && buffers.Empty());
    std::printf("BufferState: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
