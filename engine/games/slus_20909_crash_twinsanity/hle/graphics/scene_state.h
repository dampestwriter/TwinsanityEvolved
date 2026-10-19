#pragma once

#include <string>
#include <vector>

#include "engine/gs/hle/targets/coherence_state.h"
#include "engine/gs/hle/targets/target_identity.h"

namespace title {

// Checkpoint of the shared host attachments, not a texture/local-memory mirror.
// History is the existing host clear/depth/blend protocol during mixed output.
// No register snapshot, packet, backend name or C++ object representation is saved.
class SceneState {
public:
    static constexpr uint32_t kTag = 0x4e435347; // GSCN
    // v3 carries the target's frame pixel format (render_psm) and its 16-bit
    // history (was_16bit): the title's 16-bit display buffers (PSMCT16, the
    // title screen's targets) are captured as their RGBA8 attachments; an
    // alt16 companion is still refused. v2 images (no format word: 32-bit
    // targets only) remain loadable.
    static constexpr uint32_t kVersion = 3;
    static constexpr uint32_t kMinVersion = 2;
    static bool Accepts(uint32_t version) { return version >= kMinVersion && version <= kVersion; }
    static constexpr size_t kMaxBytes = 64 * 1024 * 1024;
    // Host destination/content geometry and the temporary mixed-output target
    // key. These are logical renderer values, never display-register bytes.
    struct Viewport {
        uint32_t width = 640, height = 448, content_width = 0, content_height = 0;
        uint32_t color_key = UINT32_MAX, buffer_width = 0, format = 0;
    };
    struct Target {
        ps2r::gs::TargetIdentity identity;
        ps2r::gs::CoherenceState history;
        std::vector<uint8_t> color, copy;
        std::vector<float> depth;
    };
    struct Snapshot {
        uint32_t display_frame = 0, game_frame = 0, recent = UINT32_MAX;
        uint64_t order = 0, epoch = 0;
        Viewport viewport;
        std::vector<Target> targets;
    };
    static bool Validate(const Snapshot&, std::string* error = nullptr);
    static bool ValidateViewport(const Viewport&, std::string* error = nullptr);
    // One target's identity + history against the codec's representable set
    // (the live cache asks this BEFORE a capture so the predicate refuses
    // instead of the serializer throwing). `order` = the snapshot draw order.
    static bool ValidateTarget(const ps2r::gs::TargetIdentity&, const ps2r::gs::CoherenceState&, uint64_t order,
                               std::string* error = nullptr);
    static bool Encode(const Snapshot&, std::vector<uint8_t>&, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>&, Snapshot&, std::string* error = nullptr);

private:
    struct Reader;
};

} // namespace title
