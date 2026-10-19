#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_service.h"

#include <stdexcept>
#include <utility>

namespace title {

SkyService::SkyService(Hooks hooks) : hooks_(std::move(hooks)) {}

bool SkyService::Install(const Capture& capture, const Installer& installer) {
    if (installed_) return true;
    if (!capture || !installer || !hooks_.read || !hooks_.environment || !hooks_.record)
        return false;
    auto original = capture(SkyIntent::kAddress);
    if (!original) return false;
    original_ = std::move(original);
    if (!installer(SkyIntent::kAddress, [this](CpuContext& c, uint32_t entry) { Run(c, entry); })) {
        original_ = {};
        return false;
    }
    installed_ = true;
    return true;
}

void SkyService::Run(CpuContext& context, uint32_t entry) {
    if (faulted_ || !original_) throw std::logic_error("Sky boundary has no valid continuation");
    const bool full = entry == 0 || entry == SkyIntent::kAddress;
    if (full) ++counts_.calls;
    else ++counts_.interior_calls;
    if (full && hooks_.enabled) {
        MaterialState::Environment environment;
        SkyIntent::Output intent;
        if (hooks_.environment(environment) &&
            SkyIntent::Capture(hooks_.read, entry, environment, intent) == SkyIntent::Result::Ok) {
            bool accepted = false;
            try { accepted = hooks_.record(intent); }
            catch (...) {
                faulted_ = true;
                ++counts_.recording_failures;
                throw;
            }
            if (accepted) {
                ++counts_.replaced;
                return;
            }
        }
    }
    ++counts_.model_calls;
    original_(context, entry);
}

SkyService::Counters SkyService::Counts() const { return counts_; }

}  // namespace title
