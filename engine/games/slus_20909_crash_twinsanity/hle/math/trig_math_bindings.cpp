#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

#include <utility>

#include "engine/ee/cpu/dispatch_table.h"

namespace title {

bool TrigMath::Install(const Capture& capture, const Installer& installer, bool replace_helpers) {
    if (installation_ || !capture || !installer) return false;
    using Binding = std::pair<uint32_t, void (TrigMath::*)(CpuContext&)>;
    const std::array<Binding, kEntryCount> bindings{{
        {kCosSin16Address, &TrigMath::CosSin16},
        {kSinCos16Address, &TrigMath::SinCos16},
        {kSinCos16PairAddress, &TrigMath::SinCos16Pair},
        {kPitchAddress, &TrigMath::RotationFromPitch},
        {kYawAddress, &TrigMath::RotationFromYaw},
        {kRollAddress, &TrigMath::RotationFromRoll},
        {kRotationXYAddress, &TrigMath::GetRotationXY},
        {kRotationXZAddress, &TrigMath::GetRotationXZ},
        {kRotationYZAddress, &TrigMath::GetRotationYZ},
        {kMatrixXAddress, &TrigMath::MatrixAboutX},
        {kMatrixYAddress, &TrigMath::MatrixAboutY},
        {kMatrixZAddress, &TrigMath::MatrixRotationZ},
        {kRotationXYZAddress, &TrigMath::GetRotationXYZ},
        {kTangentAddress, &TrigMath::TanOfAngle},
        {kMatrixXYAddress, &TrigMath::MatrixFromPitchYaw},
        {kMatrixXZAddress, &TrigMath::MatrixFromPitchRoll},
        {kMatrixYZAddress, &TrigMath::MatrixFromYawRoll},
        {kMatrixXYZAddress, &TrigMath::MatrixFromAngles},
        {kTurnTowardAddress, &TrigMath::TurnToward},
        {kTurnSineCosineAddress, &TrigMath::TurnBySineCosine},
        {kTurnAxisAddress, &TrigMath::TurnAboutAxis},
        {kRotationAxisSineAddress, &TrigMath::RotationFromAxisSine},
        {kRotationAxisAddress, &TrigMath::RotationAboutAxis},
        {kMatrixAxisSineAddress, &TrigMath::MatrixAboutAxis},
        {kMatrixAxisAddress, &TrigMath::AxisAngleMatrix},
        {kInverseLength4Address, &TrigMath::InverseLength4},
        {kSlerpAddress, &TrigMath::SlerpRotations},
        {kAngleFromAddress, &TrigMath::AngleFrom},
        {kAngleOfPointAddress, &TrigMath::AngleOfPoint},
        {kEulerAnglesAddress, &TrigMath::EulerAnglesOfMatrix},
        {kSnapYawAddress, &TrigMath::SnapToYaw},
    }};
    std::array<Body, bindings.size()> captured, originals;
    for (size_t i = 0; i < bindings.size(); ++i) {
        captured[i] = capture(bindings[i].first);
        if (!captured[i]) return false;
        originals[i] = DispatchTable::CountCalls(bindings[i].first, captured[i],
            std::shared_ptr<std::atomic<uint64_t>>(generated_body_calls_, &(*generated_body_calls_)[i]));
    }
    const auto active = std::make_shared<std::atomic<bool>>(false);
    const auto rollback = [&](size_t last) {
        for (size_t remaining = last + 1; remaining != 0; --remaining) {
            const size_t index = remaining - 1;
            try { (void)installer(bindings[index].first, captured[index]); }
            catch (...) {}  // An unpublished owner leaves retained callbacks inactive.
        }
    };
    for (size_t i = 0; i < bindings.size(); ++i) {
        try {
            const auto binding = bindings[i];
            Body body = originals[i];
            if (replace_helpers) {
                Body entry_body = [this, binding, original = originals[i], active]
                    (CpuContext& context, uint32_t entry) {
                    if (!active->load(std::memory_order_acquire)) original(context, entry);
                    else (this->*binding.second)(context);
                };
                body = DispatchTable::DelegateInterior(binding.first, originals[i], std::move(entry_body));
            }
            body = DispatchTable::CountCalls(binding.first, std::move(body), continuations_,
                DispatchTable::CountMode::InteriorCalls, active);
            if (installer(binding.first, std::move(body))) continue;
        } catch (...) {
            rollback(i);
            throw;
        }
        rollback(i);
        return false;
    }
    installation_ = active;
    active->store(true, std::memory_order_release);
    return true;
}

}  // namespace title
