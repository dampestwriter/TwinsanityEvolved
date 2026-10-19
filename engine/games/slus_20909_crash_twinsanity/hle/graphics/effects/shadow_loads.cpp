#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_loads.h"
#include "engine/ee/timing/tick_profile.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace title {
ShadowLoads::ShadowLoads(ShadowAssets& assets, MaterialState::Read read, Refuse refuse)
    : assets_(assets), read_(std::move(read)), refuse_(std::move(refuse)) {}
bool ShadowLoads::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || !read_) return false;
    const auto read = capture(ShadowAssets::kRead), destroy = capture(ShadowAssets::kDestroy);
    if (!read || !destroy) return false;
    read_body_ = read; destroy_body_ = destroy;
    const auto active = std::make_shared<std::atomic<bool>>(false);
    const auto rollback = [&](size_t last) {
        bool restored = true;
        for (size_t count = last + 1; count != 0; --count) {
            const bool load = count == 1;
            try {
                if (!install(load ? ShadowAssets::kRead : ShadowAssets::kDestroy, load ? read : destroy))
                    restored = false;
            } catch (...) { restored = false; }
        }
        read_body_ = {}; destroy_body_ = {};
        if (!restored) throw std::runtime_error("ShadowLoads rollback failed");
    };
    for (size_t i = 0; i < 2; ++i) {
        const bool load = i == 0;
        const Body original = load ? read : destroy;
        bool accepted = false;
        try {
            accepted = install(load ? ShadowAssets::kRead : ShadowAssets::kDestroy,
                [this, active, original, load](CpuContext& c, uint32_t e) {
                    // Even a rejecting installer may have published its callback.
                    // Until both installs commit it only forwards, including if
                    // rollback itself rejects and this owner is then destroyed.
                    if (!active->load(std::memory_order_acquire)) { original(c, e); return; }
                    Run(load, c, e);
                });
        } catch (...) { rollback(i); throw; }
        if (!accepted) { rollback(i); return false; }
    }
    active->store(true, std::memory_order_release);
    installed_ = true;
    return true;
}
void ShadowLoads::Run(bool load, CpuContext& context, uint32_t entry) {
    const uint32_t address = load ? ShadowAssets::kRead : ShadowAssets::kDestroy;
    const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
    // A suspended interior has no proved original object argument. Replaying
    // it as a fresh load would hide partial effects and publish false ownership.
    if (entry && entry != address) {
        ++counts_.interior;
        throw std::runtime_error("ShadowLoads needs a complete source operation");
    }
    auto& count = load ? counts_.reads : counts_.destroys;
    ++count;
    assets_.Forget(object);
    ++active_;
    try {
        (load ? read_body_ : destroy_body_)(context, entry);
        if (load) {
            ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Shadow);
            const auto result = assets_.ModelLoaded(read_, object);
            if (result == ShadowAssets::Result::Ok) ++counts_.converted;
            else if (result == ShadowAssets::Result::NotSelected) ++counts_.unselected;
            else {
                // The original read completed; this owner holds nothing for the
                // model. Refuse by name (the Result), never abort the load.
                ++counts_.refused;
                if (refuse_) refuse_(result == ShadowAssets::Result::InvalidMemory ||
                                     result == ShadowAssets::Result::LimitExceeded ?
                                     GraphicsRefusals::Reason::UnsupportedState :
                                     GraphicsRefusals::Reason::MissingResource,
                                     ShadowAssets::kRead, uint32_t(result));
            }
        }
        --active_;
    } catch (...) { --active_; ++counts_.failed; throw; }
}
}  // namespace title
