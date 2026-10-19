#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include <cfenv>

namespace title {

RoundingGuard::RoundingGuard() noexcept : previous_(std::fegetround()) {
    std::fesetround(FE_TOWARDZERO);
}

RoundingGuard::~RoundingGuard() noexcept {
    std::fesetround(previous_);
}

}  // namespace title
