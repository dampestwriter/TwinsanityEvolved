#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "games/slus_20909_crash_twinsanity/hle/math/animation_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_library.h"
#include "games/slus_20909_crash_twinsanity/hle/math/joint_library.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"
#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

namespace ps2r::ipu { class LibmpegBindings; }
namespace ps2r::verify { class RunMeta; }
namespace ps2r::savestate { class SectionRegistry; }

namespace title {

// Title-owned math, movie and file services. Install after the
// SDK manifest so remaining generated consumers keep their existing bindings.
class GameLibraries {
public:
    struct Selection {
        bool math_helpers = false;
        bool movie_lifecycle = false;
        bool sound_bank_headers = true;
        bool stream_files = false;
        bool culling = false;
    };
    GameLibraries(ps2r::ipu::GuestMemory& memory, ps2r::ipu::LibmpegBindings& mpeg,
                  Selection selection, MovieDecoder::StreamServices movie_stream,
                  std::function<void(uint16_t)> movie_volume);
    bool Install();
    bool InstallMovieBuffers(MovieStream& stream, MovieStream::BufferServices buffers,
                             MovieStream::Lookup lookup);
    void Record(ps2r::verify::RunMeta& meta) const;
    bool CanCheckpoint(std::string* reason = nullptr) const;
    bool RegisterCheckpointSections(ps2r::savestate::SectionRegistry& registry);
    void ResetAfterRestore();
    // Root composes Movie persistence and immutable Graphics output after
    // Install. Their callbacks must be destroyed before this owner.
    MovieDecoder& Movie() { return movie_; }

private:
    MathLibrary math_;
    CollisionMath collision_;
    TrigMath trig_;
    AnimationMath animation_;
    BoundsMath bounds_;
    ClippingMath clipping_;
    JointLibrary joint_;
    CullingLibrary culling_;
    MovieDecoder movie_;
    MovieDecoder::StreamServices movie_stream_;
    MovieDecoder::DemuxService movie_demux_;
    Selection selection_;
    bool installed_ = false;
};

}  // namespace title
