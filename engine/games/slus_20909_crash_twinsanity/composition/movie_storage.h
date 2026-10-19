#pragma once

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

class IsoFs;
namespace title { class GameLibraries; class MovieStream; }
namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Composition lifetime: constructed after Movie and Audio, destroyed after
// game workers join and before those consumers. Owns the actual storage.
class MovieStorage {
public:
    MovieStorage(title::GameLibraries& libraries, title::MovieStream& movie,
                 IsoFs& files, bool selected);
    ~MovieStorage();
    // Consumers install first; only then can music publish opaque handles.
    bool Install();
    void Record(verify::RunMeta& meta) const;

private:
    title::GameLibraries& libraries_;
    title::MovieStream& movie_;
    IsoFs& files_;
    title::StreamBuffers storage_{2 * 1024 * 1024};
    bool selected_ = false, installed_ = false;
};
} // namespace ps2r::app
