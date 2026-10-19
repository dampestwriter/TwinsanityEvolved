// movie_player.cpp -- see movie_player.h. Also the ps2e_movie_* bodies (movie.h's nine imports).
#include "movie_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio_service.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/ps2e_movie.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_graphics_hooks.h"
#include "te_host.h"
#include "te_services.h"
#include "te_weak.h"

// A host executable with neither the runner nor the test harness (the graphics self-test) has no module to call back
// into: the weak default. The runner's and the harness's definitions are strong (te_weak.h: COFF spells it /alternatename).
TE_WEAK_DEFAULT(int, te_call_void_callback, (uint32_t), { return 0; })

namespace te::movie {

namespace {
bool Trace() {
    static const bool on = [] { const char* v = std::getenv("TE_MOVIE_TRACE"); return v && *v && std::strcmp(v, "0") != 0; }();
    return on;
}
}  // namespace

MoviePlayer::MoviePlayer() = default;
MoviePlayer::~MoviePlayer() {
    for (auto& [address, p] : players_) DestroyDecoder(p);
}

MoviePlayer::Player* MoviePlayer::Find(uint32_t player) {
    const auto it = players_.find(player);
    if (it == players_.end()) { ++count_.unknown_player; return nullptr; }
    return &it->second;
}

const MoviePlayer::Player* MoviePlayer::Inspect(uint32_t player) const {
    const auto it = players_.find(player);
    return it == players_.end() ? nullptr : &it->second;
}

void MoviePlayer::Reset() {
    for (auto& [address, p] : players_) DestroyDecoder(p);
    players_.clear();
    playing_ = nullptr;
    count_ = {};
}

uint64_t MoviePlayer::DueField(uint64_t present, double fps, uint32_t hz) {
    if (!(fps > 0.0) || hz == 0) return present * 2;   // the rate unknown: every second blank, as the PS2 presented
    return static_cast<uint64_t>(std::floor(static_cast<double>(present) * static_cast<double>(hz) / fps + 1e-9));
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------

void MoviePlayer::Construct(uint32_t player) {
    // The player's state is the game's 0x41D0 bytes: the PS2's constructor zeroed and set them up; nothing of the
    // host lives there, so they are zeroed and left alone (a savestate carries them like any guest bytes).
    if (uint8_t* bytes = te::GuestPointer(player, kPlayerSize)) std::memset(bytes, 0, kPlayerSize);
    auto it = players_.find(player);
    if (it != players_.end()) DestroyDecoder(it->second);
    players_[player] = Player{};
    players_[player].constructed = true;
    ++count_.constructs;
}

void MoviePlayer::BeginPresenting(uint32_t player, uint32_t present) {
    Player* p = Find(player);
    if (!p) return;
    p->presenting = true;
    p->present = present;
    p->queue.clear();
}

bool MoviePlayer::Open(uint32_t player, const char* file, uint32_t audioChannel, int32_t width, uint32_t lentMemory) {
    Player* p = Find(player);
    if (!p) return false;
    if (p->open) Close(player);
    p->file = file ? file : "";
    p->audio_channel = audioChannel;
    p->width = width;
    p->lent_memory = lentMemory;
    p->steps = p->waits = p->queued = p->draws = p->uploads = 0;
    p->pcm_popped = 0;
    p->frames = 0;
    p->fields = p->presented = 0;
    p->fps = 0.0;
    p->sound_started = false;
    p->volume = 0.f;
    p->decoded.reset();
    p->queue.clear();
    p->texture_filled = false;
    ++count_.opens;

    if (skip_) {
        p->open = true;
        p->input_done = true;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s skipped (skip movies)\n", p->file.c_str());
        return true;
    }

    // The file on the disc (the game's ISO name: "\FMV\VIVENDI.PSS;1"; IsoFs takes either separator, any case)
    te::Disc& disc = te::Services::Get().disc();
    disc.Initialise();   // opened on first use (a seam body reached before System::Initialise constructs what it needs)
    IsoFs::Entry entry;
    if (!disc.IsOpen() || p->file.empty() || !disc.Iso().Find(p->file, entry) || entry.directory || entry.size == 0) {
        ++count_.open_failed;
        std::fprintf(stderr, "[twinsanityevolved] movie: %s not on the disc: not played\n", p->file.c_str());
        return false;
    }
    p->image_offset = uint64_t{entry.lba} * IsoFs::kSectorSize;
    p->file_size = entry.size;
    p->read_cursor = 0;
    p->input_done = false;

    ps2e_movie_config cfg{};
    cfg.audio_channel = static_cast<int>(audioChannel);
    cfg.max_pictures = 3;
    p->decoder = ps2e_movie_create(&cfg);
    if (!p->decoder) {
        ++count_.open_failed;
        return false;
    }
    p->open = true;
    playing_ = p;
    EnsureMixerSource();

    // The first two pictures (one queued for the screen, one decoded and waiting), then the sound buffer filled
    if (DecodeNext(*p)) {
        p->queue.push_back(std::move(p->decoded));
        ++p->queued;
        ++count_.pictures_queued;
        DecodeNext(*p);
    }
    FeedSound(*p);
    if (p->fps <= 0.0) p->fps = ps2e_movie_fps(p->decoder);
    const Picture* first = p->queue.empty() ? nullptr : p->queue.front().get();
    std::fprintf(stderr, "[twinsanityevolved] movie: %s opened: %u bytes, %dx%d @ %.3f fps, sound track %u (%s), %u PCM frames buffered\n",
                 p->file.c_str(), p->file_size, first ? first->width : 0, first ? first->height : 0, p->fps, audioChannel,
                 ps2e_movie_has_audio(p->decoder) ? "PCM 48 kHz" : (ps2e_movie_audio_disabled(p->decoder) ? "unsupported format" : "none yet"),
                 ps2e_movie_pcm_queued(p->decoder));
    return true;
}

void MoviePlayer::StartSound(uint32_t player, float volume) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    p->volume = std::clamp(volume, 0.f, 1.f);
    p->sound_started = true;
}

bool MoviePlayer::Step(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return false;
    ++p->steps;
    if (skip_ || !p->decoder) {
        ++count_.skipped;
        return false;   // the movie has ended: the controller stops it this frame
    }
    if (Ended(*p)) return false;
    if (p->decoded && p->queue.size() < kPictureQueue) {
        p->queue.push_back(std::move(p->decoded));
        ++p->queued;
        ++count_.pictures_queued;
        DecodeNext(*p);
        p->frames++;
    }
    FeedSound(*p);
    return true;
}

void MoviePlayer::WaitFrame(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    ++p->waits;
    if (skip_ || !p->decoder) return;   // nothing is ever presented
    // The first blank (the PS2 handler's toggle), then more until the next picture is due by the movie's rate; the
    // present callback's own frame boundary is the blank the picture is shown at. Each Boundary() is a save-state
    // checkpoint: the fiber may be saved inside it and resumed in another process whose map nodes are elsewhere,
    // so `p` is looked up again after every boundary (no host heap pointer lives across the checkpoint).
    const uint64_t due = DueField(p->presented, p->fps, te::GetSettings().field_hz);
    p = Boundary(player);
    for (int extra = 0; p && p->fields + 1 < due && extra < 8; ++extra) p = Boundary(player);
    if (!p || !p->open) return;
    if (p->presenting && p->present) {
        if (Trace()) std::fprintf(stderr, "[movie] present %llu at field %llu (due %llu), queue %zu\n", (unsigned long long)p->presented,
                                  (unsigned long long)p->fields, (unsigned long long)due, p->queue.size());
        ++p->presented;
        ++count_.presents;
        p->fields++;   // the callback's PresentFromInterrupt is one boundary (graphics.cpp)
        count_.fields++;
        te_call_void_callback(p->present);
        p = Find(player);   // the callback ran boundaries (checkpoints): look the player up again
        if (p) p->quad_pending = false;   // the frame with Draw's quad is on the screen
    }
}

void MoviePlayer::QueuePicture(uint32_t player) {
    Player* p = Find(player);
    if (!p || !p->open || p->queue.empty()) return;
    std::unique_ptr<Picture> picture = std::move(p->queue.front());
    p->queue.pop_front();
    if (picture) UploadPicture(*p, *picture);
}

void MoviePlayer::Draw(uint32_t player, uint32_t area, int32_t width, int32_t height, uint32_t skippedRows) {
    Player* p = Find(player);
    if (!p || !p->open) return;
    ++p->draws;
    const auto* a = reinterpret_cast<const int32_t*>(te::GuestPointer(area, 16));   // Platform::Movie::Area {x, y, width, height}
    if (Trace() && p->draws <= 3)
        std::fprintf(stderr, "[movie] draw %u: area {%d %d %d %d} %dx%d skipped %u, frames %u texture %u%s\n", p->draws, a ? a[0] : -1,
                     a ? a[1] : -1, a ? a[2] : -1, a ? a[3] : -1, width, height, skippedRows, p->frames, p->texture,
                     p->texture_filled ? " filled" : "");
    if (skip_ || !p->decoder || p->frames < 2 || !p->texture || !p->texture_filled) return;
    if (!a || a[2] <= 0 || a[3] <= 0 || height <= 0) return;
    (void)width;
    const float x0 = float(a[0]), y0 = float(a[1]), x1 = float(a[0] + a[2]), y1 = float(a[1] + a[3]);
    const float v0 = float(skippedRows) / float(height), v1 = float(height - int32_t(skippedRows)) / float(height);
    te_graphics_record_movie_quad(p->texture, x0, y0, x1, y1, 0.f, v0, 1.f, v1);
    p->quad_pending = true;
    p->quad[0] = x0; p->quad[1] = y0; p->quad[2] = x1; p->quad[3] = y1; p->quad[4] = v0; p->quad[5] = v1;
    ++count_.draws;
}

void MoviePlayer::Close(uint32_t player) {
    Player* p = Find(player);
    if (!p) return;
    if (p->open) ++count_.closes;
    p->open = false;
    p->presenting = false;
    p->sound_started = false;
    p->decoded.reset();
    p->queue.clear();
    p->quad_pending = false;
    DestroyDecoder(*p);
    if (playing_ == p) playing_ = nullptr;
}

// ---- the decoder ------------------------------------------------------------------------------------------------------

bool MoviePlayer::Pump(Player& p) {
    if (!p.decoder || p.input_done) return false;
    if (p.read_cursor >= p.file_size) {
        p.input_done = true;
        ps2e_movie_end_input(p.decoder);
        return false;
    }
    const uint32_t n = std::min<uint32_t>(kReadSlice, p.file_size - p.read_cursor);
    std::vector<uint8_t> slice(n);
    const std::size_t got = te::Services::Get().disc().Iso().ReadBytes(p.image_offset + p.read_cursor, n, slice.data());
    p.read_cursor += static_cast<uint32_t>(got);
    count_.bytes_read += got;
    if (got) ps2e_movie_feed(p.decoder, slice.data(), got);
    if (got < n || p.read_cursor >= p.file_size) {
        p.input_done = true;
        ps2e_movie_end_input(p.decoder);
    }
    return true;
}

bool MoviePlayer::DecodeNext(Player& p) {
    p.decoded.reset();
    if (!p.decoder) return false;
    while (ps2e_movie_pictures_ready(p.decoder) == 0) {
        if (ps2e_movie_ended(p.decoder) || !Pump(p)) {
            if (ps2e_movie_pictures_ready(p.decoder) == 0) return false;
            break;
        }
    }
    ps2e_movie_picture pic{};
    if (!ps2e_movie_take_picture(p.decoder, &pic)) return false;
    auto out = std::make_unique<Picture>();
    out->width = pic.width;
    out->height = pic.height;
    out->rgba.assign(pic.rgba8, pic.rgba8 + size_t(pic.width) * size_t(pic.height) * 4);
    p.decoded = std::move(out);
    ++count_.pictures_decoded;
    if (p.fps <= 0.0) p.fps = ps2e_movie_fps(p.decoder);
    return true;
}

// The PS2 kept its 0x6000-byte ring full ahead of the SPU2; here the decoder's PCM queue is kept at that depth (the
// decoder decodes pictures too as it demuxes, up to its max_pictures; past that the video waits inside it).
void MoviePlayer::FeedSound(Player& p) {
    if (!p.decoder) return;
    for (int pumps = 0; ps2e_movie_pcm_queued(p.decoder) < kSoundBufferFrames && pumps < 16; ++pumps) {
        if (ps2e_movie_audio_disabled(p.decoder)) return;
        if (!Pump(p)) return;
    }
}

bool MoviePlayer::Ended(const Player& p) const {
    return p.decoder && ps2e_movie_ended(p.decoder) && !p.decoded;
}

MoviePlayer::Player* MoviePlayer::Boundary(uint32_t player) {
    te::Services::Get().clock().Boundary();   // the checkpoint: the player map may be another process's afterwards
    Player* p = Find(player);
    if (p) p->fields++;
    count_.fields++;
    count_.wait_boundaries++;
    return p;
}

void MoviePlayer::UploadPicture(Player& p, const Picture& picture) {
    ps2e_draw* dev = te_graphics_device();
    if (!dev || picture.width <= 0 || picture.height <= 0) return;
    // The device's calls run on the graphics side's render thread, synchronously
    struct Job { ps2e_draw* dev; Player* p; const Picture* picture; int uploaded; } job{dev, &p, &picture, 0};
    te_graphics_on_render([](void* user) {
        Job& j = *static_cast<Job*>(user);
        Player& p = *j.p;
        const Picture& picture = *j.picture;
        if (p.texture && (p.texture_w != picture.width || p.texture_h != picture.height)) {
            ps2e_draw_texture_destroy(j.dev, p.texture);
            p.texture = 0;
        }
        if (!p.texture) {
            p.texture = ps2e_draw_texture_create(j.dev, picture.width, picture.height, nullptr);
            p.texture_w = picture.width;
            p.texture_h = picture.height;
            if (!p.texture) return;
        }
        // The decoder's alpha is 0xFF: ps2e_draw's upload scales PS2 alpha bytes (0x80 = 1.0) and saturates, so the
        // picture is opaque either way
        j.uploaded = ps2e_draw_texture_upload(j.dev, p.texture, 0, 0, picture.width, picture.height, picture.rgba.data(), 0);
    }, &job);
    if (job.uploaded) {
        p.texture_filled = true;
        ++p.uploads;
        ++count_.pictures_uploaded;
    }
}

void MoviePlayer::DestroyDecoder(Player& p) {
    if (p.decoder) {
        ps2e_movie_destroy(p.decoder);
        p.decoder = nullptr;
    }
    if (p.texture) {
        if (ps2e_draw* dev = te_graphics_device()) {
            struct Job { ps2e_draw* dev; uint32_t texture; } job{dev, p.texture};
            te_graphics_on_render([](void* user) { Job& j = *static_cast<Job*>(user); ps2e_draw_texture_destroy(j.dev, j.texture); }, &job);
        }
        p.texture = 0;
        p.texture_filled = false;
    }
}

// ---- the sound track (the audio mixer's movie source) -------------------------------------------------------------------

void MoviePlayer::EnsureMixerSource() {
    if (mixer_hooked_) return;
    mixer_hooked_ = true;
    te::audio::Service().SetMovieSource(this);
}

std::array<float, 2> MoviePlayer::Gains() const {
    if (!playing_ || !playing_->open || !playing_->sound_started) return {0.f, 0.f};
    return {playing_->volume, playing_->volume};
}

int MoviePlayer::Pop(int16_t* stereo, int frames) {
    if (!playing_ || !playing_->open || !playing_->sound_started || !playing_->decoder || frames <= 0) return 0;
    const uint32_t got = ps2e_movie_pcm_read(playing_->decoder, stereo, static_cast<uint32_t>(frames));
    playing_->pcm_popped += got;
    count_.pcm_frames += got;
    if (got < static_cast<uint32_t>(frames) && !ps2e_movie_ended(playing_->decoder)) count_.pcm_silence_frames += uint32_t(frames) - got;
    return static_cast<int>(got);
}

// ---- the save-state section --------------------------------------------------------------------------------------------
// The game's state machine (GameController::StateMovie) holds a Platform::Movie player across frame boundaries: a state
// taken while a movie plays (or between Construct and Open) must restore a player the ps2e_movie_* calls find, or every
// later Step/WaitFrame/Close returns through Find()'s nullptr and the movie neither ends nor presents (Step false ->
// the controller leaves StateMovie at once: 3 frames early on the hub pad log, every frame after it shifted).
void MoviePlayer::Serialize(std::vector<uint8_t>& out) const {
    savestate::Put(out, uint32_t(2));   // version 2: + the pending quad (quad_pending, 6 floats) per player
    savestate::Put(out, uint8_t(skip_ ? 1 : 0));
    savestate::Put(out, uint32_t(players_.size()));
    for (const auto& [address, p] : players_) {
        savestate::Put(out, address);
        savestate::Put(out, uint8_t(p.constructed)); savestate::Put(out, uint8_t(p.presenting));
        savestate::Put(out, uint8_t(p.open)); savestate::Put(out, uint8_t(p.sound_started));
        savestate::Put(out, p.present); savestate::Put(out, p.audio_channel); savestate::Put(out, p.lent_memory);
        savestate::Put(out, p.width);
        savestate::PutString(out, p.file);
        savestate::Put(out, p.fields); savestate::Put(out, p.presented); savestate::Put(out, p.frames);
        savestate::Put(out, p.uploads); savestate::Put(out, p.pcm_popped);
        savestate::Put(out, p.volume);
        savestate::Put(out, uint8_t(p.quad_pending ? 1 : 0));
        for (float f : p.quad) savestate::Put(out, f);
    }
}

bool MoviePlayer::Restore(const std::vector<uint8_t>& payload, std::string* err) {
    savestate::PayloadReader in(payload);
    uint32_t version = 0, count = 0;
    uint8_t skip = 0;
    auto fail = [&](const char* why) { if (err) *err = std::string("te::movie::MoviePlayer: ") + why; return false; };
    if (!in.Get(&version) || (version != 1 && version != 2)) return fail("payload version");
    if (!in.Get(&skip) || !in.Get(&count) || count > 64) return fail("payload header");
    Reset();
    skip_ = skip != 0;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t address = 0;
        uint8_t constructed = 0, presenting = 0, open = 0, sound_started = 0;
        Player p;
        if (!in.Get(&address) || !in.Get(&constructed) || !in.Get(&presenting) || !in.Get(&open) || !in.Get(&sound_started) ||
            !in.Get(&p.present) || !in.Get(&p.audio_channel) || !in.Get(&p.lent_memory) || !in.Get(&p.width) ||
            !savestate::GetString(in, p.file, 0x100) || !in.Get(&p.fields) || !in.Get(&p.presented) || !in.Get(&p.frames) ||
            !in.Get(&p.uploads) || !in.Get(&p.pcm_popped) || !in.Get(&p.volume))
            return fail("payload truncated");
        uint8_t quad_pending = 0;
        float quad[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        if (version >= 2) {
            if (!in.Get(&quad_pending)) return fail("payload truncated (quad)");
            for (float& f : quad) if (!in.Get(&f)) return fail("payload truncated (quad)");
        }
        p.constructed = constructed != 0;
        p.presenting = presenting != 0;
        const uint64_t fields = p.fields, presented = p.presented;
        const uint32_t frames = p.frames, uploads = p.uploads, pcm_popped = p.pcm_popped, present = p.present,
                       audio_channel = p.audio_channel, lent_memory = p.lent_memory;
        const int32_t width = p.width;
        const float volume = p.volume;
        const std::string file = p.file;
        Player& q = players_[address];
        q = std::move(p);
        if (open) {
            // Re-open from the disc and bring the decoder to where it was: the saving run had decoded 2 + frames
            // pictures (Open's two, one per Step that queued), uploaded `uploads` of them and popped `pcm_popped` PCM
            // frames; the same sequence is decoded again (deterministic) and the consumed part discarded.
            if (!Open(address, file.c_str(), audio_channel, width, lent_memory)) {
                if (!skip_) std::fprintf(stderr, "[twinsanityevolved] movie: %s could not be re-opened on restore\n", file.c_str());
                q.open = true;   // the game believes it is open: Step ends it, Close closes it
                q.input_done = true;
            } else if (q.decoder) {
                std::deque<std::unique_ptr<Picture>> all;   // the decode sequence so far
                for (auto& pic : q.queue) all.push_back(std::move(pic));
                q.queue.clear();
                if (q.decoded) all.push_back(std::move(q.decoded));
                while (all.size() < size_t(2) + frames && DecodeNext(q)) all.push_back(std::move(q.decoded));
                q.decoded.reset();
                std::unique_ptr<Picture> last_uploaded;
                for (uint32_t u = 0; u < uploads && !all.empty(); ++u) { last_uploaded = std::move(all.front()); all.pop_front(); }
                if (last_uploaded) UploadPicture(q, *last_uploaded);
                if (!all.empty()) { q.decoded = std::move(all.back()); all.pop_back(); }
                for (auto& pic : all) q.queue.push_back(std::move(pic));
                std::vector<int16_t> sink(2 * 4096);
                for (uint32_t left = pcm_popped; left > 0;) {
                    FeedSound(q);
                    const uint32_t got = ps2e_movie_pcm_read(q.decoder, sink.data(), std::min<uint32_t>(left, 4096));
                    if (!got) break;
                    left -= got;
                }
                q.uploads = uploads;
                q.pcm_popped = pcm_popped;
                // The quad the saving run had drawn for the present its WaitFrame was waiting on (see the header): into
                // this process's frame, as Draw recorded it
                if (quad_pending && q.texture && q.texture_filled) {
                    te_graphics_record_movie_quad(q.texture, quad[0], quad[1], quad[2], quad[3], 0.f, quad[4], 1.f, quad[5]);
                    q.quad_pending = true;
                    std::copy(std::begin(quad), std::end(quad), std::begin(q.quad));
                }
                std::fprintf(stderr, "[twinsanityevolved] movie: %s re-opened on restore: %u pictures decoded (%zu queued, %s waiting), "
                             "%u uploaded, %u PCM frames discarded (%u queued now), %u of %u bytes read%s\n",
                             file.c_str(), 2 + frames, q.queue.size(), q.decoded ? "one" : "none", uploads, pcm_popped,
                             ps2e_movie_pcm_queued(q.decoder), q.read_cursor, q.file_size, q.input_done ? ", input done" : "");
            }
            q.fields = fields; q.presented = presented; q.frames = frames;
            q.sound_started = sound_started != 0;
            q.volume = volume;
            q.presenting = presenting != 0;
            q.present = present;
        }
    }
    return true;
}

MoviePlayer& Service() {
    static MoviePlayer player;
    static const bool registered = [] {
        savestate::Register(savestate::kTagMovie, "te::movie::MoviePlayer", false,
            [](std::vector<uint8_t>& out) { Service().Serialize(out); },
            [](const savestate::Blob& blob, std::string* error) { return Service().Restore(blob.payload, error); });
        return true;
    }();
    (void)registered;
    return player;
}

}  // namespace te::movie

