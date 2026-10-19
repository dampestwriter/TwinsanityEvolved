#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "engine/ipu/hle/decoded_picture.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

namespace title {

// Movie RGB32 delivery and drawing, specified by TR movie/player.cpp and the
// NTSC public callers. Guest queue words carry checked IDs, never pointers to
// packets or VRAM. Start/Step/WaitFrame keep the game's existing queue cadence.
class MovieGraphics {
public:
    static constexpr uint32_t kDecodeAndUpload = 0x002b0e50;
    static constexpr uint32_t kQueuePicture = 0x002af968;
    static constexpr uint32_t kDraw = 0x002aeb30;
    static constexpr uint32_t kBucket = 27;
    static constexpr uint32_t kDisplayWidth = 0x0030a764;
    static constexpr uint32_t kDisplayHeight = 0x0030a768;
    static constexpr uint32_t kWidescreenTv = 0x0030a744;
    struct View {
        uint32_t screen_width = 0, screen_height = 0;
        uint32_t movie_width = 0, movie_height = 0;
        bool wide_tv = false, wide_movie = false;
        // The session's selected display ratio (video.aspect);
        // 0 = the title's own term (wide_tv). A non-4:3 ratio places the
        // picture by its shape (TR movie.cpp:224): a 4:3 picture is centred
        // at (4/3)/ratio of the width (pillarbox), a 16:9 one at (16/9)/ratio,
        // or letterboxed at ratio/(16/9) of the height when the TV is narrower
        // than the picture. The 32-pixel strips round the width down.
        float tv_aspect = 0.0f;
    };
    struct Image {
        uint32_t id = 0;  // never reused by this MovieGraphics owner
        ps2r::ipu::DecodedPicture pixels;
        // Nonzero for a draw's immutable sampled view of a decoded image.
        // A view has its own ID/allocation; it cannot overwrite queued users.
        uint32_t source_id = 0, sample_width = 0, sample_height = 0;
    };
    using ImageLease = std::shared_ptr<const Image>;
    struct OutputServices {
        // Source QueuePicture's immediate upload slot. Capture the immutable
        // lease before accepting; the render owner creates/uploads its resource.
        std::function<bool(ImageLease)> upload;
        // Source bucket 27. Copy draw, lease and source frame setup before
        // accepting. Deferred consumers must not reread guest/model state.
        // This lease is a sampled view, not the raw upload's ID. In this same
        // ordered bucket callback, upload the view once before its first draw.
        std::function<bool(const ps2r::gs::DrawIr&, ImageLease)> record;
    };
    struct Hooks {
        // LibmpegBindings::ReadPlaybackPicture: same timer/PCM/clock services.
        std::function<void(CpuContext&, ps2r::ipu::DecodedPicture&)> picture;
        // GraphicsResources::Register(queue, description, &handle).
        std::function<bool(const ps2r::render::SurfaceDesc&, Graphics::Handle&)> register_image;
        // GraphicsTargets::RecordMovie: explicit scene color/depth/copy and
        // this uploaded image. No target/texture address lookup.
        std::function<bool(const Graphics::Anchor&, const ps2r::gs::DrawIr&,
                           const Graphics::Handle&)> record;
        // Existing PresentFromInterrupt boundary, AFTER ordered submission.
        // Present SceneColor directly; do not select a stale sky composite.
        // This does not advance a clock, queue a picture, or add a present.
        std::function<bool()> present;
    };
    struct Counters {
        std::array<uint64_t, 3> original_calls{};
        // Upload/draw counts are accepted producer intents. Graphics owns
        // completed submission counters and the render-owner fence.
        uint64_t decoded = 0, uploaded = 0, draws = 0, presents = 0;
        uint64_t views = 0;
        uint64_t empty_queues = 0, priming_draws = 0, errors = 0, closes = 0;
    };

    MovieGraphics(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder,
                  Graphics& graphics, Hooks hooks);
    // Output owner supplies ordered upload/draw services instead of borrowing
    // one mutable Graphics queue across deferred submissions. Hooks needs
    // picture/present; its fixed-queue register_image/record must be empty.
    MovieGraphics(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder,
                  Hooks hooks, OutputServices output);
    // Lifecycle seven must already be installed. The root owns all instances
    // until guest dispatch stops, then closes Movie before destroying Graphics.
    // selected=false retains actual generated boundaries for the same-input oracle.
    int Install(const MovieDecoder::Capture&, const MovieDecoder::Installer&, bool selected);
    void DecodeAndUpload(CpuContext&);
    void QueuePicture(CpuContext&);
    // Source QueueDecoded, on the game execution owner. This complete queue
    // update cannot yield; callbacks run at the later decode/pacing service.
    bool QueueDecoded(uint32_t player);
    void Draw(CpuContext&);
    bool Present();
    // Called by decoder Close/failed Start/Initialize/restore reset. Queued
    // Graphics leases survive until its consumer drains; no backend work here.
    void Close();
    bool Active() const { return controller_ != 0; }
    // Close releases images but does not clear a refused output operation.
    // Root must join this predicate with render-owner completion guards.
    bool CanCheckpoint(std::string* reason = nullptr) const;
    bool Selected() const { return installed_ && selected_; }
    Counters Counts() const { return counts_; }
    size_t PendingPictures() const { return pictures_.size(); }
    std::string Census() const;
    // Complete source-authored movie draw, independent of GS state. Address
    // metadata is zero: explicit owned bindings supply every resource.
    static bool BuildDraw(const View&, ps2r::gs::DrawIr&);
    // The selected display ratio the Draw's View takes (empty = the title's flag).
    void SetAspectSource(std::function<double()> ratio) { aspect_ = std::move(ratio); }
    // As the original runtime does: extend the last texels strictly below max UV-0.5.
    // The view is movie-only, keeps source UVs, and has a guarded right/bottom
    // edge within a 16-aligned owned allocation. No texture/model state lookup.
    static bool BuildImageView(const ps2r::gs::DrawIr&,
                               const ps2r::ipu::DecodedPicture&,
                               ps2r::ipu::DecodedPicture&);

private:
    bool Begin(uint32_t decoder, uint32_t width, uint32_t height);
    void ReadPicture(CpuContext&);
    void CollectPictures();
    void SelectView(const ps2r::gs::DrawIr&);
    void Dispatch(size_t index, CpuContext&, uint32_t entry);
    [[noreturn]] void Fail(uint32_t entry);

    ps2r::ipu::GuestMemory& memory_;
    MovieDecoder& decoder_;
    Graphics* graphics_ = nullptr;
    Hooks hooks_;
    OutputServices output_;
    std::function<double()> aspect_;
    std::array<MovieDecoder::Body, 3> originals_;
    bool installed_ = false, selected_ = false, has_image_ = false;
    uint32_t controller_ = 0, width_ = 0, height_ = 0;
    uint32_t next_id_ = 1, latest_id_ = 0;
    Graphics::Handle image_, view_image_;
    ImageLease displayed_, sampled_;
    std::unordered_map<uint32_t, ImageLease> pictures_;
    Counters counts_;
};

}  // namespace title
