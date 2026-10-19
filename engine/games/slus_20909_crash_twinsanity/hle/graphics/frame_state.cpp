#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"

#include <utility>

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Graphics frame state: ") + why;
    return false;
}
bool Empty(const FrameEnvironment::SceneFrame& frame) {
    return !frame.color_buffer && !frame.depth_buffer && !frame.width && !frame.height &&
        !frame.source_frame && !frame.epoch;
}
bool Frame(const FrameEnvironment::SceneFrame& frame, uint64_t epoch) {
    if (!frame.width || frame.width > 2048 || frame.width % 64 ||
        !frame.height || frame.height > 2048 || !frame.epoch || frame.epoch > epoch) return false;
    const uint64_t pixels = uint64_t(frame.width) * frame.height;
    return frame.color_buffer == pixels * 2 / 8192 * 8192 &&
        frame.depth_buffer == pixels * 6 / 8192 * 8192 &&
        uint64_t(frame.depth_buffer) + pixels * 4 <= 4 * 1024 * 1024;
}
void Put(std::vector<uint8_t>& out, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
}

bool FrameState::Validate(const FrameEnvironment::Snapshot& s, std::string* error) {
    if (s.scene_valid && (!s.frame || !s.sky_known ||
                         !Frame(s.scene, s.source_epoch) || s.scene.source_frame != s.next_frame))
        return Bad(error, "current source frame is inconsistent");
    if (!s.scene_valid && !Empty(s.scene)) return Bad(error, "inactive scene carries stale identity");
    if (s.submitted_valid && (!Frame(s.submitted, s.source_epoch) ||
                             uint32_t(s.submitted.source_frame + 1) != s.next_frame))
        return Bad(error, "submitted source frame is inconsistent");
    if (!s.submitted_valid && !Empty(s.submitted)) return Bad(error, "inactive submission carries stale identity");
    if (s.scene_valid && s.submitted_valid && s.scene.epoch <= s.submitted.epoch)
        return Bad(error, "current setup precedes its submitted predecessor");
    if (error) error->clear();
    return true;
}

bool FrameState::Encode(const FrameEnvironment::Snapshot& s, std::vector<uint8_t>& out, std::string* error) {
    if (!Validate(s, error)) return false;
    std::vector<uint8_t> bytes;
    Put(bytes, kTag, 4); Put(bytes, kVersion, 4);
    const uint32_t flags = uint32_t(s.frame) | uint32_t(s.dither_known) << 1 |
        uint32_t(s.sky_known) << 2 | uint32_t(s.scene_valid) << 3 | uint32_t(s.submitted_valid) << 4;
    Put(bytes, flags, 4); Put(bytes, s.next_frame, 4); Put(bytes, s.source_epoch, 8);
    for (const auto& frame : {s.scene, s.submitted}) {
        Put(bytes, frame.color_buffer, 4); Put(bytes, frame.depth_buffer, 4);
        Put(bytes, frame.width, 4); Put(bytes, frame.height, 4);
        Put(bytes, frame.source_frame, 4); Put(bytes, frame.epoch, 8);
    }
    out = std::move(bytes);
    return true;
}

bool FrameState::Decode(const std::vector<uint8_t>& bytes, FrameEnvironment::Snapshot& out, std::string* error) {
    if (bytes.size() != 80) return Bad(error, "section extent differs");
    size_t at = 0;
    const auto take = [&](unsigned width) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[at++]) << (i * 8);
        return value;
    };
    if (take(4) != kTag || take(4) != kVersion) return Bad(error, "tag/version differs");
    const auto flags = take(4);
    if (flags & ~31u) return Bad(error, "unknown source flags");
    FrameEnvironment::Snapshot snapshot;
    snapshot.frame = flags & 1; snapshot.dither_known = flags & 2; snapshot.sky_known = flags & 4;
    snapshot.scene_valid = flags & 8; snapshot.submitted_valid = flags & 16;
    snapshot.next_frame = uint32_t(take(4)); snapshot.source_epoch = take(8);
    for (auto* frame : {&snapshot.scene, &snapshot.submitted}) {
        frame->color_buffer = uint32_t(take(4)); frame->depth_buffer = uint32_t(take(4));
        frame->width = uint32_t(take(4)); frame->height = uint32_t(take(4));
        frame->source_frame = uint32_t(take(4)); frame->epoch = take(8);
    }
    if (!Validate(snapshot, error)) return false;
    out = snapshot;
    return true;
}

}  // namespace title
