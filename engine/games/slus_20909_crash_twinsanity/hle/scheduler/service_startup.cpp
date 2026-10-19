#include "games/slus_20909_crash_twinsanity/hle/scheduler/service_startup.h"

#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_transport.h"

namespace title {
namespace {
void Keep(ServiceStartup::Report& report, ServiceStartup::Module module, const char* what) {
    report.retained[static_cast<size_t>(module)] = true;
    report.consumers.push_back({module, what});
}
}  // namespace

ServiceStartup::Inputs ServiceStartup::Current(Inputs root) {
    root.stream_files = StreamLibrary::Enabled();
    root.sound_banks = SoundBanks::Enabled();
    root.sound_controls = SoundControls::Enabled();
    root.music = MusicLibrary::Enabled();
    root.music_buffers = MusicBuffers::Enabled();
    root.transport = StreamTransport::Enabled();
    root.effects = SoundEffects::Enabled();
    root.bank_residency = SoundBanks::Enabled() && SoundControls::Enabled();
    return root;
}

ServiceStartup::Report ServiceStartup::Inspect(const Inputs& in) {
    using M = Module;
    Report r;
    // STREAM.IRX: every client of the lti_streamer server (bind 0x1e11e8,
    // Ship 0x1e2958, file RPC roots) and the EE callback server 0x1e7f80.
    if (!in.stream_files) Keep(r, M::StreamIrx, "file stream ring consumer and loader (0x2aac80 -> 0x1e2958; bind 0x2ad560 -> 0x1e11e8)");
    if (!in.disc_files) Keep(r, M::StreamIrx, "synchronous file RPC roots (0x17d130 0x17d6b8 0x191c80 0x1cc000 0x1cc1b0 0x169348 -> sceFsSifCallRpc)");
    if (!in.sound_banks) Keep(r, M::StreamIrx, "sound bank load/acknowledge (0x2ada68 0x1e7540 0x1e75d8 0x1e8278)");
    if (!in.sound_controls) Keep(r, M::StreamIrx, "sample control batches (0x1e18e8 0x1e3e80 0x1e6b50 0x1e4038 0x1e6d30 -> Ship 0x1e2958)");
    if (!in.music) Keep(r, M::StreamIrx, "music window/play/interleave (0x1e2ee0 0x1e1500 0x1e2fd0)");
    if (!in.transport) Keep(r, M::StreamIrx, "M4 shared send/reply/status, heartbeat 260 and the EE RPC server thread 0x1e7f80 (StreamTransport, hle.startup=native)");
    // CDVDSTM.IRX: STREAM.IRX streams through it; the Movie's sceCdSt cursor is Movie's.
    if (r.Retained(M::StreamIrx)) Keep(r, M::Cdvdstm, "STREAM.IRX still executing (its disc streaming)");
    if (!in.movie_cursor) Keep(r, M::Cdvdstm, "Movie disc cursor (sceCdSt* under MovieStream, Movie owner)");
    // LIBSD.IRX: voices, controls, music and the common Mixer's SpuCore entry.
    if (!in.sound_controls) Keep(r, M::Libsd, "sample voices and controls through libsd");
    if (!in.music) Keep(r, M::Libsd, "paired music voices through libsd");
    if (!in.output) Keep(r, M::Libsd, "common Mixer enters SpuCore::MixInto (hle.audio_mix off)");
    if (!in.movie_pcm) Keep(r, M::Libsd, "Movie PCM at the core-0 BVOL gain (Movie owner)");
    if (!in.effects) Keep(r, M::Libsd, "M5 effects and global controls (sound remote init 0x2c4c30 client; SoundEffects, hle.startup=native)");
    // SDRDRV.IRX: the libsdr remote (block timer, bank transfers) above libsd.
    if (r.Retained(M::Libsd)) Keep(r, M::Sdrdrv, "LIBSD.IRX still executing (sdr remote over it)");
    if (!in.sound_banks) Keep(r, M::Sdrdrv, "bank header/handle service");
    if (!in.bank_residency) Keep(r, M::Sdrdrv, "M5 .mb residency transfers (libsd_trans)");
    // IOP heap: InitIopHeap 0x17cf60 serves the Movie buffers and bank residency.
    if (!in.music_buffers) Keep(r, M::IopHeap, "Movie Prepare/OpenStream buffers (0x2ae1c8 0x2ae860 -> AllocIopHeap; 0x2ae658 0x2af5d0 free)");
    if (!in.bank_residency) Keep(r, M::IopHeap, "M5 sound bank .mb residency (TR game/filestream.cpp:634 load into IOP memory)");
    return r;
}

bool ServiceStartup::Report::StreamStartupOwned() const {
    return !Retained(Module::StreamIrx) && !Retained(Module::Cdvdstm) && !Retained(Module::IopHeap);
}

bool ServiceStartup::Report::SoundStartupOwned() const {
    return !Retained(Module::Libsd) && !Retained(Module::Sdrdrv);
}

const char* ServiceStartup::Name(Module module) {
    switch (module) {
    case Module::StreamIrx: return "STREAM.IRX";
    case Module::Cdvdstm: return "CDVDSTM.IRX";
    case Module::Libsd: return "LIBSD.IRX";
    case Module::Sdrdrv: return "SDRDRV.IRX";
    case Module::IopHeap: return "IOP heap";
    }
    return "?";
}

} // namespace title
