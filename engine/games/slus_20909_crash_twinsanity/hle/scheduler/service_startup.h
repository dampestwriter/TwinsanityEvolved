#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace title {

// The services owner's startup contract for the startup owner. It
// answers one question per retained IOP module and for the IOP heap: does a
// consumer still need it under the composed selection? The answer is derived
// from actual installed owner state after Install, never from a config key,
// and a retained module must keep executing (no fabricated load success).
class ServiceStartup {
public:
    enum class Module : uint8_t { StreamIrx, Cdvdstm, Libsd, Sdrdrv, IopHeap };
    static constexpr size_t kModules = 5;

    struct Inputs {
        // Title library owners (hle/stream, hle/audio); Current() reads them.
        bool stream_files = false;     // StreamLibrary::Enabled()  — hle.stream_files
        bool sound_banks = false;      // SoundBanks::Enabled()     — hle.sound_bank_headers
        bool sound_controls = false;   // SoundControls::Enabled()  — hle.sound_samples
        bool music = false;            // MusicLibrary::Enabled()   — hle.music
        bool music_buffers = false;    // MusicBuffers::Enabled()   — hle.music_buffers
        // Application owners the root composes (app/host).
        bool disc_files = false;       // FileServices::Enabled()   — hle.files
        bool output = false;           // SoundOutput::OutputOwner() != nullptr — hle.audio_mix
        bool movie_pcm = false;        // MovieSound selected       — hle.movie_lifecycle (Movie owner)
        bool movie_cursor = false;     // MovieStream owns the disc cursor — hle.movie_start (Movie owner)
        // The owners (Current reads them too).
        bool transport = false;        // StreamTransport::Enabled() -- hle.startup=native: the STREAM batch server,
                                       //   heartbeat 260 (voice release is the owned Audio's), EE server 0x1e7f80 idle
        bool effects = false;          // SoundEffects::Enabled() -- effects and global sound controls (0x2c4c30 client)
        bool bank_residency = false;   // SoundBanks ∧ SoundControls: bank bytes served from the owned lease
                                       //   (no AllocIopHeap, libsd_trans=0)
    };
    struct Consumer {
        Module module;
        const char* what;              // the retained consumer, with its title root
    };
    struct Report {
        std::array<bool, kModules> retained{};
        std::vector<Consumer> consumers;
        bool Retained(Module module) const { return retained[static_cast<size_t>(module)]; }
        // True only when STREAM.IRX, CDVDSTM.IRX and the IOP heap have no consumer:
        // the startup owner may then skip InitIopHeap and those loads.
        bool StreamStartupOwned() const;
        // True only when LIBSD.IRX and SDRDRV.IRX have no consumer.
        bool SoundStartupOwned() const;
    };

    // Fill the library fields from the installed owners; root fields are kept.
    static Inputs Current(Inputs root);
    static Report Inspect(const Inputs& inputs);
    static const char* Name(Module module);
};

} // namespace title
