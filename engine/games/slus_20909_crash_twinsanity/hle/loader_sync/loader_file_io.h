#pragma once
// LoaderFileIo -- the title's loader file-IO bodies and native bank-header
// lifecycle, bound over the engine's ReadEngine (the disc bytes)
// and LoaderModel (WHEN an asynchronous completion becomes visible: boundary
// slot 4 through the DeliveryMeter; a blocking wait pulls it forward).
//
// The retail bodies (SLUS_209.09):
//   FUN_002ad8f0(path)            open: takes the next handle from the loader's
//                                 counter word ($gp - 0x74d8), registers the file
//                                 with STREAM.IRX (batch cmd 31 {handle, "cdrom0:\NAME"}
//                                 through the recompiled FUN_001e2db0) and resolves
//                                 the path on the disc; returns the handle (-1 when
//                                 the path is not on the disc).
//   FUN_002adb18(fs)              file size of the handle at fs+0x28 (0 unknown),
//                                 after shipping any staged STREAM records
//                                 (FUN_001e2958(0)) -- the retail cadence.
//   FUN_002adb40(handle)          close: STREAM cmd 32 (FUN_001e74a8) then ship; 0.
//   FUN_002aac80(fs, off, size, dest, sync, status)
//                                 windowed read of the handle at fs+0x28 into dest:
//                                 sync != 0 reads inside the call (1); sync == 0
//                                 issues the read (flag 0x10000 raised on fs+0; 0)
//                                 and, re-asked with the same window, answers 0
//                                 while it is in flight, 1 once it was delivered.
//                                 *status = size. The retail body routes the window
//                                 by its size against the callback stream's
//                                 capacity (cb = fs+0x14, cap = cb+0x10): below it,
//                                 the window is DMAed into the ring at base and the
//                                 completion copy is staged (dest at fs+0x10, size at
//                                 fs+0x18); at or above it (or with no stream) the
//                                 window is DMAed straight to dest and nothing is
//                                 staged -- the completion then copies nothing.
//                                 With Deps::ring_cache the body is the retail ring
//                                 rule (below); without it every window below the
//                                 capacity is its own fill at the ring's base.
//
// The ring rule (Deps::ring_cache; retail FUN_002aac80 0x2aacf0-0x2ab128, the
// console trace of the Default.rm2 stream at coldboot). The callback stream is a
// cache of one window of the file: fs+0xc holds the BD-absolute position of the
// first byte of the fill most recently issued into the ring, fs+0 bit 0x40000
// says the ring holds a window, bit 0x10000 says a read is in flight, and the
// ring's bytes once the DMA lands are file[fs+0xc .. +min(eof - fs+0xc, cap)).
//   prologue  the request position pos = fs+0x2c + off is placed inside the
//             window [fs+0xc, fs+0xc + cap): the cursor seeks to base +
//             (pos - fs+0xc) (cb vtbl+0x64); a position outside it rewinds the
//             cursor (vtbl+0x54) and drops 0x40000.
//   hit       0x40000 up and the bytes to the ring's end cover size: the
//             title's copy-out (cb vtbl+0x14 = FUN_002b5dd8: memcpy from the
//             cursor, cursor += n) serves the window; no record is issued; 1.
//   straddle  0x40000 up, size runs past the ring's end: the tail is copied
//             out; with the remainder below cap the cursor rewinds, fs+0xc =
//             pos + tail, ONE record of min(eof - (pos + tail), cap) bytes fills
//             the ring at base and the remainder is staged (fs+0x18 = size -
//             tail, fs+0x10 = dest + tail) for the completion copy; with the
//             remainder at or above cap the 64-byte-aligned tail is copied out,
//             the rest is read straight to dest and 0x40000 drops (fs+0xc kept).
//   miss      0x40000 down, size below cap: rewind, fs+0xc = pos, ONE record
//             of min(eof - pos, cap) bytes at base, the window staged (fs+0x18 =
//             size, fs+0x10 = dest), 0x40000 raised. size at or above cap, or
//             no stream: straight to dest, nothing staged.
//   epilogue  the cursor at the ring's end drops 0x40000.
// A synchronous call (sync != 0) runs the same rule with the fill landed inside
// the call. The seam keeps no ring state of its own: every word above is the
// title's (fs / cb fields in guest memory).
//   FUN_002ad1a0(fs)              wait: with the flag up, completes the in-flight
//                                 read now, asks the title's own poll
//                                 (FUN_002adaf0) and, when it says done, runs the
//                                 staged window's completion copy (cb vtbl+0x14 from
//                                 the ring to fs+0x10, fs+0x18 bytes), clears fs+0x10
//                                 and lowers the flag. The copy + clear run under
//                                 poll visibility (the retail steps; a staging left
//                                 behind is re-run by the poll path on a later direct
//                                 window; boundary mode's lanes are kept.
//
// Archive-relative names (chunks inside CRASH.BD, named by CRASH.BH) are not
// resolved here yet: they open as not-on-disc (-1) and are counted (ArchiveMisses).
//
// The STREAM slot status the title polls (modelled, no hook). On retail an
// asynchronous read is a STREAM.IRX read record on the file's slot (fs+0 low
// byte): the EE-side STREAM client marks the slot "issued" when it stages the
// record (FUN_001e1500: `*(int8*)(0x3b3d00 + slot) = 2`), the module lists the
// slot in every reply while the IOP reads and DMAs the window into the file's
// callback ring at its base, and the client's reply processor (FUN_001e1f48)
// maps the listing to 1 and, at the first reply that no longer lists the slot,
// back to 0. The title's completion poll (FUN_002ad110 -> FUN_002adaf0 ->
// FUN_001e3308) reads that byte: 0 or 3 = done (out+0x11 = 0), 2 or 5 = queued
// (6), anything else = active (1); pending = out+0x11 > 0. Only when done does
// FUN_002ad110 run the completion copy (cb vtable+0x14 = FUN_002b5dd8) from the
// ring to the destination and clear the 0x10000 flag. NEW serves the read on
// the host, so this seam produces the same observable: the async issue writes
// the "issued" byte; the delivery (LoaderModel apply, boundary slot 4, or the
// blocking wait's CompleteNow) fills dest + ring FIRST and then writes the
// "done" byte -- a poll between the two can never see done over stale bytes.
//
// The client's aging of an unlisted slot. FUN_001e1f48 (the reply processor,
// run in the sceSifCallRpc end-callback of every status call) rewrites the
// whole table on every reply: 1 -> 0 (and the slot's countdown 0x3b3d30+slot =
// 3), 2 -> 5, 5 -> countdown-- and 0 (done) when it reaches zero; a slot the
// reply lists is set to 1 (3 -> 4); the post-pass turns 4 -> 3 and 3 -> 0. On
// the console a record in service is listed in every reply, so its byte reads
// 1 until the module stops listing it. A host-served record is unknown to the
// recompiled module: no reply ever lists it, and the client's timeout retires
// the slot (2 -> 5 -> 0) a few replies after the issue -- the title's poll then
// answers done over an undelivered window and the loader SM completes the read
// job without its data (BUG-069: the level header read, four replies, frame
// 306). Under poll visibility the issue therefore marks the slot kSlotServed, a
// value outside the client's state set {0..5}: every reader (FUN_001e3308,
// FUN_001e6aa0/6af0) answers active for it and the processor's passes leave it
// alone, so the byte reads pending exactly until the delivery writes done --
// the console's observable for a record the IOP is serving. Boundary mode keeps
// kSlotIssued (its lanes are frozen; a window there is delivered at the next
// boundary or the wait, before the countdown can retire it).
//
// Module-owned records inside the wait. Not every read record on an fs object
// is host-served: the title also stages STREAM records itself (the original
// FUN_002ab160 retained for interior savestate entries calls the issue primitive
// FUN_002ad9a8 directly -- type 0x7e; its native full entry uses LoadSoundBank;
// FUN_002ad0b0 stages a type-0x7f file segment through FUN_002ada68) and then
// waits on the same fs. Those are served by the recompiled STREAM.IRX: its CD
// worker reads the sectors (sceCdRead), DMAs them into EE RAM, and its reply
// stops listing the slot; the EE-side client maps that to "done" only inside
// the sceSifCallRpc end-callback (FUN_001e6968 -> FUN_001e1f48). On hardware
// the IOP runs concurrently and the SIF interrupt delivers that callback while
// the EE spins in FUN_002ad1a0 (retail: Ship(0) + FUN_001e37c8(slot, 0) per lap
// until fs+2 bit 0 clears). NEW's cooperative IOP kernel runs at frame
// boundaries by design, so the wait -- an EE park loop -- yields to it here:
// when the flag is up, no host-served read is in flight and the slot byte is
// module-owned (kSlotIssued / kSlotQueued / kSlotActive), Deps::pump_module runs
// the kernel's park-loop pump plus the pending RPC end-callbacks, the client's
// own Ship(0) issues the lap's status call (retail's per-lap Ship), then the
// title's own poll decides; bounded by Layout::max_module_laps. The seam writes
// no status and no memory of its own on this path: the module and the client do.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

