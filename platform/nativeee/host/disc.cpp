// platform/nativeee (host side) -- disc.h: Platform::Disc over the engine's IsoFs. The disc image path is the
// manifest (te::Settings::iso_path): opened once, read at RUN time by Files, the stream façade and the runner's ELF
// load. The libcdvd rows' constants are kept (SCECdComplete 2 / SCECdNotReady 6), nothing waits: the image is there
// or it is not.
#include <cstdio>

#include "engine/cdvd/iso_fs.h"
#include "ps2e_seam.h"
#include "te_services.h"

namespace te {

Disc::Disc(const Settings& settings) : iso_(new IsoFs), path_(settings.iso_path) {}
Disc::~Disc() = default;

int32_t Disc::Initialise() {
    if (opened_) return 1;
    if (tried_) return 0;
    tried_ = true;
    if (path_.empty()) { error_ = "no disc image (te::Settings::iso_path empty)"; return 0; }
    opened_ = iso_->Open(path_, &error_);
    if (!opened_ && GetSettings().verbose) std::fprintf(stderr, "[twinsanityevolved] disc: %s\n", error_.c_str());
    return opened_ ? 1 : 0;
}

int32_t Disc::SetMedia(int32_t media) {
    // sceCdMmode: 1 (Cd) / 2 (Dvd) accepted, returns 1; anything else is refused with 0.
    if (media != 1 && media != 2) return 0;
    media_ = media;
    return 1;
}

int32_t Disc::WaitReady() const { return opened_ ? 2 : 6; }

bool Disc::IsOpen() const { return opened_; }

}  // namespace te

extern "C" {

ps2e_s32 ps2e_disc_initialise(void) { return te::Services::Get().disc().Initialise(); }
ps2e_s32 ps2e_disc_set_media(ps2e_s32 media) { return te::Services::Get().disc().SetMedia(media); }
ps2e_s32 ps2e_disc_wait_ready(void) { return te::Services::Get().disc().WaitReady(); }

}  // extern "C"
