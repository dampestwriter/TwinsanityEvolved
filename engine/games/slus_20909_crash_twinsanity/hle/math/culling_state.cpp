#include "games/slus_20909_crash_twinsanity/hle/math/culling_state.h"

namespace title {
namespace {
bool Reject(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool ValidMasks(const CullingMath::Classification& visibility) {
    return ((visibility.outside | visibility.clipped) & ~0xf0u) == 0;
}
}

void CullingState::LoadView(const CullingMath::View& view) {
    const CullingMath::Matrix identity{{
        {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    state_.view = view;
    state_.link = CullingMath::LoadLink(view, identity, identity);
    state_.result = std::monostate{};
}

void CullingState::LoadPlanes(const CullingMath::Frustum& planes) {
    state_.planes = planes;
    state_.result = std::monostate{};
}

bool CullingState::LoadLink(
        const CullingMath::Matrix& chunk, const CullingMath::Matrix& object) {
    if (!state_.view) return false;
    state_.link = CullingMath::LoadLink(*state_.view, chunk, object);
    state_.result = std::monostate{};
    return true;
}

bool CullingState::LoadModel(const CullingMath::Matrix& model) {
    if (!state_.link) return false;
    state_.result = CullingMath::LoadModel(*state_.link, model);
    return true;
}

bool CullingState::TestCell(const CullingMath::Box& box) {
    if (!state_.planes) return false;
    state_.result = CullingMath::TestBox(*state_.planes, box);
    return true;
}

bool CullingState::TestBoxAt(const CullingMath::Box& box, const CullingMath::Matrix& model) {
    if (!state_.planes || !state_.link) return false;
    state_.result = CullingMath::TestBoxAt(*state_.planes, *state_.link, box, model);
    return true;
}

std::optional<CullingMath::Classification> CullingState::CellResult() const {
    if (const auto* value = std::get_if<CullingMath::Classification>(&state_.result)) return *value;
    return std::nullopt;
}

std::optional<CullingMath::Model> CullingState::DrawnModelResult() const {
    if (const auto* value = std::get_if<CullingMath::Model>(&state_.result)) return *value;
    return std::nullopt;
}

std::optional<CullingMath::BoxView> CullingState::MeshResult() const {
    if (const auto* value = std::get_if<CullingMath::BoxView>(&state_.result)) return *value;
    return std::nullopt;
}

std::optional<CullingMath::BoxView> CullingState::InstanceResult() const {
    auto result = MeshResult();
    if (result) {
        result->to_clip.reset();
        result->to_screen.reset();
    }
    return result;
}

void CullingState::Reset() { state_ = {}; }
CullingState::Snapshot CullingState::Capture() const { return state_; }

bool CullingState::Validate(const Snapshot& snapshot, std::string* error) {
    if (snapshot.version != 1) return Reject(error, "Culling: unsupported snapshot version");
    if (snapshot.view.has_value() != snapshot.link.has_value())
        return Reject(error, "Culling: view and link must be owned together");
    if (snapshot.result.valueless_by_exception())
        return Reject(error, "Culling: invalid result kind");
    if (const auto* cell = std::get_if<CullingMath::Classification>(&snapshot.result)) {
        if (!snapshot.planes || !ValidMasks(*cell))
            return Reject(error, "Culling: cell result requires valid planes and masks");
    }
    if (const auto* model = std::get_if<CullingMath::Model>(&snapshot.result)) {
        if (!snapshot.link || model->distance < 0)
            return Reject(error, "Culling: model result requires its view and nonnegative distance");
    }
    if (const auto* box = std::get_if<CullingMath::BoxView>(&snapshot.result)) {
        if (!snapshot.planes || !snapshot.link || !ValidMasks(box->visibility) || box->distance < 0)
            return Reject(error, "Culling: box result requires its view, planes and valid outcome");
        const bool visible = box->visibility.outside == 0;
        if (box->to_screen.has_value() != visible ||
            box->to_clip.has_value() != (visible && box->visibility.clipped != 0))
            return Reject(error, "Culling: matrices disagree with source result validity");
    }
    return true;
}

bool CullingState::Restore(const Snapshot& snapshot, std::string* error) {
    if (!Validate(snapshot, error)) return false;
    state_ = snapshot;
    return true;
}

}  // namespace title
