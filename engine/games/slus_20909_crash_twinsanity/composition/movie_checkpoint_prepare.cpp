#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"

#include "app/host/session_clock.h"
#include "engine/ee/savestate/state_restorer.h"

namespace ps2r::app {
bool MovieCheckpoint::RegisterPreparation(StateRestorer& restorer) {
    if (preparation_required_ || !restorer.Registry().Has(kTag) ||
        !restorer.Registry().Has(SessionClock::kTag)) return false;
    restorer.AddImagePreflight(
        [this](const auto& blobs, auto* error) { return PrepareImage(blobs, error); },
        [this] { DiscardPrepared(); });
    preparation_required_ = true;
    return true;
}

void MovieCheckpoint::DiscardPrepared() {
    prepared_.reset();
    prepared_record_.clear();
}

bool MovieCheckpoint::PrepareImage(const std::vector<savestate::Blob>& blobs, std::string* error) {
    DiscardPrepared();
    if (!CanCheckpoint(error) || !ValidateImage(blobs, error)) return false;
    const savestate::Blob* movie = nullptr;
    const savestate::Blob* clock = nullptr;
    for (const auto& blob : blobs) {
        if (blob.tag == kTag) movie = &blob;
        if (blob.tag == SessionClock::kTag) clock = &blob;
    }
    Snapshot state;
    if (!movie || !Decode(*movie, state, error)) {
        if (error) *error = "Movie preparation requires the complete migrated MOVI owner";
        return false;
    }
    int64_t origin = now_();
    if (clock) {
        SessionClock::Snapshot saved;
        std::string reason;
        if (!SessionClock::Decode(*clock, saved, reason)) {
            if (error) *error = reason;
            return false;
        }
        origin = int64_t(saved.domain.tick);
    } else if (!state.contexts.empty()) {
        if (error) *error = "Movie context preparation requires its saved logical clock";
        return false;
    }
    std::vector<uint8_t> record;
    if (!Encode(state, record, error)) return false;
    auto prepared = software_.PrepareIdle(state.contexts, origin, error);
    if (!prepared) return false;
    prepared_record_ = std::move(record);
    prepared_ = std::move(prepared);
    ++preparations_;
    return true;
}
} // namespace ps2r::app
