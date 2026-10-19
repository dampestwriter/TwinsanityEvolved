#pragma once

#include <string>

namespace ps2r {
namespace gs { class DrawIrThreadQueue; class DrawIrQueueConsumer; }
namespace verify { class RunMeta; }
namespace app {
class VideoStack;

// Read only after the output queue drains, on its existing context owner.
class VideoCensus {
public:
    static void Record(verify::RunMeta& meta, VideoStack& video,
                       gs::DrawIrThreadQueue* queue, gs::DrawIrQueueConsumer* consumer,
                       const std::string& output_directory);
};

}  // namespace app
}  // namespace ps2r
