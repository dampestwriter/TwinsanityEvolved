#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/sif/hle/rpc_router.h"

namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {

// Service-only startup (`hle.startup=native`): the IOP never boots. The
// title's startup still runs as recompiled (TR src/platform/ps2/system.cpp
// StartServices: eight sceSifLoadModule requests, the memory card, the pads,
// the IOP heap, the sound remote init), and every request is answered by the
// native owner of that module's intent -- never by a module load, never by an
// IOP kernel:
//
//   SIO2MAN.IRX PADMAN.IRX  -> InputBindings (hle.input_service)
//   MCMAN.IRX   MCSERV.IRX  -> SaveStorage   (hle.save_storage)
//   LIBSD.IRX               -> Audio / SoundOutput (hle.sound_samples + audio_mix), SoundEffects
//   SDRDRV.IRX              -> the libsdr face (SdrRemote) + SoundBanks (bank headers)
//   STREAM.IRX              -> StreamTransport (this class's RPC server for 0x12345)
//   CDVDSTM.IRX             -> FileServices + the Movie disc cursor (hle.files, hle.movie_start)
//
// The returned module id is the owner's handle (1..8 in the title's request
// order); a path none of the owners claims is refused (-1) and counted. The IOP
// heap allocator refuses (0) and counts: with MusicBuffers and the owned bank
// lease selected nothing allocates there, and a refusal is observable.
class NativeStartup {
public:
    struct Owners {
        bool input = false, saves = false, audio = false, banks = false, transport = false, files = false, movie_cursor = false;
    };
    // EE RAM access for the RPC server (the title library holds no EE memory dependency).
    using EeRead = std::function<uint8_t(uint32_t addr)>;
    using EeWrite = std::function<void(uint32_t addr, uint8_t value)>;

    NativeStartup(bool selected, Owners owners, EeRead read, EeWrite write);

    bool Selected() const { return selected_; }
    // sceSifLoadModule's answer for `path` (the title's "cdrom0:\CRASH6\SYS\<MODULE>.IRX").
    int32_t LoadModule(const std::string& path);
    // The STREAM.IRX server face (RpcRouter 0x12345): the title's batch -> StreamTransport::Serve.
    RpcRouter::Server& Server() { return server_; }
    // IOP heap: no IOP, no heap -- every allocation refused and counted.
    uint32_t AllocIopHeap(uint32_t size);
    void FreeIopHeap(uint32_t addr);

    // The loader lift's view of the transport (LoaderSpinLift::IopInstant):
    // every served batch is a completed round trip, and the title's load-wait
    // loop keeps "headroom" while the batches served since the frame began fit
    // the console's per-frame round-trip capacity -- the one hardware datum the
    // lift already carries (Layout::stream_rpc_service_us, 160 us measured per
    // STREAM reply; ~104 per 16.7 ms frame). No service time is charged to the
    // owned FileSchedule position by this: the capacity bounds the laps the way
    // T0's quantum does on the console, nothing else.
    void OnBoundary();                      // the frame began: the batch count is the frame's base
    uint64_t RoundTrips() const;            // StreamTransport batches served so far
    bool TimeLeft(uint64_t service_us, uint64_t frame_us) const;

    struct Census {
        uint64_t loads = 0, refused_loads = 0, heap_allocs_refused = 0, heap_frees = 0, server_calls = 0, server_malformed = 0;
        std::vector<std::string> modules;   // "<MODULE>.IRX:<owner>" in request order
    };
    const Census& Count() const { return census_; }
    // run.meta: `startup=native|retained`, `startup_modules=`, `stream_transport=`,
    // `stream_transport_unowned=`, `sound_effects=`, `iop_heap_native=`.
    void Record(verify::RunMeta& meta) const;

private:
    class TransportServer : public RpcRouter::Server {
    public:
        TransportServer(NativeStartup& owner, EeRead read, EeWrite write)
            : owner_(owner), read_(std::move(read)), write_(std::move(write)) {}
        void Call(const RpcRouter::CallRequest& req) override;
    private:
        NativeStartup& owner_;
        EeRead read_;
        EeWrite write_;
    };
    const char* OwnerOf(const std::string& module) const;

    bool selected_;
    Owners owners_;
    TransportServer server_;
    Census census_;
    uint64_t frame_base_batches_ = 0;
};

}  // namespace ps2r::app
