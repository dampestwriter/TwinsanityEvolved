#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"

#include <stdexcept>

#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"

namespace ps2r::app {
GraphicsCheckpoint::GraphicsCheckpoint(::title::GraphicsScene& scene, Ordered ordered,
    ::title::GraphicsPresentation* movie,::title::GraphicsDisplay* display, Resize resize)
    : scene_(scene), ordered_(std::move(ordered)),movie_(movie),display_(display),resize_(std::move(resize)) {}

bool GraphicsCheckpoint::CanCapture(std::string* error) {
    if (!registered_ || !ordered_) {
        if (error) *error = "Graphics surface state owner is not registered";
        return false;
    }
    const bool ok = ordered_([&] {
        return scene_.SettleForCapture(error) && scene_.CanCaptureState(error) &&
            (!movie_ || movie_->CanCaptureState(error)) && (!display_ || display_->CanCaptureState(error));
    });
    if (!ok && error && error->empty()) *error = "Graphics surface owner failed to drain";
    return ok;
}

bool GraphicsCheckpoint::Register(StateRestorer& restorer) {
    if (registered_ || !ordered_ || bool(movie_) != bool(display_)) return false;
    using Codec = ::title::SceneState;
    savestate::SectionSpec spec;
    spec.tag = Codec::kTag;
    spec.version = Codec::kVersion;
    spec.owner = "Graphics shared surfaces";
    spec.required = !legacy_;
    spec.capture_required = true;
    if (legacy_) {
        // A frozen image carries no owned target set: the retained pipeline's
        // own VRAM restore governs the live targets (the retained arm's path
        // for the same anchors); nothing owned is published.
        spec.absent = [this] {
            if (!legacy_()) throw std::runtime_error("Graphics shared surfaces section is missing without an admitted legacy import");
            if (!ordered_([&] { scene_.DiscardStagedState(); return true; }))
                throw std::runtime_error("Graphics shared surfaces legacy reset lost its owner");
        };
    }
    spec.serialize = [this](auto& bytes) {
        Codec::Snapshot snapshot;
        std::string error;
        if (!ordered_([&] { return scene_.SettleForCapture(&error) && scene_.CaptureState(snapshot,&error); }) ||
            !Codec::Encode(snapshot,bytes,&error))
            throw std::runtime_error("Graphics surface capture: " + error);
    };
    spec.validate = [](const savestate::Blob& blob, auto* error) {
        Codec::Snapshot snapshot;
        return Codec::Accepts(blob.version) && Codec::Decode(blob.payload,snapshot,error);
    };
    spec.restore = [this](const savestate::Blob&, auto* error) {
        if (!ordered_([&] {
                const auto previous = scene_.CaptureViewport();
                if (!scene_.PublishState(error)) return false;
                // Preserve the existing on-change resize policy. Restoring an
                // identical game viewport must not undo a user's window drag.
                if (resize_ && (previous.width != staged_width_ || previous.height != staged_height_))
                    resize_(staged_width_,staged_height_);
                staged_width_ = staged_height_ = 0;
                return true;
            }))
            throw std::runtime_error("Graphics surface publication lost its validated owner");
        return true;
    };
    if (!restorer.Registry().Register(spec)) return false;
    restorer.AddImagePreflight([this](const auto& sections, auto* error) {
        // Discard a previous attempt even when another image's section is bad.
        staged_width_ = staged_height_ = 0;
        if (!ordered_([&] { scene_.DiscardStagedState(); return true; })) return false;
        for (const auto& section : sections) {
            if (section.tag != Codec::kTag) continue;
            Codec::Snapshot snapshot;
            if (!Codec::Decode(section.payload,snapshot,error) ||
                !ordered_([&] { return scene_.StageState(snapshot,error); })) return false;
            staged_width_ = snapshot.viewport.width;
            staged_height_ = snapshot.viewport.height;
            return true;
        }
        if (legacy_ && legacy_()) return true;   // the admitted frozen image: nothing to stage
        if (error) *error = "Graphics shared surfaces section is required";
        return false;
    });
    if (movie_ && !RegisterPresentation(restorer)) return false;
    registered_ = true;
    return true;
}
} // namespace ps2r::app
