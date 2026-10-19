#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/timing/tick_profile.h"

namespace {

LoaderFileIo::Deps g_deps;
LoaderFileIo::Layout g_layout;
LoaderFileIo::Census g_census;
std::atomic<uint64_t> g_header_calls{0}, g_sound_bank_headers{0}, g_header_bytes{0},
                      g_header_misses{0}, g_header_failures{0};
std::map<int32_t, LoaderFileIo::File> g_files;      // open handles -> disc extent
std::map<uint32_t, LoaderFileIo::Window> g_windows; // fs object -> the window in flight
int32_t g_last_handle = -1;

uint32_t A(const CpuContext& ctx, int i) { return ctx.cpuRegs.GPR.r[4 + i].UL[0]; }
void Ret(CpuContext& ctx, uint32_t v) { ctx.cpuRegs.GPR.r[2].UD[0] = v; }
void RetS(CpuContext& ctx, int32_t v) { ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(static_cast<int64_t>(v)); }
bool InRam(uint32_t a) { return a >= 0x10000u && a < 0x02000000u; }
bool ReadInFlight(uint32_t object) {
    if (StreamLibrary::OwnsRead(object)) return StreamLibrary::Scheduling()->InFlight(object);
    return g_deps.model && g_deps.model->InFlight(object);
}
bool PollReadPending(uint32_t object) {
    if (StreamLibrary::OwnsRead(object))
        return StreamLibrary::Scheduling()->Poll(object) == FileSchedule::PollOutcome::HeldInflight;
    return g_deps.model && g_deps.model->Poll(object) == LoaderModel::PollOutcome::HeldInflight;
}

int32_t NextFileHandle(const CpuContext& ctx) {
    const uint32_t counter = ctx.cpuRegs.GPR.r[28].UL[0] + static_cast<uint32_t>(g_layout.counter_gp_offset);
    int32_t handle = memory::read<int32_t>(counter);
    if (handle < 0) handle = 0;
    memory::write<uint32_t>(counter, static_cast<uint32_t>(handle) + 1u);
    return handle;
}

std::string ReadPath(uint32_t addr) {
    std::string path;
    for (uint32_t i = 0; i < LoaderFileIo::kMaxPath; ++i) {
        const char c = static_cast<char>(memory::read<uint8_t>(addr + i));
        if (c == 0) break;
        path += c;
    }
    return path;
}

// The disc image records ISO9660 identifiers in upper case with ";1"; the title
// names files with either slash. Archive-relative names (no dot in the last
// component, or a component that is not on the disc) miss here.
bool FindOnDisc(const std::string& path, LoaderFileIo::File& out) {
    if (!g_deps.engine) return false;
    std::string p;
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        p += c;
    }
    IsoFs::Entry e;
    if (!g_deps.engine->Find(p, e) && !g_deps.engine->Find(p + ";1", e)) return false;
    if (e.directory) return false;
    out.lba = e.lba;
    out.size = e.size;
    out.found = true;
    return true;
}

}  // namespace

void LoaderFileIo::Bind(Deps deps) { Bind(std::move(deps), Layout{}); }
void LoaderFileIo::Bind(Deps deps, const Layout& layout) {
    g_deps = std::move(deps);
    g_layout = layout;
    StreamLibrary::Bind(g_deps.model);
    StreamLibrary::BindFiles(g_deps.now_tick, ApplyFileBytes, g_deps.park_frame);
}
const LoaderFileIo::Census& LoaderFileIo::Stats() { return g_census; }
LoaderFileIo::BankHeaderCounters LoaderFileIo::CountBankHeaders() {
    return {g_header_calls.load(std::memory_order_relaxed),
            g_sound_bank_headers.load(std::memory_order_relaxed),
            g_header_bytes.load(std::memory_order_relaxed),
            g_header_misses.load(std::memory_order_relaxed),
            g_header_failures.load(std::memory_order_relaxed)};
}
void LoaderFileIo::ResetForTest() {
    ResetState();
    g_census = Census{};
    g_header_calls.store(0, std::memory_order_relaxed);
    g_sound_bank_headers.store(0, std::memory_order_relaxed);
    g_header_bytes.store(0, std::memory_order_relaxed);
    g_header_misses.store(0, std::memory_order_relaxed);
    g_header_failures.store(0, std::memory_order_relaxed);
}
void LoaderFileIo::ResetState() {
    StreamLibrary::Reset();
    g_files.clear();
    g_windows.clear();
    g_last_handle = -1;
}
bool LoaderFileIo::CaptureState(State& out) {
    State state;
    if (!StreamLibrary::CaptureState(state.streams)) return false;
    // A retained sample/module read is outside this file snapshot. Ordinary
    // selected reads have no LoaderModel entry and remain fully capturable.
    for (const auto& window : g_windows)
        if (g_deps.model && g_deps.model->InFlight(window.first)) return false;
    state.files = g_files;
    state.windows = g_windows;
    state.last_handle = g_last_handle;
    out = std::move(state);
    return true;
}
bool LoaderFileIo::ValidateState(const State& state, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!StreamLibrary::ValidateState(state.streams)) return fail("stream ownership or selection mismatch");
    if (state.files.size() > 65536 || state.windows.size() > 65536 || state.last_handle < -1)
        return fail("invalid file-state extent");
    for (const auto& entry : state.files) {
        const auto& file = entry.second;
        if (entry.first < 0 || file.path.empty() || file.path.size() > kMaxPath ||
            file.path.find('\0') != std::string::npos) return fail("invalid saved file identity");
        File current;
        if (FindOnDisc(file.path, current) != file.found ||
            (file.found && (current.lba != file.lba || current.size != file.size)))
            return fail("saved file does not match mounted media");
    }
    if (state.streams.enabled && state.streams.files.size() != state.files.size())
        return fail("incomplete file ownership");
    for (const auto& owned : state.streams.files) {
        const auto file = state.files.find(owned.first);
        if (file == state.files.end()) return fail("owned handle has no file identity");
        if (owned.second.sample != StreamLibrary::IsSampleFile(file->second.path))
            return fail("saved file classification mismatch");
    }
    for (const auto& entry : state.windows) {
        const auto& window = entry.second;
        if (entry.first < 0x10000 || entry.first > 0x02000000 - 0x48 ||
            !InRam(window.dest) || window.size > 0x02000000 - window.dest)
            return fail("invalid saved read window");
    }
    if (state.streams.has_schedule) {
        for (const auto& entry : state.streams.schedule.reads) {
            const auto& request = *entry.second.payload;
            const auto file = state.files.find(request.handle);
            const auto owner = state.streams.files.find(request.handle);
            const auto extent = (request.file_offset % IsoFs::kSectorSize + request.bytes.size() +
                                 IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
            // A music window charges its buffer fill (a prefix of the extent)
            // before readiness; every other destination charges its whole extent.
            const bool music = request.kind == FileSchedule::Destination::Music;
            if (file == state.files.end() || owner == state.streams.files.end() ||
                !file->second.found || request.file_offset > file->second.size ||
                request.bytes.size() > file->second.size - request.file_offset ||
                entry.second.lba != file->second.lba + request.file_offset / IsoFs::kSectorSize ||
                (music ? entry.second.sectors == 0 || entry.second.sectors > extent
                       : entry.second.sectors != extent))
                return fail("owned read disagrees with its file identity");
        }
    }
    return true;
}
bool LoaderFileIo::RestoreState(const State& state, std::string* error) {
    if (!ValidateState(state, error)) return false;
    if (!StreamLibrary::RestoreState(state.streams)) return false;
    StreamLibrary::Bind(g_deps.model);
    g_files = state.files;
    g_windows = state.windows;
    g_last_handle = state.last_handle;
    return true;
}
bool LoaderFileIo::ResolveFile(const std::string& path, File& file) {
    File resolved;
    resolved.path = path;
    if (!FindOnDisc(path, resolved)) return false;
    file = std::move(resolved);
    return true;
}

