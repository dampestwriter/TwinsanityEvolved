// test_movie.cpp -- movie.h over te::movie::MoviePlayer (the engine's ps2e_movie decoder + the clock + the present
// callback + the mixer's movie source), on SYNTHETIC program streams written into the generated fixture ISO (no game
// data): a sound-only PSS (two PCM tracks), a PSS with pictures encoded at test time by libavcodec's
// mpeg2video encoder (when the linked FFmpeg has it; else that half SKIPs), the skip toggle, a missing file.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include "audio_service.h"
#include "movie_player.h"
#include "ps2e_seam.h"
#include "te_graphics_hooks.h"
#include "te_services.h"
#include "te_test.h"
#include "te_test_iso.h"

namespace {
using te::movie::MoviePlayer;
using Bytes = std::vector<uint8_t>;

std::string g_iso;

// ---- a program-stream writer (the PSS the disc carries: MPEG-2 packs, video PES 0xE0, PCM in private_stream_1) ----
void Put(Bytes& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); }
void PutBe16(Bytes& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void PutLe32(Bytes& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void Append(Bytes& v, const Bytes& b) { v.insert(v.end(), b.begin(), b.end()); }

Bytes Pack() {
    Bytes v;
    Put(v, {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8});
    return v;
}
// MPEG-2 PES with a 5-byte PTS stand-in
Bytes Pes(uint8_t id, const uint8_t* payload, size_t n) {
    Bytes v;
    Put(v, {0x00, 0x00, 0x01, id});
    PutBe16(v, uint16_t(3 + 5 + n));
    Put(v, {0x80, 0x80, 0x05, 0x21, 0x00, 0x01, 0x00, 0x01});
    v.insert(v.end(), payload, payload + n);
    return v;
}
Bytes AudioPayload(int channel, const Bytes& body) {
    Bytes v = {0xFF, 0xA0, uint8_t(channel >> 8), uint8_t(channel)};
    Append(v, body);
    return v;
}
Bytes AdsHeader(uint32_t body) {
    Bytes h;
    Put(h, {'S', 'S', 'h', 'd'});
    PutLe32(h, 0x18); PutLe32(h, 1); PutLe32(h, 48000); PutLe32(h, 2); PutLe32(h, 0x200);
    PutLe32(h, 0xFFFFFFFFu); PutLe32(h, 0xFFFFFFFFu);
    Put(h, {'S', 'S', 'b', 'd'});
    PutLe32(h, body);
    return h;
}
// One planar 1024-byte pair: 256 left samples (base + i) then 256 right samples (base - i)
Bytes PlanarPair(int16_t base) {
    Bytes p;
    for (int i = 0; i < 256; ++i) { const uint16_t l = uint16_t(base + i); p.push_back(uint8_t(l)); p.push_back(uint8_t(l >> 8)); }
    for (int i = 0; i < 256; ++i) { const uint16_t r = uint16_t(base - i); p.push_back(uint8_t(r)); p.push_back(uint8_t(r >> 8)); }
    return p;
}

// A program stream: `video` (an MPEG-2 ES, may be empty) and two PCM tracks of `pairs` pairs each (channel 0 at base
// 1000.., channel 1 at base -5000..), interleaved pack by pack -- one pair of each track then one video slice, the
// slices sized so the video's last bytes (its sequence end code, which ends the stream) come after the last pair, as
// the disc's movies mux their sound ahead of their pictures -- then the program end
Bytes MakePss(const Bytes& video, int pairs) {
    Bytes s;
    const size_t slice = std::max<size_t>(64, (video.size() + size_t(pairs) - 1) / size_t(std::max(pairs, 1)));
    size_t voff = 0;
    const uint32_t body = uint32_t(pairs) * 1024;
    Append(s, Pack());
    { const Bytes a0 = AudioPayload(0, AdsHeader(body)), a1 = AudioPayload(1, AdsHeader(body));
      Append(s, Pes(0xBD, a0.data(), a0.size())); Append(s, Pes(0xBD, a1.data(), a1.size())); }
    int pair = 0;
    while (voff < video.size() || pair < pairs) {
        Append(s, Pack());
        if (pair < pairs) {
            const Bytes a0 = AudioPayload(0, PlanarPair(int16_t(1000 + pair))), a1 = AudioPayload(1, PlanarPair(int16_t(-5000 - pair)));
            Append(s, Pes(0xBD, a0.data(), a0.size()));
            Append(s, Pes(0xBD, a1.data(), a1.size()));
            ++pair;
        }
        if (voff < video.size()) {
            const size_t n = std::min(slice, video.size() - voff);
            Append(s, Pes(0xE0, video.data() + voff, n));
            voff += n;
        }
    }
    Put(s, {0x00, 0x00, 0x01, 0xB9});
    return s;
}

// An MPEG-2 video elementary stream of `frames` pictures, w x h at 25 fps, each a flat grey of 20 + 20 * n, ending
// with the sequence end code. Empty when the linked libavcodec has no mpeg2video encoder.
Bytes EncodeMpeg2(int w, int h, int frames) {
    Bytes es;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) return es;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = w; ctx->height = h;
    ctx->time_base = AVRational{1, 25};
    ctx->framerate = AVRational{25, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = 4;
    ctx->max_b_frames = 1;
    ctx->bit_rate = 400000;
    if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return es; }
    AVFrame* frame = av_frame_alloc();
    frame->format = ctx->pix_fmt; frame->width = w; frame->height = h;
    av_frame_get_buffer(frame, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(ctx, pkt) == 0) { es.insert(es.end(), pkt->data, pkt->data + pkt->size); av_packet_unref(pkt); }
    };
    for (int n = 0; n < frames; ++n) {
        av_frame_make_writable(frame);
        const uint8_t grey = uint8_t(20 + 20 * n);
        for (int y = 0; y < h; ++y) std::memset(frame->data[0] + y * frame->linesize[0], grey, size_t(w));
        for (int y = 0; y < h / 2; ++y) { std::memset(frame->data[1] + y * frame->linesize[1], 128, size_t(w / 2)); std::memset(frame->data[2] + y * frame->linesize[2], 128, size_t(w / 2)); }
        frame->pts = n;
        avcodec_send_frame(ctx, frame);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    Put(es, {0x00, 0x00, 0x01, 0xB7});
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return es;
}

constexpr uint32_t kPlayer = 0x00410000, kArea = 0x00300100, kPath = 0x00300000;
constexpr int kVideoFrames = 10;

void WriteArea(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t a[4] = {x, y, w, h};
    std::memcpy(te::GuestPointer(kArea, 16), a, 16);
}

void TestDueField() {
    // 25 fps at 60 Hz: a picture every 2.4 fields; 29.97 at 60 and 25 at 50: every second blank, as the PS2 presented
    const uint64_t due25[] = {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24};
    for (uint64_t p = 0; p < 11; ++p) CHECK_EQ(MoviePlayer::DueField(p, 25.0, 60), due25[p]);
    for (uint64_t p = 0; p < 20; ++p) CHECK_EQ(MoviePlayer::DueField(p, 29.97, 60), p * 2);
    for (uint64_t p = 0; p < 20; ++p) CHECK_EQ(MoviePlayer::DueField(p, 25.0, 50), p * 2);
    CHECK_EQ(MoviePlayer::DueField(5, 0.0, 60), 10u);   // the rate unknown: every second blank
}

void TestSkipAndMissing() {
    te::Settings s = te_test::DefaultSettings();
    s.iso_path = g_iso;
    s.skip_movies = true;
    te_test::Reset(s);
    auto& m = te::movie::Service();
    m.Reset();
    std::memset(te::GuestPointer(kPlayer, MoviePlayer::kPlayerSize), 0xAB, MoviePlayer::kPlayerSize);
    const uint32_t file = te_test::WriteString(kPath, "\\MOVIE_V.PSS;1");
    ps2e_movie_construct(kPlayer);   // takes the skip setting
    CHECK(m.SkipMovies());
    CHECK_EQ(te::GuestPointer(kPlayer, 1)[0], 0);
    CHECK_EQ(te::GuestPointer(kPlayer + MoviePlayer::kPlayerSize - 1, 1)[0], 0);
    ps2e_movie_begin_presenting(kPlayer, 1234);
    CHECK_EQ(ps2e_movie_open(kPlayer, file, 0, 512, 0x00100000), 1);
    CHECK(m.Inspect(kPlayer)->open);
    CHECK_EQ(m.Inspect(kPlayer)->present, 1234u);
    ps2e_movie_start_sound(kPlayer, 0.75f);
    const uint32_t calls = te_test::CallbacksCalled();
    const uint64_t boundaries = te::Services::Get().clock().BoundaryCount();
    ps2e_movie_wait_frame(kPlayer);   // returns at once: nothing is presented, no boundary passes
    CHECK_EQ(te_test::CallbacksCalled(), calls);
    CHECK_EQ(te::Services::Get().clock().BoundaryCount(), boundaries);
    WriteArea(0, 0, 512, 448);
    ps2e_movie_draw(kPlayer, kArea, 512, 416, 0);
    ps2e_movie_queue_picture(kPlayer);
    CHECK_EQ(ps2e_movie_step(kPlayer), 0);   // ended at once: the controller stops it this frame
    CHECK_EQ(m.Count().skipped, 1u);
    CHECK_EQ(m.Count().draws, 0u);
    ps2e_movie_close(kPlayer);
    CHECK(!m.Inspect(kPlayer)->open);
    CHECK_EQ(m.Count().closes, 1u);
    // The mixer's movie source is silent when nothing plays
    int16_t pcm[16];
    CHECK_EQ(m.Pop(pcm, 8), 0);
    CHECK(m.Gains()[0] == 0.f);

    // Not skipping: a file that is not on the disc is not played (Open false, counted), the game's Start then fails
    s.skip_movies = false;
    te_test::Reset(s);
    m.Reset();
    m.SetSkipMovies(false);
    m.Construct(kPlayer);
    m.BeginPresenting(kPlayer, 5);
    CHECK(!m.Open(kPlayer, "\\FMV\\NOSUCH.PSS;1", 0, 512, 0));
    CHECK_EQ(m.Count().open_failed, 1u);
    CHECK(!m.Inspect(kPlayer)->open);
    CHECK(!m.Step(kPlayer));
    m.Close(kPlayer);
    // An unknown player: refused, counted, no crash
    CHECK(!m.Open(0x00500000, "X", 0, 0, 0));
    CHECK(!m.Step(0x00500000));
    m.Close(0x00500000);
    CHECK(m.Count().unknown_player >= 3);
}

void TestSoundOnly() {
    te::Settings s = te_test::DefaultSettings();
    s.iso_path = g_iso;
    te_test::Reset(s);
    auto& m = te::movie::Service();
    m.Reset();
    m.SetSkipMovies(false);
    te::audio::Service().Reset();
    m.Construct(kPlayer);
    m.BeginPresenting(kPlayer, 9);
    // The second sound track (the story movies have one per language)
    CHECK(m.Open(kPlayer, "\\MOVIE_A.PSS;1", 1, 512, 0));
    const MoviePlayer::Player* p = m.Inspect(kPlayer);
    CHECK(p->open);
    CHECK_EQ(p->audio_channel, 1u);
    CHECK(p->queue.empty() && !p->decoded);   // no pictures in it
    // Silent until StartSound (the PS2 filled its ring, then started the SPU2)
    int16_t pcm[64 * 2];
    CHECK_EQ(m.Pop(pcm, 64), 0);
    CHECK(m.Gains()[0] == 0.f && m.Gains()[1] == 0.f);
    m.StartSound(kPlayer, 0.5f);
    CHECK(std::fabs(m.Gains()[0] - 0.5f) < 1e-6f && std::fabs(m.Gains()[1] - 0.5f) < 1e-6f);
    CHECK_EQ(m.Pop(pcm, 64), 64);
    CHECK(pcm[0] == -5000 && pcm[1] == -5000 && pcm[2] == -4999 && pcm[3] == -5001);   // channel 1, L R interleaved
    CHECK_EQ(m.Count().pcm_frames, 64u);
    CHECK_EQ(m.Count().pcm_silence_frames, 0u);
    // The mixer pops it as its movie source: one tick at 0.5 gain carries the samples
    std::vector<float> tick;
    te::audio::Service().MixTick(tick);
    CHECK_EQ(tick.size(), 1600u);   // 800 stereo frames at 60 Hz
    CHECK(std::fabs(tick[0] - (-4936.f / 32768.f) * 0.5f) < 1e-4f);   // frame 64 of channel 1 (-5000 + 64), half gain
    // No pictures: the movie has ended -- the controller stops it at its first Step
    CHECK(!m.Step(kPlayer));
    CHECK_EQ(m.Count().pictures_decoded, 0u);
    m.Close(kPlayer);
    CHECK_EQ(m.Pop(pcm, 8), 0);
    CHECK_EQ(m.Count().closes, 1u);
}

void TestVideo(bool have_video) {
    if (!have_video) {
        std::fprintf(stderr, "test_movie: SKIP the picture half (libavcodec has no mpeg2video encoder here)\n");
        return;
    }
    te::Settings s = te_test::DefaultSettings();
    s.iso_path = g_iso;
    te_test::Reset(s);
    auto& m = te::movie::Service();
    m.Reset();
    m.SetSkipMovies(false);
    te::audio::Service().Reset();
    te::Clock& clock = te::Services::Get().clock();
    const uint32_t calls0 = te_test::CallbacksCalled();
    std::memset(te::GuestPointer(kPlayer, MoviePlayer::kPlayerSize), 0xAB, MoviePlayer::kPlayerSize);
    m.Construct(kPlayer);
    m.BeginPresenting(kPlayer, 4321);
    // MoviePlayer::Start's order: BeginPresenting, Open, StartSound
    CHECK(m.Open(kPlayer, "\\MOVIE_V.PSS;1", 0, 64, 0));
    const MoviePlayer::Player* p = m.Inspect(kPlayer);
    CHECK(p->open);
    CHECK_EQ(p->queue.size(), 1u);    // the first picture queued for the screen
    CHECK(p->decoded != nullptr);     // the second decoded and waiting
    CHECK(p->queue.front() && p->queue.front()->width == 64 && p->queue.front()->height == 48);
    CHECK(std::fabs(p->fps - 25.0) < 1e-6);
    CHECK_EQ(m.Count().pictures_decoded, 2u);
    CHECK(p->decoder != nullptr);
    m.StartSound(kPlayer, 1.0f);
    {   // the sound track (channel 0) is buffered with the first pictures: the first pair's samples
        int16_t pcm[4];
        CHECK_EQ(m.Pop(pcm, 2), 2);
        CHECK(pcm[0] == 1000 && pcm[1] == 1000 && pcm[2] == 1001 && pcm[3] == 999);
    }

    // The game's frame while a movie plays: Update -> Step; render -> Draw; then WaitFrame (present callback)
    uint32_t steps_true = 0, presents = 0;
    uint64_t fields_before = clock.BoundaryCount();
    WriteArea(0, 0, 512, 448);
    for (int frame = 0; frame < 40; ++frame) {
        if (!m.Step(kPlayer)) break;
        ++steps_true;
        m.Draw(kPlayer, kArea, 64, 48, 0);
        const uint32_t calls = te_test::CallbacksCalled();
        const uint64_t b = clock.BoundaryCount();
        m.WaitFrame(kPlayer);
        CHECK_EQ(te_test::CallbacksCalled(), calls + 1);   // the present callback, once per WaitFrame
        CHECK_EQ(te_test::LastCallback(), 4321u);
        CHECK(clock.BoundaryCount() >= b + 1 && clock.BoundaryCount() <= b + 3);   // 1..3 fields per picture at 25 fps / 60 Hz
        ++presents;
        m.QueuePicture(kPlayer);   // what the game's PresentMovieFrame does inside the callback
    }
    // Every picture is queued once; Step says ended when the decoder is drained and no picture waits to be queued,
    // which is the Step after the last picture was queued (the PS2's Finished: the last queued picture is not shown)
    CHECK_EQ(steps_true, uint32_t(kVideoFrames - 1));
    CHECK_EQ(presents, uint32_t(kVideoFrames - 1));
    CHECK_EQ(m.Count().pictures_decoded, uint64_t(kVideoFrames));
    CHECK_EQ(m.Count().pictures_queued, uint64_t(kVideoFrames));
    CHECK_EQ(te_test::CallbacksCalled() - calls0, uint32_t(kVideoFrames - 1));
    // The fields the pictures took (the callback's boundary is counted by the player, not run here: no module), by
    // the movie's rate: 10 pictures at 25 fps = 24 fields at 60 Hz, less the first (due 0)
    const uint64_t fields = clock.BoundaryCount() - fields_before;
    std::fprintf(stderr, "test_movie: %u pictures took %llu run boundaries (+%u counted for the callbacks' presents)\n",
                 presents, (unsigned long long)fields, presents);
    CHECK(fields >= 9 && fields <= 22);
    CHECK_EQ(m.Count().presents, uint64_t(kVideoFrames - 1));
    // With a (headless) GL device every presented picture was uploaded and drawn from the second picture on; without
    // one nothing is uploaded and Draw records nothing (a draw needs the texture)
    if (te_graphics_device()) {
        CHECK_EQ(m.Count().pictures_uploaded, uint64_t(kVideoFrames - 1));
        CHECK_EQ(m.Count().draws, uint64_t(kVideoFrames - 2));
    } else {
        CHECK_EQ(m.Count().pictures_uploaded, 0u);
        CHECK_EQ(m.Count().draws, 0u);
    }
    // The sound track came with the pictures (channel 0) and the mixer's ticks (the clock's boundaries) popped it
    CHECK(m.Count().pcm_frames > 0);
    CHECK(!m.Step(kPlayer));
    m.Close(kPlayer);
    CHECK(!m.Inspect(kPlayer)->open);
    CHECK_EQ(m.Count().closes, 1u);
    // A second movie on the same player works the same way
    m.BeginPresenting(kPlayer, 4321);
    CHECK(m.Open(kPlayer, "\\MOVIE_V.PSS;1", 1, 64, 0));
    CHECK(m.Step(kPlayer));
    m.Close(kPlayer);
    CHECK_EQ(m.Count().closes, 2u);
}
}  // namespace

int main() {
    setenv("TE_GFX_HEADLESS", "1", 0);   // the picture texture needs the graphics device: a hidden window, not a visible one
    const Bytes video = EncodeMpeg2(64, 48, kVideoFrames);
    std::vector<uint8_t> elf(16);
    std::memcpy(elf.data(), "SLES_525.68-TEST", 16);
    g_iso = te_test::WriteTestIso({
        {"", "MOVIE_A.PSS", MakePss(Bytes{}, 24)},
        {"", "MOVIE_V.PSS", MakePss(video, 40)},
        {"", "SLES_525.68", elf},
    });
    if (g_iso.empty()) { std::fprintf(stderr, "cannot write the test ISO\n"); return 2; }
    if (const char* dump = std::getenv("TE_MOVIE_TEST_DUMP")) {   // the synthetic video PSS, for the engine's Ps2eMovie test
        const Bytes pss = MakePss(video, 40);
        if (FILE* f = std::fopen(dump, "wb")) { std::fwrite(pss.data(), 1, pss.size(), f); std::fclose(f); }
    }
    TestDueField();
    TestSkipAndMissing();
    TestSoundOnly();
    TestVideo(!video.empty());
    std::remove(g_iso.c_str());
    return te_test::Finish("test_movie");
}
