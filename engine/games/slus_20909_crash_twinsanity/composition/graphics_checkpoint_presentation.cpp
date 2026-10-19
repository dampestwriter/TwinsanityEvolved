#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"

#include <stdexcept>

#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"

namespace ps2r::app {
bool GraphicsCheckpoint::RegisterPresentation(StateRestorer& restorer) {
    using Codec = ::title::PresentationState;
    savestate::SectionSpec spec;
    spec.tag = Codec::kTag; spec.version = Codec::kVersion;
    spec.owner = "Graphics selected presentation";
    spec.required = !legacy_;
    spec.capture_required = true;
    if (legacy_) {
        // A frozen image presents nothing owned: no selected Movie picture, no
        // owned display image (the scanout is the retained pipeline's).
        spec.absent = [this] {
            if (!legacy_()) throw std::runtime_error("Graphics presentation section is missing without an admitted legacy import");
            if (!ordered_([&] {
                    movie_->Clear(); display_->Clear();
                    scene_.CollectAfterQuiescence();
                    return true;
                }))
                throw std::runtime_error("Graphics presentation legacy reset lost its owner");
        };
    }
    spec.serialize = [this](auto& bytes) {
        Codec::Snapshot snapshot;
        std::string error;
        if (!ordered_([&] {
            return movie_->CaptureState(snapshot.movie,&error) &&
                display_->CaptureState(snapshot.display,snapshot.display_source,&error);
        }) || !Codec::Encode(snapshot,bytes,&error))
            throw std::runtime_error("Graphics presentation capture: " + error);
    };
    spec.validate = [](const savestate::Blob& blob,auto* error) {
        Codec::Snapshot snapshot;
        return blob.version == Codec::kVersion && Codec::Decode(blob.payload,snapshot,error);
    };
    spec.restore = [this](const savestate::Blob&,auto* error) {
        if (!ordered_([&] {
            if (!movie_->PublishState(error) || !display_->PublishState(error)) return false;
            scene_.CollectAfterQuiescence(); // old selected leases are now released
            return true;
        }))
            throw std::runtime_error("Graphics presentation lost its staged owner");
        return true;
    };
    if (!restorer.Registry().Register(spec)) return false;
    restorer.AddImagePreflight([this](const auto& sections,auto* error) {
        const savestate::Blob* image = nullptr;
        const savestate::Blob* frame = nullptr;
        for (const auto& blob : sections) {
            if (blob.tag == Codec::kTag) image = &blob;
            if (blob.tag == ::title::FrameState::kTag) frame = &blob;
        }
        return ordered_([&] {
            movie_->DiscardStagedState(); display_->DiscardStagedState();
            if (!image && !frame && legacy_ && legacy_()) return true;   // the admitted frozen image
            Codec::Snapshot snapshot;
            ::title::FrameEnvironment::Snapshot source;
            if (!image || !frame || !Codec::Decode(image->payload,snapshot,error) ||
                !::title::FrameState::Decode(frame->payload,source,error)) return false;
            if (snapshot.movie.frame.epoch > source.source_epoch ||
                snapshot.display_source.epoch > source.source_epoch) {
                if (error) *error = "Graphics selected image is newer than its source owner";
                return false;
            }
            if (!movie_->StageState(snapshot.movie,error) ||
                !display_->StageState(snapshot.display,snapshot.display_source,error)) {
                movie_->DiscardStagedState(); display_->DiscardStagedState();
                return false;
            }
            return true;
        });
    });
    return true;
}
} // namespace ps2r::app