class ReadEngine;
class LoaderModel;

class LoaderFileIo {
public:
    struct Deps {
        ReadEngine* engine = nullptr;             // null = no disc: every open misses
        LoaderModel* model = nullptr;             // null = async reads complete inside the call
        std::function<uint64_t()> now_tick;       // the domain tick (the read's issue tick)
        std::function<void()> park_frame;         // the same logical wait as the comparison arm
        std::function<void(uint32_t fn, CpuContext&)> call;   // runs a recompiled function
        // One lap of the cooperative IOP for a module-owned record: the kernel's
        // park-loop pump, the EE-side service of a worker's bridged call and the
        // pending sceSifCallRpc end-callbacks as guest calls on this context.
        // Frame-neutral (no frame counted); the IOP clock may advance past one
        // quantum through the bridge's drive. Unbound = the wait returns as the poll says.
        std::function<void(CpuContext&)> pump_module;
        // The title's ring rule (header comment) in place of the per-record fill.
        bool ring_cache = false;
        // Evidence lane: one line per windowed-read request ("req fs= handle= off= size= dest= sync= pos=").
        std::function<void(const std::string&)> trace;
    };
    struct Layout {
        int32_t counter_gp_offset = -0x74d8;      // the loader's handle counter, $gp-relative
        uint32_t stream_open_cmd_addr = 0x309bf4; // STREAM open-record latch (-1 = none staged)
        uint32_t stream_batch_count_addr = 0x3b27ea;   // staged batch count (u16)
        uint32_t fn_register_file = 0x1e2db0;     // EnqueueCmd31(handle, name, 0)
        uint32_t fn_ship = 0x1e2958;              // Ship(0)
        uint32_t fn_unregister_file = 0x1e74a8;   // EnqueueCmd32(handle)
        uint32_t fn_poll = 0x2adaf0;              // the title's completion poll (fs) -> pending
        uint32_t stream_slot_status_addr = 0x3b3d00;   // STREAM client per-slot status (int8[0x30])
        uint32_t stream_slot_count = 0x30;        // slots the client tracks (FUN_001e1f48's loop bound)
        // The slot byte values that mean "a STREAM record the module is serving"
        // (FUN_001e3308's reading: 2/5 queued, 1 active; 0/3 done) and the most
        // IOP laps one wait yields for such a record before returning as the poll says.
        uint8_t module_slot_states[3] = {kSlotActive, kSlotIssued, kSlotQueued};
        uint32_t max_module_laps = 8;
        // The callback stream's vtable: the int16 `this` adjustment the wait adds
        // to cb before the call, and the completion copy (FUN_002b5dd8) slot.
        uint32_t cb_vtbl_this_adjust = 0x10;
        uint32_t cb_vtbl_completion_copy = 0x14;
        uint32_t fn_get_heap = 0x181620;          // GetHeapManager()
        uint32_t fn_free = 0x2045a8;              // FreeMemory(heap, allocation)
        uint32_t fn_alloc_aligned = 0x204530;     // MemoryAllocateAligned(heap, size, alignment)
    };
    // The slot byte values the title's own client writes (see the header comment).
    static constexpr uint8_t kSlotIssued = 2;     // FUN_001e1500: record staged
    static constexpr uint8_t kSlotDone = 0;       // FUN_001e1f48: the module no longer lists the slot
    static constexpr uint8_t kSlotActive = 1;     // FUN_001e1f48: the module's reply lists the slot
    static constexpr uint8_t kSlotQueued = 5;     // FUN_001e3308 reads it as queued (with 2)
    // A host-served window in flight (poll visibility): outside the client's state
    // set, so the reply processor neither ages nor retires it and every reader
    // answers active -- the byte the console's client shows while the IOP serves it.
    static constexpr uint8_t kSlotServed = 0x10;
    static void Bind(Deps deps);
    static void Bind(Deps deps, const Layout& layout);

