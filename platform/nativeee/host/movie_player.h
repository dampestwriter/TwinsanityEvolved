// movie_player.h -- platform/nativeee's movie owner: Platform::Movie (movie.h) over the engine's ps2e_movie (the C
// movie-decoder surface, engine/platform/ps2e: PSS demux + MPEG-2 video + the PCM sound track), the graphics owner's
// picture quad (te_graphics_hooks.h) and the audio owner's mixer (the movie is the mixer's PcmSource). Host-typed so
// tests drive it directly; the extern "C" ps2e_movie_* bodies in movie.cpp are one-liners over Service().
//
// The model is the PS2 player's (game/src/platform/ps2/movie/player.cpp), with the decoder and the clock replaced:
//   * Open finds the file on the disc (te::Services::disc()), decodes the first two pictures (one queued for the
//     screen, one decoded and waiting) and fills the sound buffer; the file is read on in 128 KB slices as decoding
//     needs them (never whole: the disc's movies are up to 160 MB).
//   * Step queues the waiting picture when fewer than two wait for the screen, decodes the next, feeds the sound;
//     false once the movie has ended (the decoder is drained and nothing waits to be queued).
//   * WaitFrame is where the game's one thread meets the clock while a movie plays: it runs te::Clock boundaries
//     (fields) until the next picture is DUE by the movie's own frame rate (its sequence header: 25 fps on the PAL
//     disc; at 60 Hz a picture every 2.4 fields -- the PS2 at 50 Hz showed one every second blank), then calls the
//     game's present callback back into the module (te_call_void_callback: the vertical-blank handler's call), which
//     presents the frame (one more boundary), asks QueuePicture for the next picture and finishes the frame.
//   * QueuePicture uploads the queue's first picture into the picture texture ("sends it to the screen's memory");
//     Draw records that texture over the game's area into the movies bucket (nothing until two pictures were queued).
//   * The sound: the decoder's PCM (48 kHz stereo, the SPU2's rate) is popped by the audio mixer's tick (AudioMixer's
//     movie source, one tick per boundary) from StartSound on, at the game's volume; nothing before (the PS2 filled
//     the ring, then started the SPU2's block transfer).
// Skipping (the FEATURES "skip movies" toggle, te::Settings::skip_movies): every movie ends at its first Step and
// nothing is drawn -- the stub's behaviour, kept behind the setting.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/sound/pcm_source.h"

typedef struct ps2e_movie ps2e_movie;

namespace te::movie {

class MoviePlayer final : public ps2r::sound::PcmSource {
public:
    static constexpr uint32_t kPlayerSize = 0x41D0;        // Platform::Movie::PlayerSize
    static constexpr uint32_t kLentMemorySize = 0x2E040;   // Platform::Movie::LentMemorySize
    static constexpr uint32_t kPictureQueue = 2;           // pictures waiting for the screen (the PS2's PictureQueueSize)
    static constexpr uint32_t kSoundBufferFrames = 0x6000 / 4;   // the PS2 ring: 0x6000 bytes of 16-bit stereo
    static constexpr uint32_t kReadSlice = 64 * 2048;      // bytes read off the disc per pump

    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    struct Player {
        bool constructed = false, presenting = false, open = false, sound_started = false;
        uint32_t present = 0;          // the game's present callback: a wasm table index
        uint32_t audio_channel = 0, lent_memory = 0;
        int32_t width = 0;             // the game's width (its texture width unit; informational)
        std::string file;
        // the disc file
        uint64_t image_offset = 0;
        uint32_t file_size = 0, read_cursor = 0;
        bool input_done = false;
        // the decoder and the pictures
        ps2e_movie* decoder = nullptr;
        std::unique_ptr<Picture> decoded;        // decoded, not yet queued (the PS2's player->decoded)
        std::deque<std::unique_ptr<Picture>> queue;   // waiting for the screen (player->queue)
        uint32_t frames = 0;                     // Steps that queued a picture (player->frames; Draw needs 2)
        uint32_t texture = 0;                    // the picture texture on the graphics device (0 = none yet)
        int texture_w = 0, texture_h = 0;
        bool texture_filled = false;
        // The quad Draw recorded into the frame's movies bucket and the present callback has not yet shown: the game draws
        // BEFORE WaitFrame and the frame is presented INSIDE it, so a save state taken at one of WaitFrame's boundaries
        // (the frame-boundary checkpoints) sits between the two and the host-side draw would be lost -- the loaded run's first present
        // was black (found when the drive model moved the boot timeline under the movie round-trip gate). Carried in the
        // movie section (payload version 2) and recorded again on restore.
        bool quad_pending = false;
        float quad[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};   // x0 y0 x1 y1 v0 v1 (frame pixels / texture fractions)
        // the clock while the movie plays
        double fps = 0.0;                        // the sequence header's, once known
        uint64_t fields = 0;                     // boundaries since Open
        uint64_t presented = 0;                  // present callbacks made
        float volume = 0.f;
        // counters
        uint32_t steps = 0, waits = 0, queued = 0, draws = 0, uploads = 0;
        uint32_t pcm_popped = 0;                 // PCM frames the mixer popped from this player (the restore discards as many)
    };