namespace {

// The STREAM client's per-slot status byte for an fs object's slot (fs+0 low
// byte); out of range when the slot is not one the client tracks (the poll
// itself answers -1 for such a slot and never reads the table).
uint32_t SlotStatusAddr(uint32_t fs) {
    const uint32_t slot = memory::read<uint8_t>(fs + LoaderFileIo::kFsSlot);
    if (slot >= g_layout.stream_slot_count) return 0;
    return g_layout.stream_slot_status_addr + slot;
}

}  // namespace

int LoaderFileIo::SlotStatus(uint32_t fs) {
    const uint32_t a = SlotStatusAddr(fs);
    return a ? static_cast<int>(memory::read<uint8_t>(a)) : -1;
}

bool LoaderFileIo::SlotModuleOwned(uint32_t fs) {
    const int s = SlotStatus(fs);
    if (s < 0) return false;
    for (uint8_t v : g_layout.module_slot_states)
        if (s == v) return true;
    return false;
}

bool LoaderFileIo::PollInFlight() {
    if (auto* schedule = StreamLibrary::Scheduling()) {
        for (const auto& window : g_windows)
            if (schedule->Poll(window.first) == FileSchedule::PollOutcome::HeldInflight) return true;
        return false;
    }
    if (!g_deps.model) return false;
    for (const auto& w : g_windows) {
        if (g_deps.model->Poll(w.first) == LoaderModel::PollOutcome::HeldInflight) return true;
    }
    return false;
}

// Retail builds "cdrom0:\NAME" in its own frame and hands it to EnqueueCmd31;
// the string is copied into the STREAM batch record, so the frame is transient.
// The body runs below the caller's $sp exactly where retail's frame stood.
void LoaderFileIo::RegisterWithStream(CpuContext& ctx, int32_t handle, const std::string& path) {
    if (!g_deps.call) return;
    std::string wire = "cdrom0:\\";
    for (char c : path) wire += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    constexpr uint32_t kFrame = 0x120;
    const uint32_t sp = ctx.cpuRegs.GPR.r[29].UL[0];
    const uint32_t frame = (sp - kFrame) & ~0xFu;
    const uint32_t buf = frame + 0x10;
    const uint32_t n = static_cast<uint32_t>(wire.size() < 262 ? wire.size() : 262);
    for (uint32_t i = 0; i < n; ++i) memory::write<uint8_t>(buf + i, static_cast<uint8_t>(wire[i]));
    memory::write<uint8_t>(buf + n, 0);
    ctx.cpuRegs.GPR.r[29].UD[0] = frame;
    ctx.cpuRegs.GPR.r[4].UD[0] = static_cast<uint64_t>(static_cast<int64_t>(handle));
    ctx.cpuRegs.GPR.r[5].UD[0] = buf;
    ctx.cpuRegs.GPR.r[6].UD[0] = 0;
    g_deps.call(g_layout.fn_register_file, ctx);
    ctx.cpuRegs.GPR.r[29].UD[0] = sp;
}

// Ship(0) only when a record is staged (the open-record latch or a non-zero
// batch count): the size query sits on the loader's hot poll path.
void LoaderFileIo::ShipStagedRecords(CpuContext& ctx) {
    if (!g_deps.call) return;
    const int32_t open_cmd = memory::read<int32_t>(g_layout.stream_open_cmd_addr);
    const uint16_t staged = memory::read<uint16_t>(g_layout.stream_batch_count_addr);
    if (open_cmd == -1 && staged == 0) return;
    ctx.cpuRegs.GPR.r[4].UD[0] = 0;
    g_deps.call(g_layout.fn_ship, ctx);
}

void LoaderFileIo::Open(CpuContext& ctx) {
    ++g_census.opens;
    const std::string path = ReadPath(A(ctx, 0));
    const int32_t handle = NextFileHandle(ctx);
    if (StreamLibrary::Opened(handle, path)) RegisterWithStream(ctx, handle, path);
    File f;
    f.path = path;
    if (FindOnDisc(path, f)) {
        g_files[handle] = f;
        g_last_handle = handle;
        RetS(ctx, handle);
        return;
    }
    ++g_census.misses;
    const size_t last = path.find_last_of("\\/");
    const std::string leaf = last == std::string::npos ? path : path.substr(last + 1);
    if (leaf.find('.') == std::string::npos) ++g_census.archive_misses;
    g_files[handle] = f;
    g_last_handle = handle;
    RetS(ctx, -1);
}

void LoaderFileIo::FileSize(CpuContext& ctx) {
    const uint32_t fs = A(ctx, 0);
    int32_t handle = -1;
    if (InRam(fs)) handle = memory::read<int32_t>(fs + kFsHandle);
    uint32_t size = 0;
    auto it = handle >= 0 ? g_files.find(handle) : g_files.end();
    // The retail query has no argument: when this isn't an explicit fs query,
    // it describes the most recent open. Audio opens retain their Ship contract.
    const int32_t queried = it != g_files.end() ? handle : g_last_handle;
    if (!StreamLibrary::FileOwned(queried)) ShipStagedRecords(ctx);
    if (it != g_files.end() && it->second.found) {
        size = it->second.size;
    } else if (g_last_handle >= 0) {
        auto last = g_files.find(g_last_handle);
        if (last != g_files.end() && last->second.found) size = last->second.size;
    }
    Ret(ctx, size);
}

void LoaderFileIo::Close(CpuContext& ctx) {
    ++g_census.closes;
    const int32_t handle = ctx.cpuRegs.GPR.r[4].SL[0];
    const bool registered = StreamLibrary::FileRegistered(handle);
    if (StreamLibrary::FileOwned(handle)) {
        for (auto it = g_windows.begin(); it != g_windows.end();) {
            const uint32_t fs = it->first;
            if (memory::read<int32_t>(fs + kFsHandle) != handle) { ++it; continue; }
            memory::write<uint32_t>(fs + kFsDest, 0);
            memory::write<uint32_t>(fs + kFsFlags,
                                    memory::read<uint32_t>(fs + kFsFlags) & ~(kFlagReadPending | kFlagRingValid));
            it = g_windows.erase(it);
        }
    }
    // Retire the old descriptor before cancellation callbacks can reopen it.
    // No post-callback erase may remove a new incarnation of the same handle.
    g_files.erase(handle);
    if (g_last_handle == handle) g_last_handle = -1;
    StreamLibrary::Closing(handle);
    if (handle >= 0 && registered && g_deps.call) {
        ctx.cpuRegs.GPR.r[4].UD[0] = static_cast<uint64_t>(static_cast<int64_t>(handle));
        g_deps.call(g_layout.fn_unregister_file, ctx);
        ctx.cpuRegs.GPR.r[4].UD[0] = 0;
        g_deps.call(g_layout.fn_ship, ctx);
    }
    Ret(ctx, 0);
}

