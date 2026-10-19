// tests/ipu/test_ps2e_movie.cpp -- ctest Ps2eMovie: the C movie-decoder surface (engine/platform/ps2e/ps2e_movie.h)
// over a synthetic program stream: two PCM sound tracks (channels 0 and 1) with their ADS headers, a program end,
// chunked feeding, channel selection, the end states and the counters. The video decoder is exercised on a real
// PSS file only when one is named on the command line (`test_ps2e_movie <file.pss>`; the engine tree carries no game
// data and the tree's one getenv site is ConfigLoader's -- ctest CoreSingleGetenvSite): then the first pictures must
// decode at the movie's size with a sequence-header frame rate, and the sound track must come with them.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/platform/ps2e/ps2e_movie.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::printf("FAIL: %s (line %d): %s\n", msg, __LINE__, #cond);   \
        }                                                                    \
    } while (0)

using Bytes = std::vector<uint8_t>;

static void Put(Bytes& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); }
static void PutBe16(Bytes& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
static void PutLe32(Bytes& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static Bytes Cat(const std::vector<Bytes>& parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// MPEG-2 pack header, 14 bytes
static Bytes Pack() {
    Bytes v;
    Put(v, {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8});
    return v;
}
// MPEG-2 PES 0xBD with a 5-byte PTS: 00 00 01 BD len 0x80 0x80 5 <pts> <payload>
static Bytes AudioPes(const Bytes& payload) {
    Bytes v;
    Put(v, {0x00, 0x00, 0x01, 0xBD});
    PutBe16(v, uint16_t(3 + 5 + payload.size()));
    Put(v, {0x80, 0x80, 0x05, 0x21, 0x00, 0x01, 0x00, 0x01});
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}
static Bytes Prefix(int channel) { return {0xFF, 0xA0, uint8_t(channel >> 8), uint8_t(channel)}; }
// 'SShd' 0x18 {PCM16LE, 48000, 2, 0x200, -1, -1} 'SSbd' body
static Bytes AdsHeader(uint32_t body) {
    Bytes h;
    Put(h, {'S', 'S', 'h', 'd'});
    PutLe32(h, 0x18);
    PutLe32(h, 1);
    PutLe32(h, 48000);
    PutLe32(h, 2);
    PutLe32(h, 0x200);
    PutLe32(h, 0xFFFFFFFFu);
    PutLe32(h, 0xFFFFFFFFu);
    Put(h, {'S', 'S', 'b', 'd'});
    PutLe32(h, body);
    return h;
}
// One planar pair: 256 left samples (base + i) then 256 right samples (base - i)
static Bytes PlanarPair(int16_t base) {
    Bytes p;
    for (int i = 0; i < 256; ++i) { const uint16_t l = uint16_t(base + i); p.push_back(uint8_t(l)); p.push_back(uint8_t(l >> 8)); }
    for (int i = 0; i < 256; ++i) { const uint16_t r = uint16_t(base - i); p.push_back(uint8_t(r)); p.push_back(uint8_t(r >> 8)); }
    return p;
}

// Two sound tracks, 2 pairs each, interleaved per pack, then the program end
static Bytes TwoTrackStream() {
    return Cat({Pack(), AudioPes(Cat({Prefix(0), AdsHeader(2048)})), AudioPes(Cat({Prefix(1), AdsHeader(2048)})),
                Pack(), AudioPes(Cat({Prefix(0), PlanarPair(1000)})), AudioPes(Cat({Prefix(1), PlanarPair(-5000)})),
                Pack(), AudioPes(Cat({Prefix(0), PlanarPair(2000)})), AudioPes(Cat({Prefix(1), PlanarPair(-6000)})),
                Bytes{0x00, 0x00, 0x01, 0xB9}});
}

static void TestChannelOne() {
    ps2e_movie_config cfg{};
    cfg.audio_channel = 1;
    ps2e_movie* m = ps2e_movie_create(&cfg);
    CHECK(m != nullptr, "create");
    CHECK(!ps2e_movie_has_audio(m) && ps2e_movie_pcm_queued(m) == 0, "fresh: no audio yet");
    CHECK(!ps2e_movie_input_ended(m) && !ps2e_movie_ended(m), "fresh: not ended");
    const Bytes s = TwoTrackStream();
    // fed in odd-sized chunks: packets straddle the chunks
    size_t off = 0;
    int ready = 0;
    while (off < s.size()) {
        const size_t n = std::min<size_t>(37, s.size() - off);
        ready = ps2e_movie_feed(m, s.data() + off, n);
        off += n;
    }
    CHECK(ready == 0 && ps2e_movie_pictures_ready(m) == 0, "audio-only: no pictures");
    CHECK(ps2e_movie_has_audio(m), "channel 1's header accepted");
    CHECK(!ps2e_movie_audio_disabled(m), "not disabled by channel 0's header");
    CHECK(ps2e_movie_pcm_queued(m) == 512, "two pairs of channel 1 = 512 frames");
    CHECK(ps2e_movie_input_ended(m), "the program end ends the input");
    CHECK(ps2e_movie_ended(m), "ended: input ended, no pictures (nothing to flush)");
    int16_t out[300 * 2];
    uint32_t got = ps2e_movie_pcm_read(m, out, 300);
    CHECK(got == 300, "read 300 of 512");
    CHECK(out[0] == -5000 && out[1] == -5000 && out[2] == -4999 && out[3] == -5001, "channel 1's samples, interleaved L R");
    CHECK(out[2 * 256] == -6000 && out[2 * 256 + 1] == -6000, "the second pair follows the first");
    got = ps2e_movie_pcm_read(m, out, 300);
    CHECK(got == 212 && ps2e_movie_pcm_queued(m) == 0, "the rest, then empty");
    CHECK(ps2e_movie_pcm_read(m, out, 10) == 0, "empty reads 0");
    ps2e_movie_counters c{};
    ps2e_movie_counts(m, &c);
    CHECK(c.bytes_fed == s.size(), "bytes_fed");
    CHECK(c.packs == 3, "three packs");
    CHECK(c.audio_payloads == 6 && c.audio_skipped == 3, "six audio payloads, channel 0's three skipped");
    CHECK(c.pcm_frames_pushed == 512 && c.pcm_frames_read == 512, "pcm counters");
    CHECK(c.video_es_bytes == 0 && c.pictures_decoded == 0 && c.pictures_taken == 0, "no video");
    ps2e_movie_picture pic{};
    CHECK(ps2e_movie_take_picture(m, &pic) == 0, "take on an empty queue is 0");
    ps2e_movie_destroy(m);
}

static void TestAnyChannelAndEndInput() {
    ps2e_movie* m = ps2e_movie_create(nullptr);   // the defaults: every channel
    const Bytes s = TwoTrackStream();
    const Bytes without_end(s.begin(), s.end() - 4);
    ps2e_movie_feed(m, without_end.data(), without_end.size());
    CHECK(ps2e_movie_has_audio(m), "any: a header accepted");
    // Every channel's payload counts as body (the single-track path's behaviour): a two-track stream mixes the
    // tracks' bytes -- which is why a producer selects the channel for such a movie
    CHECK(ps2e_movie_pcm_queued(m) >= 512, "any: channel 0's header, every later payload as body");
    CHECK(!ps2e_movie_input_ended(m), "no end code yet");
    ps2e_movie_end_input(m);
    CHECK(ps2e_movie_input_ended(m) && ps2e_movie_ended(m), "end_input ends it");
    ps2e_movie_counters c{};
    ps2e_movie_counts(m, &c);
    CHECK(c.audio_skipped == 0, "any: nothing skipped");
    ps2e_movie_destroy(m);
    ps2e_movie_destroy(nullptr);   // tolerated
    CHECK(ps2e_movie_feed(nullptr, "x", 1) == 0 && ps2e_movie_pcm_queued(nullptr) == 0, "null movie: zeros");
}

// A real movie when the command line names one: the first pictures decode, the sound comes along
static int TestRealFile(const char* path) {
    if (!path || !*path) {
        std::printf("SKIP: no .pss file named on the command line (the video decoder half needs one)\n");
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("FAIL: cannot read %s\n", path);
        return 1;
    }
    ps2e_movie_config cfg{};
    cfg.audio_channel = 0;
    cfg.max_pictures = 3;
    ps2e_movie* m = ps2e_movie_create(&cfg);
    std::vector<char> chunk(128 * 1024);
    int pictures = 0, max_ready = 0;
    uint32_t pcm_at_first_picture = 0;
    ps2e_movie_picture first{};
    std::vector<uint8_t> first_pixels;
    int w = 0, h = 0;
    while (pictures < 12 && f) {
        f.read(chunk.data(), std::streamsize(chunk.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        int ready = ps2e_movie_feed(m, chunk.data(), size_t(n));
        if (ready > max_ready) max_ready = ready;
        ps2e_movie_picture pic{};
        while (ps2e_movie_take_picture(m, &pic)) {
            if (pictures == 0) {
                first = pic;
                first_pixels.assign(pic.rgba8, pic.rgba8 + size_t(pic.width) * pic.height * 4);
                pcm_at_first_picture = ps2e_movie_pcm_queued(m);
                w = pic.width; h = pic.height;
            } else {
                CHECK(pic.width == w && pic.height == h, "every picture has the movie's size");
            }
            ++pictures;
        }
    }
    CHECK(pictures >= 12, "12 pictures decoded from the file's head");
    CHECK(max_ready <= 3, "max_pictures bounds the pictures waiting");
    CHECK(w >= 64 && w <= 1920 && h >= 64 && h <= 1088, "a plausible picture size");
    CHECK(ps2e_movie_fps(m) > 10.0 && ps2e_movie_fps(m) < 61.0, "the sequence header's frame rate");
    CHECK(ps2e_movie_has_audio(m), "the sound track's header was accepted");
    CHECK(ps2e_movie_pcm_queued(m) > 0, "PCM queued alongside the pictures");
    uint64_t sum = 0;
    for (size_t i = 0; i + 3 < first_pixels.size(); i += 4) sum += first_pixels[i] + first_pixels[i + 1] + first_pixels[i + 2];
    bool alpha_opaque = true;
    for (size_t i = 3; i < first_pixels.size(); i += 4) alpha_opaque &= first_pixels[i] == 0xFF;
    CHECK(alpha_opaque, "RGBA8 with opaque alpha");
    ps2e_movie_counters c{};
    ps2e_movie_counts(m, &c);
    CHECK(c.pictures_taken == uint64_t(pictures) && c.pictures_decoded >= c.pictures_taken, "picture counters");
    std::printf("real file: %s -> %dx%d @ %.3f fps, %d pictures (first mean %.1f), pcm queued %u at the first picture, %llu now; decoder %s\n",
                path, w, h, ps2e_movie_fps(m), pictures, first_pixels.empty() ? 0.0 : double(sum) / double(first_pixels.size() / 4 * 3),
                pcm_at_first_picture, (unsigned long long)ps2e_movie_pcm_queued(m), ps2e_movie_decoder_version());
    ps2e_movie_destroy(m);
    return 0;
}

int main(int argc, char** argv) {
    TestChannelOne();
    TestAnyChannelAndEndInput();
    CHECK(ps2e_movie_decoder_version() && std::strlen(ps2e_movie_decoder_version()) >= 5, "decoder version string");
    TestRealFile(argc > 1 ? argv[1] : nullptr);
    std::printf("ps2e_movie: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
