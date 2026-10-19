#include "games/slus_20909_crash_twinsanity/composition/movie_boot.h"

#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "app/host/session_clock.h"
#include "engine/ee/savestate/state_restorer.h"

namespace ps2r::app {

MovieBoot::MovieBoot(MovieCheckpoint& movie, const InputSave& input, const SessionClock& clock)
    : movie_(movie), input_(input), clock_(clock) {}

bool MovieBoot::PrepareFile(const std::string& path, const InputSave::Checkpoint::LegacyPolicy& legacy,
                            std::string& error) {
    // A failed second preparation must not leave a previous file authorized.
    prepared_ = false;
    savestate::FileMeta meta;
    std::vector<savestate::Blob> original, movie_owned, owned;
    if (!savestate::ReadStateFile(path, &meta, &original, &error)) return false;
    bool imported = false;
    if (!movie_.PrepareBoot(original, movie_owned, imported, &error)) return false;
    auto approved = legacy;
    approved.approved = approved.approved && imported;
    if (!clock_.ValidateBootImage(meta, original, approved.approved, error) ||
        !input_.PrepareBoot(movie_owned, approved, owned, error) ||
        !movie_.ValidateImage(owned, &error)) return false;
    auto image = savestate::SerializeStateFile(meta, owned);
    image_ = std::move(image);
    meta_ = meta;
    imported_ = imported;
    prepared_ = true;
    return true;
}

bool MovieBoot::Restore(StateRestorer& restorer, std::string& error) {
    if (!prepared_) {
        error = "Movie boot image was not prepared or was already consumed";
        return false;
    }
    prepared_ = false;
    const auto before = movie_.Restored();
    if (!restorer.RestoreImage(image_.data(), image_.size(), &error)) return false;
    // Diagnostic only: real MOVI applications count as restored. A synthesized
    // OLD import additionally counts its approved migration after success.
    if (imported_ && movie_.Restored() == before + 1) ++movie_.migrated_;
    return true;
}

}  // namespace ps2r::app
