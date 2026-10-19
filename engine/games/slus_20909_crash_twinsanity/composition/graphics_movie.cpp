#include "games/slus_20909_crash_twinsanity/composition/graphics_movie.h"

#include "engine/ee/hle/thread_manager.h"

#include <cstdio>
#include <stdexcept>

#include "engine/core/verify/run_meta.h"

namespace ps2r::app {
namespace {
bool Refuse(std::string* reason, const std::string& why) {
    if (reason) *reason = why;
    return false;
}
std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", value);
    return text;
}
}  // namespace

GraphicsMovie::GraphicsMovie(ipu::GuestMemory& memory, ::title::MovieDecoder& decoder, Hooks hooks,
                             ::title::MovieGraphics::OutputServices output)
    : hooks_(std::move(hooks)), decoder_(decoder),
      movie_(memory, decoder, {hooks_.picture, {}, {},
          [this] { return presenting && hooks_.select && hooks_.select(presenting_); }}, std::move(output)),
      step_(memory, decoder, movie_), start_(memory, decoder, movie_, hooks_.call),
      wait_(memory, decoder, movie_, hooks_.call), stop_(memory, decoder, movie_, hooks_.call) {}
GraphicsMovie::~GraphicsMovie() {
    movie_.Close();
    if (hooks_.selected && installed_) decoder_.SetPictureServices({});
}

std::string GraphicsMovie::MissingServices(uint32_t mask) {
    // MovieDecoder::Services() bit order; the name says which selection or
    // root step supplies the bit.
    constexpr std::array<const char*, 7> names{
        "lifecycle (hle.movie_lifecycle)", "stream services", "input services", "audio services",
        "audio playback (hle.movie_lifecycle)",
        "player buffers (MovieStorage: hle.music_buffers with hle.stream_files, installed before the player owners)",
        "picture services (hle.graphics_movie)"};
    std::string missing;
    for (unsigned bit = 0; bit < names.size(); ++bit)
        if (!(mask & (1u << bit))) missing += (missing.empty() ? "" : ", ") + std::string(names[bit]);
    return missing;
}

bool GraphicsMovie::Install(const ::title::MovieDecoder::Capture& capture,
                            const ::title::MovieDecoder::Installer& install, std::string* reason) {
    if (installed_) return Refuse(reason, "Graphics Movie is already installed");
    if (!capture || !install) return Refuse(reason, "Graphics Movie requires the root capture/install services");
    constexpr std::array<uint32_t, 2> addresses{kInterruptPresent, kPresent};
    if (hooks_.selected) {
        if (!hooks_.picture || !hooks_.submitted || !hooks_.select || !hooks_.clear)
            return Refuse(reason, "selected Movie pictures require the root picture/submission/select/clear services");
        for (size_t i = 0; i < addresses.size(); ++i) {
            originals_[i] = capture(addresses[i]);
            if (!originals_[i]) return Refuse(reason, "Movie presentation transport " + Hex(addresses[i]) + " has no captured body");
        }
    }
    if (hooks_.stop && (!hooks_.start || !hooks_.wait || !hooks_.step))
        return Refuse(reason, "selected Movie Stop requires the paired Start, WaitFrame and Step selections");
    if (hooks_.wait && !hooks_.start)
        return Refuse(reason, "selected Movie WaitFrame requires the Start selection");
    if (movie_.Install(capture, install, hooks_.selected) != 3)
        return Refuse(reason, std::string("Movie pictures refused installation") +
            (hooks_.selected ? " (selected pictures require the installed Movie lifecycle and a live shared scene)" : ""));
    if (!step_.Install(capture, install, hooks_.step))
        return Refuse(reason, !movie_.Selected() ?
            "selected Movie Step requires owned Movie pictures (hle.graphics_movie)" :
            "selected Movie Step requires the Movie lifecycle and audio services (hle.movie_lifecycle)");
    if (hooks_.selected) {
        for (size_t i = 0; i < addresses.size(); ++i)
            if (!install(addresses[i], [this, i](CpuContext& context, uint32_t at) {
                Dispatch(i, context, at);
            })) return Refuse(reason, "Movie presentation transport " + Hex(addresses[i]) + " refused installation"); // root aborts before any game execution
    }
    installed_ = true;
    return true;
}