    static void Open(CpuContext& ctx);
    static void FileSize(CpuContext& ctx);
    static void Close(CpuContext& ctx);
    static void WindowedRead(CpuContext& ctx);
    static void WaitReadComplete(CpuContext& ctx);
    // NTSC FileStreamStartRead: absolute offset within the opened backing file.
    // True when the file service handled the call (including an explicit error).
    static bool StartRead(CpuContext& ctx);
    // NTSC StartSoundBankRead(fs, sound, absolute offset, byte count).
    // No SPU address is used: the typed destination carries the Audio ticket.
    static bool StartSoundRead(CpuContext& ctx, uint64_t generation, uint64_t sequence);
    // Music's requested file window, with an owned logical stream key. No
    // FileStream pointer, IOP allocation, guest DMA or model closure exists.
    // `charged` is the extent the drive is charged before the window is
    // delivered (the stream's buffer fill; 0 = the whole window). The owned
    // bytes are the whole window either way; the remainder is charged as
    // refills through ChargeMusicRefill while the stream consumes.
    static bool StartMusicRead(uint32_t stream, uint64_t generation, uint64_t sequence,
                               int32_t handle, uint32_t offset, uint32_t size, uint32_t charged = 0);
    // The drive extent of a music buffer refill: `offset`/`bytes` within the
    // owned file; nothing is read or delivered. False when the handle is not
    // an owned music file or the extent lies outside it.
    static bool ChargeMusicRefill(int32_t handle, uint64_t offset, uint32_t bytes);
    // Ensures the still-module-owned sound-bank command can use this file.
    static void RegisterForAudio(CpuContext& ctx);