    MoviePlayer();
    ~MoviePlayer() override;

    // The FEATURES toggle: true = every movie is skipped (ends at its first Step, draws nothing).
    void SetSkipMovies(bool skip) { skip_ = skip; }
    bool SkipMovies() const { return skip_; }

    void Construct(uint32_t player);
    void BeginPresenting(uint32_t player, uint32_t present);
    bool Open(uint32_t player, const char* file, uint32_t audioChannel, int32_t width, uint32_t lentMemory);
    void StartSound(uint32_t player, float volume);
    bool Step(uint32_t player);
    void WaitFrame(uint32_t player);
    void QueuePicture(uint32_t player);
    void Draw(uint32_t player, uint32_t area, int32_t width, int32_t height, uint32_t skippedRows);
    void Close(uint32_t player);

    // PcmSource (the audio mixer pops the sound track here, one tick per boundary)
    std::array<float, 2> Gains() const override;
    int Pop(int16_t* stereo, int frames) override;

    const Player* Inspect(uint32_t player) const;
    // The save-state section (savestate::kTagMovie): the players the game constructed, by guest address, with what the
    // game's own 0x41D0 bytes do not hold -- constructed/presenting/the present callback/open/the file/sound. A movie
    // PLAYING at the checkpoint is re-opened on restore from its first picture (the decoder is not state); its
    // fields/presented counts are kept so WaitFrame's due-field pacing catches the pictures up rather than re-timing them.
    void Serialize(std::vector<uint8_t>& out) const;
    bool Restore(const std::vector<uint8_t>& payload, std::string* err);
    struct Counters {
        uint64_t constructs = 0, opens = 0, open_failed = 0, skipped = 0, closes = 0, unknown_player = 0;
        uint64_t pictures_decoded = 0, pictures_queued = 0, pictures_uploaded = 0, presents = 0, draws = 0,
                 fields = 0, wait_boundaries = 0, bytes_read = 0;
        uint64_t pcm_frames = 0, pcm_silence_frames = 0;   // popped by the mixer; of them, frames the decoder had not yet produced
    };
    Counters Count() const { return count_; }
    void Reset();

    // The fields at which present p is due, by the movie's rate (hz fields per second): floor(p * hz / fps).
    static uint64_t DueField(uint64_t present, double fps, uint32_t hz);

private:
    Player* Find(uint32_t player);
    bool Pump(Player& p);                 // one slice off the disc into the decoder; false at the file's end
    bool DecodeNext(Player& p);           // p.decoded <- the next picture (pumping as needed); false when none will come
    void FeedSound(Player& p);            // pump until the sound buffer's worth of PCM waits (or the input ended)
    bool Ended(const Player& p) const;
    Player* Boundary(uint32_t player);   // one clock boundary (a save-state checkpoint); the player looked up again after it
    bool PictureDue(const Player& p) const;
    void UploadPicture(Player& p, const Picture& picture);
    void DestroyDecoder(Player& p);
    void EnsureMixerSource();

    bool skip_ = false;
    bool mixer_hooked_ = false;
    Player* playing_ = nullptr;           // the one open player (the sound source)
    std::map<uint32_t, Player> players_;  // by guest address
    Counters count_;
};

MoviePlayer& Service();

}  // namespace te::movie