void LoaderFileIo::RegisterForAudio(CpuContext& ctx) {
    const uint32_t fs = A(ctx, 0);
    g_windows.erase(fs);
    const int32_t handle = memory::read<int32_t>(fs + kFsHandle);
    const auto it = g_files.find(handle);
    if (it == g_files.end() || StreamLibrary::FileRegistered(handle)) return;
    if (!g_deps.call) {
        StreamLibrary::ReadError();
        return;
    }
    RegisterWithStream(ctx, handle, it->second.path);
    StreamLibrary::Registered(handle);
}

// PAL src/game/filestream.cpp LoadSoundBank; independently matched to NTSC
// FUN_002ab160. The header is a blocking, whole-file load: no caller can observe
// its temporary FileStream's pending state. Only the completed header survives.
uint32_t LoaderFileIo::ReadBankHeader(CpuContext& ctx, const std::string& path) {
    File file;
    if (!FindOnDisc(path, file) || file.size == 0) {
        g_header_misses.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    // A header must fit in EE main memory; reject a corrupt extent before a
    // host allocation or a guest copy can wrap.
    if (file.size > 0x02000000u - 0x10000u) {
        g_header_failures.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g_deps.call(g_layout.fn_get_heap, ctx);
    const uint32_t heap = ctx.cpuRegs.GPR.r[2].UL[0];
    ctx.cpuRegs.GPR.r[4].UD[0] = heap;
    ctx.cpuRegs.GPR.r[5].UD[0] = file.size;
    ctx.cpuRegs.GPR.r[6].UD[0] = 0x40;
    g_deps.call(g_layout.fn_alloc_aligned, ctx);
    const uint32_t header = ctx.cpuRegs.GPR.r[2].UL[0];
    if (header == 0) {
        g_header_failures.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const bool valid = InRam(header) && (header & 0x3fu) == 0 && file.size <= 0x02000000u - header;
    if (valid) {
        const uint32_t sectors = (file.size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
        std::vector<uint8_t> payload(static_cast<size_t>(sectors) * IsoFs::kSectorSize);
        if (g_deps.engine->ReadSectors(file.lba, sectors, payload.data()) == sectors) {
            CopyIn(header, payload.data(), file.size, file.size);
            g_header_bytes.fetch_add(file.size, std::memory_order_relaxed);
            return header;
        }
    }
    // A failed/short read never publishes a partially populated header.
    ctx.cpuRegs.GPR.r[4].UD[0] = heap;
    ctx.cpuRegs.GPR.r[5].UD[0] = header;
    g_deps.call(g_layout.fn_free, ctx);
    g_header_failures.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void LoaderFileIo::LoadSoundBank(CpuContext& ctx) {
    g_header_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_deps.call) {
        g_header_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t bank = A(ctx, 0);
    const std::string name = ReadPath(A(ctx, 1));
    const auto saved_sp = ctx.cpuRegs.GPR.r[29];
    const auto saved_ra = ctx.cpuRegs.GPR.r[31];
    // Keep the sample path above the nested guest calls' stack frames.
    const uint32_t frame = (saved_sp.UL[0] - 0x120u) & ~0xfu;
    ctx.cpuRegs.GPR.r[29].UD[0] = frame;

    const uint32_t old_header = memory::read<uint32_t>(bank);
    if (old_header != 0) {
        g_deps.call(g_layout.fn_get_heap, ctx);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UL[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = old_header;
        g_deps.call(g_layout.fn_free, ctx);
    }
    memory::write<uint32_t>(bank, 0);
    // The title consumes a file number for .mh even when it is missing. Keep
    // the following .mb handle's numbering without registering .mh with STREAM.
    NextFileHandle(ctx);
    const uint32_t header = ReadBankHeader(ctx, name + ".mh");
    memory::write<uint32_t>(bank, header);
    if (header != 0) g_sound_bank_headers.fetch_add(1, std::memory_order_relaxed);

    // The samples are still used by the music/voice stream commands. Preserve
    // their close/Ship and open staging; no global RPC or Ship bypass belongs
    // to the header replacement.
    const int32_t samples = memory::read<int32_t>(bank + 4);
    if (samples != -1) {
        ctx.cpuRegs.GPR.r[4].SD[0] = samples;
        Close(ctx);
    }
    const std::string samples_path = name + ".mb";
    const uint32_t path_addr = frame + 0x10;
    CopyIn(path_addr, reinterpret_cast<const uint8_t*>(samples_path.c_str()),
           static_cast<uint32_t>(samples_path.size() + 1), static_cast<uint32_t>(samples_path.size() + 1));
    ctx.cpuRegs.GPR.r[4].UD[0] = path_addr;
    Open(ctx);
    memory::write<uint32_t>(bank + 4, ctx.cpuRegs.GPR.r[2].UL[0]);
    ctx.cpuRegs.GPR.r[29] = saved_sp;
    ctx.cpuRegs.GPR.r[31] = saved_ra;
}

bool LoaderFileIo::ReadBytes(uint32_t lba, uint64_t byte_offset, uint32_t size, uint8_t* dst) {
    std::memset(dst, 0, size);
    if (!g_deps.engine) return false;
    if (size == 0) return true;
    const uint32_t first = lba + static_cast<uint32_t>(byte_offset / IsoFs::kSectorSize);
    const uint32_t skew = static_cast<uint32_t>(byte_offset % IsoFs::kSectorSize);
    const uint32_t sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
    std::vector<uint8_t> raw(static_cast<size_t>(sectors) * IsoFs::kSectorSize, 0);
    const uint32_t got = g_deps.engine->ReadSectors(first, sectors, raw.data());
    std::memcpy(dst, raw.data() + skew, size);
    return got == sectors;
}

void LoaderFileIo::CopyIn(uint32_t dest, const uint8_t* src, uint32_t bytes, uint32_t fill_to) {
    for (uint32_t i = 0; i < bytes; ++i) memory::write<uint8_t>(dest + i, src[i]);
    for (uint32_t i = bytes; i < fill_to; ++i) memory::write<uint8_t>(dest + i, 0);
}
void LoaderFileIo::ApplyFileBytes(const FileSchedule::Payload& payload) {
    const auto size = static_cast<uint32_t>(payload.bytes.size());
    CopyIn(payload.destination, payload.bytes.data(), size, size);
    if (payload.ring_base) {
        if (payload.ring_base != payload.destination) CopyIn(payload.ring_base, payload.bytes.data(), size, size);
        memory::write<uint32_t>(payload.ring_cursor, payload.ring_base);
    }
}

struct LoaderFileIo::Request {
    uint32_t fs = 0, offset = 0, size = 0, dest = 0, sync = 0, status = 0;
    uint32_t lba = 0;            // the file's first sector (the archive's when the handle is unknown)
    uint64_t byte_offset = 0;    // the window's first byte from that sector: fs+0x2c + offset
};

bool LoaderFileIo::StartRead(CpuContext& ctx) {
    const uint32_t fs = A(ctx, 0);
    if (fs < 0x10000 || fs > 0x02000000 - 0x48) return false;
    const int32_t handle = memory::read<int32_t>(fs + kFsHandle);
    if (!StreamLibrary::FileOwned(handle)) return false;
    const auto it = g_files.find(handle);
    Request r;
    r.fs = fs; r.offset = A(ctx, 1); r.size = A(ctx, 2); r.dest = A(ctx, 3);
    if (it == g_files.end() || !it->second.found || !g_deps.engine ||
        r.offset > it->second.size || r.size > it->second.size - r.offset ||
        !InRam(r.dest) || r.size > 0x02000000u - r.dest) {
        StreamLibrary::ReadError();
        RetS(ctx, -1);
        return true;
    }
    // A stream has one active request. Finish the previous handover before
    // reusing its buffer, even when a caller overlaps a blocking request.
    const auto identity = it->second;
    const auto generation = StreamLibrary::FileGeneration(handle);
    const auto saved = ctx.cpuRegs;
    WaitReadComplete(ctx);
    ctx.cpuRegs = saved;
    if (memory::read<int32_t>(fs + kFsHandle) != handle ||
        StreamLibrary::FileGeneration(handle) != generation || !StreamLibrary::FileOwned(handle)) {
        StreamLibrary::ReadError();
        RetS(ctx, -1);
        return true;
    }
    memory::write<uint32_t>(fs + kFsFlags, memory::read<uint32_t>(fs + kFsFlags) | kFlagReadPending);
    r.lba = identity.lba;
    r.byte_offset = r.offset; // StartRead already receives the archive's offset.
    RetS(ctx, IssueRead(r, r.offset, r.size, r.dest, false) ? 0 : -1);
    return true;
}

bool LoaderFileIo::StartSoundRead(CpuContext& ctx, uint64_t generation, uint64_t sequence) {
    const uint32_t fs = A(ctx, 0), sound = A(ctx, 1), offset = A(ctx, 2), size = A(ctx, 3);
    RetS(ctx, -1);
    if (fs < 0x10000 || fs > 0x02000000 - 0x48) return false;
    const auto handle = memory::read<int32_t>(fs + kFsHandle);
    const auto file = g_files.find(handle);
    if (!StreamLibrary::FileOwned(handle) || file == g_files.end() || !file->second.found ||
        !g_deps.engine || size == 0 || size % 16 || offset > file->second.size ||
        size > file->second.size - offset) {
        StreamLibrary::ReadError();
        return false;
    }
    const auto identity = file->second;
    const auto generation_before_wait = StreamLibrary::FileGeneration(handle);
    const auto saved = ctx.cpuRegs;
    WaitReadComplete(ctx);
    ctx.cpuRegs = saved;
    const auto current = g_files.find(handle);
    if (!StreamLibrary::FileOwned(handle) || current == g_files.end() ||
        memory::read<int32_t>(fs + kFsHandle) != handle ||
        StreamLibrary::FileGeneration(handle) != generation_before_wait ||
        current->second.path != identity.path || current->second.lba != identity.lba ||
        current->second.size != identity.size) {
        StreamLibrary::ReadError();
        RetS(ctx, -1);
        return false;
    }
    FileSchedule::Payload request;
    request.object = fs; request.handle = handle; request.file_offset = offset;
    request.kind = FileSchedule::Destination::Sound;
    request.sound = sound; request.generation = generation; request.sequence = sequence;
    request.bytes.resize(size);
    if (!ReadBytes(identity.lba, offset, size, request.bytes.data()) ||
        !StreamLibrary::SubmitFile(std::move(request), identity.lba + offset / IsoFs::kSectorSize,
            (offset % IsoFs::kSectorSize + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize,
            g_deps.now_tick ? g_deps.now_tick() : 0)) {
        StreamLibrary::ReadError();
        RetS(ctx, -1);
        return false;
    }
    memory::write<uint32_t>(fs + kFsFlags, memory::read<uint32_t>(fs + kFsFlags) | kFlagReadPending);
    RetS(ctx, 0);
    return true;
}

bool LoaderFileIo::StartMusicRead(uint32_t stream, uint64_t generation, uint64_t sequence,
                                 int32_t handle, uint32_t offset, uint32_t size, uint32_t charged) {
    const auto file = g_files.find(handle);
    if (!StreamLibrary::MusicFileOwned(handle) || file == g_files.end() || !file->second.found ||
        !g_deps.engine || stream >= 48 || !generation || !sequence || !size ||
        offset > file->second.size || size > file->second.size - offset) {
        StreamLibrary::ReadError();
        return false;
    }
    FileSchedule::Payload request;
    request.kind = FileSchedule::Destination::Music;
    request.object = FileSchedule::MusicObject(stream); request.stream = stream;
    request.handle = handle; request.file_offset = offset;
    request.generation = generation; request.sequence = sequence;
    request.bytes.resize(size);
    const auto lba = file->second.lba;
    const uint32_t fill = charged && charged < size ? charged : size;
    if (!ReadBytes(lba, offset, size, request.bytes.data()) ||
        !StreamLibrary::SubmitFile(std::move(request), lba + offset / IsoFs::kSectorSize,
            static_cast<uint32_t>((static_cast<uint64_t>(offset % IsoFs::kSectorSize) +
                fill + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize),
            g_deps.now_tick ? g_deps.now_tick() : 0)) {
        StreamLibrary::ReadError();
        return false;
    }
    return true;
}
bool LoaderFileIo::ChargeMusicRefill(int32_t handle, uint64_t offset, uint32_t bytes) {
    const auto file = g_files.find(handle);
    auto* schedule = StreamLibrary::Scheduling();
    if (!schedule || !StreamLibrary::MusicFileOwned(handle) || file == g_files.end() || !file->second.found ||
        !bytes || offset > file->second.size || bytes > file->second.size - offset) return false;
    schedule->ChargeRead(static_cast<uint32_t>(file->second.lba + offset / IsoFs::kSectorSize),
                         static_cast<uint32_t>((offset % IsoFs::kSectorSize + bytes + IsoFs::kSectorSize - 1) /
                                               IsoFs::kSectorSize));
    return true;
}

void LoaderFileIo::WindowedRead(CpuContext& ctx) {
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Loader);
    const uint32_t fs = A(ctx, 0);
    const int32_t offset = ctx.cpuRegs.GPR.r[5].SL[0];
    uint32_t size = A(ctx, 2);
    const uint32_t dest = A(ctx, 3);
    const uint32_t sync = A(ctx, 4);
    const uint32_t status = A(ctx, 5);
    const uint32_t file_size = memory::read<uint32_t>(fs + kFsFileSize);
    const int32_t handle = memory::read<int32_t>(fs + kFsHandle);
    uint32_t lba = memory::read<uint32_t>(fs + kFsSeek);   // the title's own extent when the handle is unknown
    auto it = g_files.find(handle);
    const bool known = it != g_files.end() && it->second.found;
    if (known) lba = it->second.lba;
    if (g_deps.trace) {
        char line[160];
        std::snprintf(line, sizeof line, "req fs=0x%x handle=%d off=0x%x size=0x%x dest=0x%x sync=%u pos=0x%llx fsz=0x%x", fs,
                      handle, static_cast<uint32_t>(offset), size, dest, sync,
                      static_cast<unsigned long long>(memory::read<uint32_t>(fs + kFsSeek)) + static_cast<uint32_t>(offset), file_size);
        g_deps.trace(line);
    }
    if (status != 0) memory::write<uint32_t>(status, size);
    if (size == 0 || !g_deps.engine || dest == 0) {
        Ret(ctx, 0);
        return;
    }
    const uint64_t off64 = static_cast<uint32_t>(offset);
    if (file_size == 0) {
        if (size > kBogusSizeLimit) { Ret(ctx, 0); return; }
    } else if (off64 >= file_size) {
        Ret(ctx, 0);
        return;
    } else if (off64 + size > file_size) {
        size = static_cast<uint32_t>(file_size - off64);
    }
    const uint64_t byte_offset = static_cast<uint64_t>(memory::read<uint32_t>(fs + kFsSeek)) + off64;   // the object's seek + the window
    if (StreamLibrary::FileOwned(handle)) {
        if (!known || byte_offset > it->second.size || size > it->second.size - byte_offset ||
            !InRam(dest) || size > 0x02000000u - dest) {
            StreamLibrary::ReadError();
            if (status) memory::write<uint32_t>(status, 0);
            Ret(ctx, 0);
            return;
        }
        const auto window = g_windows.find(fs);
        const bool same = window != g_windows.end() && window->second.dest == dest &&
                          window->second.offset == static_cast<uint32_t>(offset) && window->second.size == size;
        if ((sync != 0 || !same) && ReadInFlight(fs)) {
            const auto saved = ctx.cpuRegs;
            WaitReadComplete(ctx);
            ctx.cpuRegs = saved;
        }
    }

    if (g_deps.ring_cache) {
        Request r;
        r.fs = fs; r.offset = static_cast<uint32_t>(offset); r.size = size; r.dest = dest; r.sync = sync; r.status = status;
        r.lba = lba; r.byte_offset = byte_offset;
        RingCacheRead(ctx, r);
        return;
    }
    ps2r::ee::TickProfile::NoteLoad(size);   // the title's file window: a load tick

    if (sync == 0) {
        auto w = g_windows.find(fs);
        if (w != g_windows.end() && w->second.dest == dest && w->second.offset == static_cast<uint32_t>(offset) &&
            w->second.size == size) {
            // The re-ask is the loader's poll of this window: HeldInflight while
            // the read is still outstanding (noted on the model), Progress once
            // it was delivered (the released read is consumed).
            if (PollReadPending(fs)) {
                ++g_census.async_polls_pending;
                Ret(ctx, 0);   // the same window is still outstanding
                return;
            }
            g_windows.erase(w);
            ++g_census.async_polls_done;
            if (status != 0) memory::write<uint32_t>(status, size);
            Ret(ctx, 1);   // the window is available
            return;
        }
        // Retail FUN_002aac80 (0x2aafa4-0x2ab0e8, the "ring window not valid"
        // branch this seam stands in for) picks the DMA target by the window size
        // against the callback stream's capacity (cb vtbl+0x3c = cb+0x10):
        //   size <  cap : the window is DMAed into the ring at base (the cursor
        //                 rewound, cb vtbl+0x54), fs+0x10 = dest and fs+0x18 = size
        //                 are staged, and the completion (FUN_002ad110 / FUN_002ad1a0)
        //                 copies size bytes ring -> dest (cb vtbl+0x14 = FUN_002b5dd8);
        //   size >= cap, or no stream : the window is DMAed straight to dest
        //                 (FUN_002ad9a8(fs, pos, size, dest)); fs+0x10/fs+0x18 are not
        //                 written and the ring is untouched, so the completion runs no
        //                 copy (fs+0x10 == 0). A ring copy of an oversized window would
        //                 read past the ring's end into dest.
        const uint32_t cb = memory::read<uint32_t>(fs + kFsCbObj);
        const bool through_ring = InRam(cb) && size < memory::read<uint32_t>(cb + 0x10);
        auto payload = std::make_shared<std::vector<uint8_t>>(size);
        if (!ReadBytes(lba, byte_offset, size, payload->data()) && StreamLibrary::FileOwned(handle)) {
            StreamLibrary::ReadError();
            if (status) memory::write<uint32_t>(status, 0);
            Ret(ctx, 0);
            return;
        }
        memory::write<uint32_t>(fs + kFsFlags, memory::read<uint32_t>(fs + kFsFlags) | kFlagReadPending);
        if (through_ring) {
            memory::write<uint32_t>(fs + kFsDest, dest);
            memory::write<uint32_t>(fs + kFsSize, size);
        } else {
            ++g_census.async_direct;
        }
        ++g_census.async_issued;
        g_census.bytes_read += size;
        if (StreamLibrary::FileOwned(handle)) {
            FileSchedule::Payload request;
            request.object = fs; request.handle = handle; request.file_offset = byte_offset;
            request.destination = dest; request.bytes = std::move(*payload);
            if (through_ring) {
                request.ring_base = memory::read<uint32_t>(cb + kCbBase);
                request.ring_cursor = cb + kCbCursor;
            }
            const auto skew = static_cast<uint32_t>(byte_offset % IsoFs::kSectorSize);
            const auto sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
            const auto tick = g_deps.now_tick ? g_deps.now_tick() : 0;
            g_windows[fs] = Window{dest, size, static_cast<uint32_t>(offset)};
            if (!StreamLibrary::SubmitFile(std::move(request),
                    lba + static_cast<uint32_t>(byte_offset / IsoFs::kSectorSize), sectors, tick)) {
                StreamLibrary::ReadError();
                g_windows.erase(fs);
                memory::write<uint32_t>(fs + kFsDest, 0);
                memory::write<uint32_t>(fs + kFsFlags, memory::read<uint32_t>(fs + kFsFlags) & ~kFlagReadPending);
                if (status) memory::write<uint32_t>(status, 0);
            }
            Ret(ctx, 0);
            return;
        }
        // The STREAM client marks the file's slot "issued" when it stages the read
        // record (FUN_001e1500); the title's poll reads pending from it until the
        // delivery below writes "done". Under poll visibility the slot is marked
        // served instead: the client's reply processor ages an issued slot the
        // module never lists to done within a few replies (header comment), and a
        // host-served record is never listed.
        const uint32_t slot_status = StreamLibrary::FileOwned(handle) ? 0 : SlotStatusAddr(fs);
        if (slot_status) {
            const bool served = g_deps.model && g_deps.model->PollVisibility();
            memory::write<uint8_t>(slot_status, served ? kSlotServed : kSlotIssued);
            ++g_census.slot_issued;
        }
        const uint32_t fill = size;
        auto apply = StreamLibrary::Track(fs, handle, [fs, dest, payload, fill, slot_status, through_ring] {
            CopyIn(dest, payload->data(), static_cast<uint32_t>(payload->size()), fill);
            // The callback stream buffer (fs+0x14 -> {+8 base, +0xc cursor, +0x10 cap})
            // receives the same bytes when the window went through the ring (size
            // below its capacity) and it is not the destination itself: the IOP DMAs
            // the window to the ring's base, and the title's completion copy
            // (FUN_002b5dd8) moves it from the cursor, reset to base. A direct
            // (oversized / stream-less) window leaves the ring and its cursor alone.
            const uint32_t cb = through_ring ? memory::read<uint32_t>(fs + kFsCbObj) : 0u;
            if (InRam(cb)) {
                const uint32_t base = memory::read<uint32_t>(cb + 0x8);
                if (InRam(base)) {
                    const uint32_t n = static_cast<uint32_t>(payload->size());
                    if (base != dest) CopyIn(base, payload->data(), n, n);
                    memory::write<uint32_t>(cb + 0xc, base);
                }
            }
            // Bytes first, status last: the module's reply stops listing the slot
            // only after its DMA landed, and the client maps that to "done" (0).
            if (slot_status) {
                memory::write<uint8_t>(slot_status, kSlotDone);
                ++g_census.slot_done;
            }
        });
        g_windows[fs] = Window{dest, size, static_cast<uint32_t>(offset)};
        if (g_deps.model) {
            const uint32_t skew = static_cast<uint32_t>(byte_offset % IsoFs::kSectorSize);
            const uint32_t sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
            const uint64_t tick = g_deps.now_tick ? g_deps.now_tick() : 0;
            g_deps.model->Enqueue(fs, lba + static_cast<uint32_t>(byte_offset / IsoFs::kSectorSize), sectors, tick, apply);
        } else {
            apply();
        }
        Ret(ctx, 0);
        return;
    }
    ++g_census.sync_reads;
    g_census.bytes_read += size;
    std::vector<uint8_t> buf(size);
    if (!ReadBytes(lba, byte_offset, size, buf.data()) && StreamLibrary::FileOwned(handle)) {
        StreamLibrary::ReadError();
        if (status) memory::write<uint32_t>(status, 0);
        Ret(ctx, 0);
        return;
    }
    CopyIn(dest, buf.data(), size, size);
    Ret(ctx, 1);
}

void LoaderFileIo::WaitReadComplete(CpuContext& ctx) {
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Loader);
    const uint32_t fs = A(ctx, 0);
    ++g_census.waits;
    uint32_t flags = memory::read<uint32_t>(fs + kFsFlags);
    if (!(flags & kFlagReadPending)) return;
    ps2r::ee::TickProfile::NoteLoad(0);   // a completion waited for: a load tick
    if (StreamLibrary::TryWait(ctx)) {
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            if (RunStagedCompletionCopy(ctx, fs)) ++g_census.wait_completion_copies;
            memory::write<uint32_t>(fs + kFsFlags, memory::read<uint32_t>(fs + kFsFlags) & ~kFlagReadPending);
        }
        return;
    }
    ShipStagedRecords(ctx);
    if (g_deps.model && g_deps.model->InFlight(fs)) {
        g_deps.model->CompleteNow(fs);
        ++g_census.waits_forced;
    }
    auto poll = [&ctx, fs]() -> uint32_t {
        if (!g_deps.call) return 0;
        ctx.cpuRegs.GPR.r[4].UD[0] = fs;
        g_deps.call(g_layout.fn_poll, ctx);
        return ctx.cpuRegs.GPR.r[2].UL[0];
    };
    uint32_t pending = poll();
    // A record the module owns (no host read in flight, the slot byte in the
    // module's states): the retail spin would see the SIF interrupt's end-callback
    // land mid-loop; here each lap lets the cooperative IOP progress and delivers
    // the end-callbacks, then the title's own poll decides again. Bounded; the flag
    // is never lowered by anything but that poll. Each lap runs the client's
    // Ship(0) unconditionally, as the retail lap does: the slot byte ages only in
    // the client's reply processor, and the module answers (and advances its job
    // engine) only on a call -- a lap without a call can never see the record
    // retire, staged records or not. Ship(0) itself skips when the client's
    // previous call is still outstanding (sceSifCheckStatRpc).
    if (pending != 0 && g_deps.pump_module && (!g_deps.model || !g_deps.model->InFlight(fs)) && SlotModuleOwned(fs)) {
        ++g_census.waits_module;
        for (uint32_t lap = 0; lap < g_layout.max_module_laps && pending != 0; ++lap) {
            ++g_census.module_laps;
            g_deps.pump_module(ctx);
            if (g_deps.call) {
                ctx.cpuRegs.GPR.r[4].UD[0] = 0;
                g_deps.call(g_layout.fn_ship, ctx);
            }
            pending = poll();
        }
    }
    if (pending == 0) {
        // The retail wait's completion step (FUN_002ad1a0 0x2ad1f0-0x2ad21c): with a
        // ring window staged on the object (fs+0x10 != 0) the callback stream's
        // completion copy (cb vtbl+0x14, `this` adjusted by the int16 at vtbl+0x10)
        // moves fs+0x18 bytes from the stream's cursor to the staged destination,
        // then the staging word is cleared. Without the clear the staging outlives
        // the window: the next read on the object that completes through the
        // title's poll path (FUN_002ad110 -- a direct window, which stages
        // nothing) re-runs the copy into memory the title has re-used since
        //Under poll visibility only: boundary mode waits every
        // oversized window, so its lanes never meet the stale staging, and they
        // stay as they are.
        // Under the ring rule the staged copy is the ONLY path by which a ring
        // window reaches its destination (the fill lands in the ring, as on the
        // console), so the step runs in either visibility there.
        if ((g_deps.ring_cache || (g_deps.model && g_deps.model->PollVisibility())) && g_deps.call) {
            if (RunStagedCompletionCopy(ctx, fs)) ++g_census.wait_completion_copies;
        }
        flags = memory::read<uint32_t>(fs + kFsFlags);
        memory::write<uint32_t>(fs + kFsFlags, flags & ~kFlagReadPending);
    }
}

// The retail wait's completion step (FUN_002ad1a0 0x2ad1f0-0x2ad21c): with a ring
// window staged on the object (fs+0x10 != 0) the callback stream's completion copy
// (cb vtbl+0x14, `this` adjusted by the int16 at vtbl+0x10) moves fs+0x18 bytes from
// the stream's cursor to the staged destination, then the staging word is cleared.
// Returns whether a copy ran.
bool LoaderFileIo::RunStagedCompletionCopy(CpuContext& ctx, uint32_t fs) {
    const uint32_t dest = memory::read<uint32_t>(fs + kFsDest);
    if (dest == 0) return false;
    const uint32_t cb = memory::read<uint32_t>(fs + kFsCbObj);
    const uint32_t size = memory::read<uint32_t>(fs + kFsSize);
    bool ran = false;
    if (InRam(cb) && InRam(memory::read<uint32_t>(cb))) {
        RingCopyOut(ctx, cb, dest, size);
        ran = true;
    }
    memory::write<uint32_t>(fs + kFsDest, 0);
    return ran;
}

// The title's copy-out (cb vtbl+0x14 = FUN_002b5dd8: memcpy(dest, cursor, n); cursor
// += n), run as the retail body runs it -- through the vtable with the int16 `this`
// adjustment at vtbl+0x10 -- so the cursor word is the title's own. Without a bound
// call site (a harness with no recompiled code) the same two steps run on the host.
void LoaderFileIo::RingCopyOut(CpuContext& ctx, uint32_t cb, uint32_t dest, uint32_t n) {
    const uint32_t vtbl = InRam(cb) ? memory::read<uint32_t>(cb) : 0u;
    if (g_deps.call && InRam(vtbl)) {
        const int32_t adjust = static_cast<int16_t>(memory::read<uint16_t>(vtbl + g_layout.cb_vtbl_this_adjust));
        const uint32_t fn = memory::read<uint32_t>(vtbl + g_layout.cb_vtbl_completion_copy);
        ctx.cpuRegs.GPR.r[4].UD[0] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(cb) + adjust));
        ctx.cpuRegs.GPR.r[5].UD[0] = dest;
        ctx.cpuRegs.GPR.r[6].UD[0] = n;
        ctx.cpuRegs.GPR.r[7].UD[0] = 1;
        g_deps.call(fn, ctx);
        return;
    }
    const uint32_t cursor = memory::read<uint32_t>(cb + kCbCursor);
    for (uint32_t i = 0; i < n; ++i) memory::write<uint8_t>(dest + i, memory::read<uint8_t>(cursor + i));
    memory::write<uint32_t>(cb + kCbCursor, cursor + n);
}

// One STREAM read record on the drive timeline (retail FUN_002ad9a8(fs, pos, size,
// dest)): `pos` is the byte position from the file's first sector, `dest` the ring's
// base (into_ring) or the caller's buffer. A synchronous request lands inside the
// call; an asynchronous one marks the slot issued, raises the pending flag's
// delivery through the model (poll / boundary / wait) and writes done last. The
// payload is the fill's bytes only: the ring's bytes past the fill keep the
// previous window, as the console's do; the cursor is the title's (rewound by the
// caller before the issue, advanced by its copy-out).
bool LoaderFileIo::IssueRead(const Request& r, uint64_t pos, uint32_t size, uint32_t dest, bool into_ring) {
    auto payload = std::make_shared<std::vector<uint8_t>>(size);
    const int32_t handle = memory::read<int32_t>(r.fs + kFsHandle);
    if (!ReadBytes(r.lba, pos, size, payload->data()) && StreamLibrary::FileOwned(handle)) {
        StreamLibrary::ReadError();
        memory::write<uint32_t>(r.fs + kFsDest, 0);
        memory::write<uint32_t>(r.fs + kFsFlags,
                                memory::read<uint32_t>(r.fs + kFsFlags) & ~(kFlagReadPending | kFlagRingValid));
        if (r.status) memory::write<uint32_t>(r.status, 0);
        return false;
    }
    g_census.bytes_read += size;
    if (into_ring) ++g_census.ring_fills;
    if (r.sync != 0) {
        ++g_census.sync_reads;
        CopyIn(dest, payload->data(), size, size);
        return true;
    }
    ++g_census.async_issued;
    if (!into_ring) ++g_census.async_direct;
    if (StreamLibrary::FileOwned(handle)) {
        FileSchedule::Payload request;
        request.object = r.fs; request.handle = handle; request.file_offset = pos;
        request.destination = dest; request.bytes = std::move(*payload);
        const auto skew = static_cast<uint32_t>(pos % IsoFs::kSectorSize);
        const auto sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
        const auto tick = g_deps.now_tick ? g_deps.now_tick() : 0;
        g_windows[r.fs] = Window{r.dest, r.size, r.offset};
        if (StreamLibrary::SubmitFile(std::move(request),
                r.lba + static_cast<uint32_t>(pos / IsoFs::kSectorSize), sectors, tick)) return true;
        StreamLibrary::ReadError();
        g_windows.erase(r.fs);
        memory::write<uint32_t>(r.fs + kFsDest, 0);
        memory::write<uint32_t>(r.fs + kFsFlags,
                                memory::read<uint32_t>(r.fs + kFsFlags) & ~(kFlagReadPending | kFlagRingValid));
        if (r.status) memory::write<uint32_t>(r.status, 0);
        return false;
    }
    const uint32_t slot_status = StreamLibrary::FileOwned(handle) ? 0 : SlotStatusAddr(r.fs);
    if (slot_status) {
        const bool served = g_deps.model && g_deps.model->PollVisibility();
        memory::write<uint8_t>(slot_status, served ? kSlotServed : kSlotIssued);
        ++g_census.slot_issued;
    }
    auto apply = StreamLibrary::Track(r.fs, handle, [dest, payload, slot_status] {
        CopyIn(dest, payload->data(), static_cast<uint32_t>(payload->size()), static_cast<uint32_t>(payload->size()));
        if (slot_status) {
            memory::write<uint8_t>(slot_status, kSlotDone);
            ++g_census.slot_done;
        }
    });
    g_windows[r.fs] = Window{r.dest, r.size, r.offset};
    if (g_deps.model) {
        const uint32_t skew = static_cast<uint32_t>(pos % IsoFs::kSectorSize);
        const uint32_t sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
        const uint64_t tick = g_deps.now_tick ? g_deps.now_tick() : 0;
        g_deps.model->Enqueue(r.fs, r.lba + static_cast<uint32_t>(pos / IsoFs::kSectorSize), sectors, tick, apply);
    } else {
        apply();
    }
    return true;
}

// The ring rule (header comment): retail FUN_002aac80 0x2aacf0-0x2ab128 over the
// title's own words -- fs+0 (flags), fs+0xc (the fill's start), fs+0x10/+0x18 (the
// staged window), fs+0x2c/+0x30 (the file's extent), cb {base, cursor, cap}.
void LoaderFileIo::RingCacheRead(CpuContext& ctx, const Request& r) {
    ps2r::ee::TickProfile::NoteLoad(r.size);   // the title's file window (hit, miss or poll): a load tick
    const uint32_t fs = r.fs;
    const uint32_t file_base = memory::read<uint32_t>(fs + kFsSeek);
    const uint32_t file_size = memory::read<uint32_t>(fs + kFsFileSize);
    // s6 = fs+0x2c + fs+0x30; an object whose size word is still 0 (the size query
    // not yet answered -- the prologue admits such a read) has no end to clamp by.
    const uint32_t eof = file_size ? file_base + file_size : 0xffffffffu;
    const uint32_t pos = file_base + r.offset;                                     // s3
    const uint32_t cb = memory::read<uint32_t>(fs + kFsCbObj);
    const bool stream = InRam(cb);
    const uint32_t base = stream ? memory::read<uint32_t>(cb + kCbBase) : 0u;
    const uint32_t cap = stream ? memory::read<uint32_t>(cb + kCbCap) : 0u;

    if (r.sync == 0) {
        auto w = g_windows.find(fs);
        if (w != g_windows.end() && w->second.dest == r.dest && w->second.offset == r.offset && w->second.size == r.size) {
            // The re-ask of the window in flight (the loader's poll of it): 0 while
            // the read is outstanding; once delivered the staged copy lands the
            // window (the step the title's poll path runs) and the answer is 1.
            if (PollReadPending(fs)) {
                ++g_census.async_polls_pending;
                Ret(ctx, 0);
                return;
            }
            g_windows.erase(w);
            ++g_census.async_polls_done;
            if (g_deps.call) RunStagedCompletionCopy(ctx, fs);
            if (r.status != 0) memory::write<uint32_t>(r.status, r.size);
            Ret(ctx, 1);
            return;
        }
    }

    // A new request on the object: the window it had in flight is over (the title
    // waited for it); a hit issues nothing, so the entry must not outlive it to
    // match a later request that only shares (dest, size, offset) -- the next file
    // on the same object starts with the same words.
    g_windows.erase(fs);
    uint32_t flags = memory::read<uint32_t>(fs + kFsFlags);
    uint32_t remaining = 0;   // s2: bytes from the cursor to the ring's end
    if (stream) {
        // Prologue (0x2aacf0-0x2aadc4): the cursor is placed at the request inside
        // the ring's window; a position outside [fs+0xc, fs+0xc + cap) rewinds it
        // and drops the valid bit.
        const uint32_t fill_start = memory::read<uint32_t>(fs + kFsRingStart);
        uint32_t cursor = memory::read<uint32_t>(cb + kCbCursor);
        const uint32_t tell = cursor - base;
        const uint32_t at = fill_start + tell;
        bool rewind = false;
        if (at < pos) {
            const uint32_t ahead = pos - at;
            if (static_cast<int32_t>(ahead) < static_cast<int32_t>(cap - tell)) cursor += ahead;
            else rewind = true;
        } else if (at > pos) {
            if (pos < fill_start) rewind = true;
            else cursor -= at - pos;
        }
        if (rewind) {
            cursor = base;
            flags &= ~kFlagRingValid;
            memory::write<uint32_t>(fs + kFsFlags, flags);
        }
        memory::write<uint32_t>(cb + kCbCursor, cursor);
        remaining = cap - (cursor - base);
    }

    uint32_t result = r.sync != 0 ? 1u : 0u;
    if (flags & kFlagRingValid) {
        if (remaining >= r.size) {
            // Hit (0x2aade8): served from the ring by the title's copy-out; no record.
            ++g_census.ring_hits;
            RingCopyOut(ctx, cb, r.dest, r.size);
            result = 1;
        } else {
            // Straddle (0x2aae04-0x2aafa0).
            ++g_census.ring_straddles;
            const uint32_t tail = remaining;
            const uint32_t new_start = pos + tail;        // s7
            const uint32_t remainder = r.size - tail;     // s6
            if (remainder < cap) {
                RingCopyOut(ctx, cb, r.dest, tail);
                uint32_t fill = eof - new_start;          // s0 = min(eof - start, cap)
                if (fill > cap) fill = cap;
                memory::write<uint32_t>(cb + kCbCursor, base);
                memory::write<uint32_t>(fs + kFsRingStart, new_start);
                flags |= kFlagReadPending;
                memory::write<uint32_t>(fs + kFsFlags, flags);
                if (!IssueRead(r, new_start, fill, base, true)) { Ret(ctx, 0); return; }
                if (r.sync != 0) {
                    RingCopyOut(ctx, cb, r.dest + tail, remainder);
                    flags &= ~kFlagReadPending;
                    memory::write<uint32_t>(fs + kFsFlags, flags);
                } else {
                    memory::write<uint32_t>(fs + kFsSize, remainder);
                    memory::write<uint32_t>(fs + kFsDest, r.dest + tail);
                }
            } else {
                const uint32_t aligned = tail & ~0x3fu;
                RingCopyOut(ctx, cb, r.dest, aligned);
                flags |= kFlagReadPending;
                memory::write<uint32_t>(fs + kFsFlags, flags);
                if (!IssueRead(r, static_cast<uint64_t>(pos) + aligned, r.size - aligned, r.dest + aligned, false)) {
                    Ret(ctx, 0);
                    return;
                }
                flags = memory::read<uint32_t>(fs + kFsFlags);
                if (r.sync != 0) flags &= ~kFlagReadPending;
                flags &= ~kFlagRingValid;
                memory::write<uint32_t>(fs + kFsFlags, flags);
            }
        }
    } else if (stream && r.size < cap) {
        // Miss (0x2aafd0-0x2ab0a8): one fill from the request position.
        ++g_census.ring_misses;
        memory::write<uint32_t>(cb + kCbCursor, base);
        memory::write<uint32_t>(fs + kFsRingStart, pos);
        uint32_t fill = eof - pos;
        if (fill > cap) fill = cap;
        flags |= kFlagReadPending;
        memory::write<uint32_t>(fs + kFsFlags, flags);
        if (!IssueRead(r, pos, fill, base, true)) { Ret(ctx, 0); return; }
        if (r.sync != 0) {
            RingCopyOut(ctx, cb, r.dest, r.size);
            flags &= ~kFlagReadPending;
        } else {
            memory::write<uint32_t>(fs + kFsSize, r.size);
            memory::write<uint32_t>(fs + kFsDest, r.dest);
        }
        flags |= kFlagRingValid;
        memory::write<uint32_t>(fs + kFsFlags, flags);
    } else {
        // Direct (0x2ab0b0): at or above the capacity, or no stream.
        flags |= kFlagReadPending;
        memory::write<uint32_t>(fs + kFsFlags, flags);
        if (!IssueRead(r, pos, r.size, r.dest, false)) { Ret(ctx, 0); return; }
        if (r.sync != 0) {
            flags &= ~kFlagReadPending;
            memory::write<uint32_t>(fs + kFsFlags, flags);
        }
    }
    // Epilogue (0x2ab0ec-0x2ab124): the cursor at the ring's end drops the valid bit.
    if (stream && memory::read<uint32_t>(cb + kCbCursor) - base == cap) {
        flags = memory::read<uint32_t>(fs + kFsFlags);
        memory::write<uint32_t>(fs + kFsFlags, flags & ~kFlagRingValid);
    }
    Ret(ctx, result);
}