bool GraphicsMovie::InstallPlayer(const ::title::MovieDecoder::Capture& capture,
                                  const ::title::MovieDecoder::Installer& install, std::string* reason) {
    if (!installed_) return Refuse(reason, "Movie player owners install after the Movie pictures");
    if (player_installed_) return Refuse(reason, "Movie player owners are already installed");
    if (!capture || !install) return Refuse(reason, "Movie player owners require the root capture/install services");
    if (hooks_.stop && (!hooks_.start || !hooks_.wait || !hooks_.step))
        return Refuse(reason, "selected Movie Stop requires the paired Start, WaitFrame and Step selections");
    if (hooks_.wait && !hooks_.start)
        return Refuse(reason, "selected Movie WaitFrame requires the Start selection");
    const auto mask_reason = [&](const char* owner) {
        const uint32_t mask = decoder_.Services();
        return std::string("selected Movie ") + owner + " requires the complete Movie service mask: have " +
            Hex(mask) + ", missing " + MissingServices(mask);
    };
    const auto pictures_reason = [&](const char* owner) {
        return std::string("selected Movie ") + owner + " requires owned Movie pictures (hle.graphics_movie)";
    };
    if (!start_.Install(capture, install, hooks_.start, hooks_.stop)) {
        if (!hooks_.start) return Refuse(reason, "Movie Start has no captured body");
        if (!movie_.Selected()) return Refuse(reason, pictures_reason("Start"));
        return Refuse(reason, decoder_.Services() != 127u ? mask_reason("Start") : "Movie Start refused installation");
    }
    if (!wait_.Install(capture, install, hooks_.wait)) {
        if (!hooks_.wait) return Refuse(reason, "Movie WaitFrame has no captured body");
        if (!movie_.Selected()) return Refuse(reason, pictures_reason("WaitFrame"));
        return Refuse(reason, decoder_.Services() != 127u ? mask_reason("WaitFrame") :
            "Movie WaitFrame refused installation (its retained spin/present bodies are not captured)");
    }
    if (!stop_.Install(capture, install, hooks_.stop)) {
        if (!hooks_.stop) return Refuse(reason, "Movie Stop has no captured body");
        if (!movie_.Selected()) return Refuse(reason, pictures_reason("Stop"));
        return Refuse(reason, decoder_.Services() != 127u ? mask_reason("Stop") : "Movie Stop refused installation");
    }
    player_installed_ = true;
    return true;
}

void GraphicsMovie::Dispatch(size_t index, CpuContext& context, uint32_t entry) {
    constexpr std::array<uint32_t, 2> addresses{kInterruptPresent, kPresent};
    if (entry && entry != addresses[index]) {
        // An interior entry is a restored session resuming the transport after
        // its SyncV (the savestate boundary lives inside Present): the original
        // finishes the frame's presentation exactly as recorded; the owned
        // picture select/release of THAT frame is not re-derived (counted).
        ++interior_resumes_[index];
        originals_[index](context, entry);
        return;
    }
    if (failed_ || presenting) {
        failed_ = true;
        throw std::runtime_error("Graphics Movie presentation requires a complete source boundary");
    }
    const bool selected = index == 0 && movie_.Active();
    Frame frame;
    if (selected && !hooks_.submitted(frame)) {
        failed_ = true;
        throw std::runtime_error("Graphics Movie has no completed source submission");
    }
    ++transport_calls_[index]; // honest retained transport, not producer retirement
    try {
        originals_[index](context, entry);
        if (selected) {
            presenting_ = frame;
            presenting = true;
            const bool accepted = movie_.Present();
            presenting = false;
            if (!accepted) throw std::runtime_error("Graphics Movie presentation refused");
        } else if (!hooks_.clear()) {
            // The release of a presented picture is owned work at the ordered
            // slot like any other group: a refusal (the owned output stopped
            // or faulted elsewhere) releases nothing and is counted; it is not
            // this transport's fault and never waits on or fails the frame.
            ++release_refused_;
        }
    } catch (const ThreadExitUnwind&) {
        // The boundary thread unwinding through Present's SyncV (a session
        // close, or the live F8's quiesce of the game thread at the admitted
        // boundary): not a presentation fault -- the restored session resumes
        // the transport from its recorded continuation (an interior entry).
        presenting = false;
        throw;
    } catch (...) {
        presenting = false;
        failed_ = true;
        throw;
    }
}

bool GraphicsMovie::CanCheckpoint(std::string* reason) const {
    if (!step_.CanCheckpoint(reason)) return false;
    if (!start_.CanCheckpoint(reason)) return false;
    if (!wait_.CanCheckpoint(reason)) return false;
    if (!stop_.CanCheckpoint(reason)) return false;
    if (!movie_.CanCheckpoint(reason)) return false;
    if (!failed_ && !presenting) return true;
    if (reason) *reason = "Graphics Movie owns an active picture/presentation or failed output";
    return false;
}

void GraphicsMovie::Record(verify::RunMeta& meta) const {
    meta.Set("movie_graphics", movie_.Census());
    step_.Record(meta);
    start_.Record(meta);
    wait_.Record(meta);
    stop_.Record(meta);
    meta.Set("graphics_movie_transport", "interrupt_model_calls=" + std::to_string(transport_calls_[0]) +
        ",present_model_calls=" + std::to_string(transport_calls_[1]) +
        ",interior_resumes=" + std::to_string(interior_resumes_[0] + interior_resumes_[1]) +
        ",failed=" + std::to_string(failed_) + ",release_refused=" + std::to_string(release_refused_));
}
}  // namespace ps2r::app