    // NTSC 0x2ab160: LoadSoundBank(SoundBankFiles* bank, const char* name).
    // Loads name.mh natively into bank+0 before replacing the name.mb handle at
    // bank+4 through the existing Open/Close path. The header's temporary stream,
    // RPC read and wait are retired. The dispatch owner must use the original
    // body for interior entries: this entry requires the original a0/a1, not a
    // saved continuation's registers or stack frame.
    static void LoadSoundBank(CpuContext& ctx);
    struct BankHeaderCounters {
        // Misses are missing/empty headers. Failures cover failed allocation,
        // invalid extents/destinations, short reads or an unbound guest caller.
        uint64_t header_calls = 0, sound_bank_headers = 0, header_bytes = 0, header_misses = 0, header_failures = 0;
    };
    // Observer-only atomic loads, outside savestates. A concurrent call can be
    // partially reflected; these counters never drive completion or scheduling.
    static BankHeaderCounters CountBankHeaders();

    struct Census {
        uint64_t opens = 0, misses = 0, archive_misses = 0, closes = 0, sync_reads = 0, async_issued = 0,
                 async_polls_pending = 0, async_polls_done = 0, waits = 0, waits_forced = 0, bytes_read = 0,
                 slot_issued = 0, slot_done = 0,   // STREAM slot status writes (issue / delivery)
                 async_direct = 0,                  // async windows routed straight to dest (size >= ring cap, or no stream)
                 waits_module = 0, module_laps = 0, // waits that yielded to the IOP for a module-owned record / their laps
                 wait_completion_copies = 0,        // waits that ran the title's completion copy and cleared the staging (poll visibility)
                 ring_hits = 0, ring_straddles = 0, ring_misses = 0,   // the ring rule's three paths (Deps::ring_cache)
                 ring_fills = 0;                    // records issued into the ring under the ring rule (one per fill)
    };
    static const Census& Stats();
    static void ResetForTest();
    // The STREAM slot byte of an fs object's slot (the title's poll input); -1 when the slot is out of range.
    static int SlotStatus(uint32_t fs);
    // Whether that byte is one the module is serving (Layout::module_slot_states).
    static bool SlotModuleOwned(uint32_t fs);
    // Polls the model for the windows in flight on the title's behalf: the
    // title's own poll reads the modelled slot byte (pending while the read is
    // in flight), not a seam, so this is where its outcome reaches the model
    // (HeldInflight, the note LoaderModel::ShouldYieldPollSlice consumes).
    // Returns whether a window is still in flight. Touches no guest memory.
    static bool PollInFlight();

