#pragma once

#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"

namespace title {

// The exact image awaiting the existing display tick. Shared scene references
// retain identity across restore; detached/converted images own complete pixels.
// No backend names, scanout registers, packet storage or execution counts.
class PresentationState {
public:
    static constexpr uint32_t kTag = 0x53525047; // GPRS
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kMaxBytes = 64 * 1024 * 1024;
    enum class Kind : uint32_t { None, Scene, Image };
    struct Image {
        Kind kind = Kind::None;
        FrameEnvironment::SceneFrame frame;
        uint32_t width = 0, height = 0, scale = 0;
        std::vector<uint8_t> rgba;
    };
    struct Snapshot {
        Image movie, display;
        FrameEnvironment::SceneFrame display_source;
    };
    static bool Validate(const Image&,std::string* error = nullptr);
    static bool Validate(const Snapshot&,std::string* error = nullptr);
    static bool Encode(const Snapshot&,std::vector<uint8_t>&,std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>&,Snapshot&,std::string* error = nullptr);
private:
    struct Reader;
};
} // namespace title