extern "C" {

void ps2e_movie_construct(ps2e_u32 player) {
    te::movie::Service().SetSkipMovies(te::GetSettings().skip_movies);
    te::movie::Service().Construct(player);
}
void ps2e_movie_begin_presenting(ps2e_u32 player, ps2e_u32 present) { te::movie::Service().BeginPresenting(player, present); }
ps2e_s32 ps2e_movie_open(ps2e_u32 player, ps2e_u32 file, ps2e_s32 audioChannel, ps2e_s32 width, ps2e_u32 lentMemory) {
    return te::movie::Service().Open(player, te::GuestString(file, 0x100), static_cast<uint32_t>(audioChannel), width, lentMemory) ? 1 : 0;
}
void ps2e_movie_start_sound(ps2e_u32 player, ps2e_f32 volume) { te::movie::Service().StartSound(player, volume); }
ps2e_s32 ps2e_movie_step(ps2e_u32 player) { return te::movie::Service().Step(player) ? 1 : 0; }
void ps2e_movie_wait_frame(ps2e_u32 player) { te::movie::Service().WaitFrame(player); }
void ps2e_movie_queue_picture(ps2e_u32 player) { te::movie::Service().QueuePicture(player); }
void ps2e_movie_draw(ps2e_u32 player, ps2e_u32 area, ps2e_s32 width, ps2e_s32 height, ps2e_s32 skippedRows) {
    te::movie::Service().Draw(player, area, width, height, static_cast<uint32_t>(skippedRows));
}
void ps2e_movie_close(ps2e_u32 player) { te::movie::Service().Close(player); }

}  // extern "C"