    // fs object layout the bodies read/write (retail).
    static constexpr uint32_t kFsFlags = 0x00, kFsSlot = 0x00, kFsRingStart = 0x0c, kFsDest = 0x10, kFsCbObj = 0x14,
                              kFsSize = 0x18, kFsHandle = 0x28, kFsSeek = 0x2c, kFsFileSize = 0x30;
    // The callback stream object at fs+0x14: {vtbl +0, base +8, cursor +0xc, cap +0x10}.
    static constexpr uint32_t kCbBase = 0x08, kCbCursor = 0x0c, kCbCap = 0x10;
    static constexpr uint32_t kFlagReadPending = 0x00010000u;
    static constexpr uint32_t kFlagRingValid = 0x00040000u;
    static constexpr uint32_t kMaxPath = 255;
    static constexpr uint32_t kBogusSizeLimit = 1u << 24;   // a read with no file size past this is refused

    struct File {          // an open handle's disc extent
        uint32_t lba = 0;
        uint32_t size = 0;
        bool found = false;
        std::string path;
    };
    struct Window {        // the asynchronous window in flight on an fs object
        uint32_t dest = 0, size = 0, offset = 0;
    };
    struct State {
        std::map<int32_t, File> files;
        std::map<uint32_t, Window> windows;
        int32_t last_handle = -1;
        StreamLibrary::State streams;
    };
    // Service-owned state only. In-flight deliveries are refused explicitly:
    // the composition owner must not label an opaque callback as persistent.
    static bool CaptureState(State& out);
    static bool ValidateState(const State& state, std::string* error = nullptr);
    static bool RestoreState(const State& state, std::string* error = nullptr);
    static void ResetState();
    static bool ResolveFile(const std::string& path, File& file);

private:
    struct Request;    // one windowed-read call after the common prologue
    static void RingCacheRead(CpuContext& ctx, const Request& r);
    static bool IssueRead(const Request& r, uint64_t pos, uint32_t size, uint32_t dest, bool into_ring);
    static void RingCopyOut(CpuContext& ctx, uint32_t cb, uint32_t dest, uint32_t n);
    static bool RunStagedCompletionCopy(CpuContext& ctx, uint32_t fs);
    static void RegisterWithStream(CpuContext& ctx, int32_t handle, const std::string& path);
    static void ShipStagedRecords(CpuContext& ctx);
    static uint32_t ReadBankHeader(CpuContext& ctx, const std::string& path);
    static void CopyIn(uint32_t dest, const uint8_t* src, uint32_t bytes, uint32_t fill_to);
    static bool ReadBytes(uint32_t lba, uint64_t byte_offset, uint32_t size, uint8_t* dst);
    static void ApplyFileBytes(const FileSchedule::Payload& payload);
};
