#include "games/slus_20909_crash_twinsanity/hle/saves/save_legacy_import.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "engine/ee/hle/kernel_codes.h"
#include "engine/ee/savestate/thread_section.h"

namespace title {
namespace {
using namespace ps2r::savestate;
constexpr uint32_t kRamBytes = 32 * 1024 * 1024, kPhysical = 0x1fffffff;
constexpr uint32_t kInitialised = 0x309fb4, kOperation = 0x309fb8, kLastOperation = 0x309fbc;
constexpr uint32_t kNext = 0x309fc0, kStep = 0x309fc4, kFile = 0x309fcc;
constexpr uint32_t kResults = 0x3c6100, kContext = 0x3c61a0, kTemplate = 0x3064e0;
constexpr uint32_t kRegion = 0x30a6f8, kProduct = 0x3c6190, kInfo = 0x2e7d68;
constexpr uint32_t kClient = 0x3c8580, kInitSema = 0x2e9ccc, kCommandSema = 0x2e9cd0;
bool Fail(std::string* error, const char* message) {
    if (error) *error = std::string("Save legacy import: ") + message;
    return false;
}
uint32_t U32(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
        (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}
int32_t I32(const std::vector<uint8_t>& bytes, size_t at) {
    const uint32_t value = U32(bytes, at);
    return value <= INT32_MAX ? static_cast<int32_t>(value) : -1 - static_cast<int32_t>(UINT32_MAX - value);
}
const Blob* Find(const std::vector<Blob>& blobs, uint32_t tag) {
    for (const auto& blob : blobs) if (blob.tag == tag) return &blob;
    return nullptr;
}
bool Version(const Blob& blob) { return blob.version == 1 && (blob.flags & ~kSectionRequired) == 0; }
bool String(const std::vector<uint8_t>& ram, size_t at, size_t size, std::string& out) {
    const auto first = ram.begin() + at, end = first + size;
    const auto zero = std::find(first, end, 0);
    if (zero == end) return false;
    out.assign(first, zero);
    return true;
}
bool SaveCode(uint32_t pc) {
    pc &= kPhysical;
    // NTSC complete manager/step bodies, and mcInit through sceMcDelete.
    // End addresses are the next functions in shards 028 / 030.
    return (pc >= 0x2a2970 && pc < 0x2a8fc0) || (pc >= 0x2c5080 && pc < 0x2c5ff0);
}
bool Address(uint32_t address) {
    return (address & 3) == 0 && (address & kPhysical) >= 0x100000 &&
        (address & kPhysical) < kRamBytes;
}
bool Manager(const std::vector<uint8_t>& ram, SaveStorage::Snapshot& state, std::string* error) {
    if (U32(ram, kInitialised) != 1) return Fail(error, "manager initialisation is not complete");
    if (U32(ram, kOperation) != 0 || I32(ram, kNext) != -1 || U32(ram, kStep) != 0)
        return Fail(error, "manager has an active operation, next operation or step");
    if (U32(ram, kLastOperation) > 11) return Fail(error, "invalid last operation");
    if (I32(ram, kFile) != -1) return Fail(error, "residual file descriptor cannot be migrated");
    // Source Finish uses {-1,-1,zero...}, not an absent-state reset. Check
    // against its meaning as well as RAM's template, which can be corrupted.
    for (uint32_t i = 0; i < 0x9c; ++i) {
        const uint8_t expected = i < 8 ? 255 : 0;
        if (ram[kContext + i] != expected || ram[kTemplate + i] != expected)
            return Fail(error, "request context or template retains data");
    }
    state.initialised = true;
    if (!String(ram, kRegion, 8, state.region) || !String(ram, kProduct, 16, state.product))
        return Fail(error, "unterminated region or product");
    for (size_t i = 0; i < state.results.size(); ++i) {
        const uint32_t at = kResults + static_cast<uint32_t>(i * 12);
        state.results[i] = {I32(ram, at), I32(ram, at + 4), I32(ram, at + 8)};
    }
    return true;
}
bool CommandAndObservation(const std::vector<uint8_t>& ram, const Blob& mchl,
                           SaveStorage::Snapshot& state, std::string* error) {
    if (mchl.payload.size() != 12) return Fail(error, "MCHL must contain exactly 12 bytes");
    if (U32(mchl.payload, 0) != 0 || U32(mchl.payload, 4) != 0)
        return Fail(error, "MCHL has a pending command or undelivered result");
    unsigned seen = 0, observed_slot = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto value = mchl.payload[8 + slot];
        if (value > 1) return Fail(error, "MCHL seen flags are not canonical");
        if (value) { ++seen; observed_slot = slot; }
    }
    if (seen > 1) return Fail(error, "shared storage observation cannot be assigned to multiple seen slots");
    const SaveStorage::Info info{I32(ram, kInfo), I32(ram, kInfo + 4), I32(ram, kInfo + 8)};
    if (seen == 0) {
        if (info.type != -1 || info.free_kilobytes != -1 || info.formatted != -1)
            return Fail(error, "storage observation exists without a seen slot");
        for (const auto& result : state.results)
            if (result.succeeded != -1 || result.step != -1 || result.error != 0)
                return Fail(error, "operation result exists without any card observation");
    } else {
        if (info.type == -1) return Fail(error, "seen slot has no storage observation");
        state.info[observed_slot] = info;
    }
    // Other slots remain explicitly unobserved (-1,-1,-1). The next owner
    // Update inspects its actual host slot before publishing StorageInfo.
    return true;
}
bool ClientAndRpc(const std::vector<uint8_t>& ram, const Blob& sifs, std::string* error) {
    const uint32_t server = U32(ram, kClient + 0x24);
    if (!server) return Fail(error, "initialised manager has no bound MC client");
    if (U32(ram, kClient + 4) != U32(ram, kClient + 0x18) ||
        (U32(ram, kClient + 0x10) & 1) != 0)
        return Fail(error, "MC client still has queued work");
    // Neither legacy producer serializes the deferred end-callback queue.
    // A nonzero end function is ambiguous even when SIFS says no RPC active.
    if (U32(ram, kClient + 0x1c) != 0 || U32(ram, kClient + 0x20) != 0)
        return Fail(error, "MC client retains an ambiguous end callback");
    if (sifs.payload.size() < 40) return Fail(error, "SIFS header is truncated");
    const uint32_t count = U32(sifs.payload, 36);
    if (count > 4096 || sifs.payload.size() != 40 + size_t{count} * 9)
        return Fail(error, "SIFS count or length is invalid");
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 40 + size_t{i} * 9;
        const uint32_t client = U32(sifs.payload, at) & kPhysical;
        const uint8_t active = sifs.payload[at + 8];
        if (active > 1 || client == 0 || client > kRamBytes - 40 || (client & 3))
            return Fail(error, "SIFS has an invalid pending client record");
        if (SaveCode(U32(ram, client + 0x1c)) ||
            (active && (client == kClient || U32(ram, client + 0x24) == server)))
            return Fail(error, "SIFS retains a Save callback or active MC RPC");
    }
    return true; // Other services' RPCs still require their own root preflight.
}
bool OwnerThreads(const std::vector<uint8_t>& ram, const Blob& thrd, std::string* error) {
    std::vector<ThreadSection::Record> records;
    if (!ThreadSection::Parse(thrd.payload, records, error)) return false;
    if (records.empty()) return Fail(error, "THRD has no incoming owner state");
    std::unordered_set<int32_t> tids;
    unsigned owners = 0;
    size_t at = 4;
    for (const auto& thread : records) {
        if (thrd.payload[at + 60] > 1 || thrd.payload[at + 61] > 1 ||
            thrd.payload[at + 62] > 1 || thrd.payload[at + 63] != 0)
            return Fail(error, "THRD flags are not canonical");
        at += ThreadSection::kRecordHeaderBytes + (thread.has_ctx ? sizeof(CpuContext) : 0) +
            thread.chain.size() * 8;
        if (thread.tid < 1 || thread.tid > 255 || !tids.insert(thread.tid).second)
            return Fail(error, "THRD has invalid or duplicate thread ids");
        const bool dormant = thread.status == THS_DORMANT;
        if (!dormant && thread.status != THS_RUN && thread.status != THS_READY &&
            thread.status != THS_WAIT && thread.status != THS_SUSPEND && thread.status != THS_WAITSUSPEND)
            return Fail(error, "THRD has an unknown thread status");
        if (thread.wait_type < TSW_NONE || thread.wait_type > TSW_EVENT ||
            ((thread.wait_type == TSW_SEMA || thread.wait_type == TSW_EVENT) && thread.wait_id <= 0))
            return Fail(error, "THRD has an unknown wait kind or object");
        if (!Address(thread.entry) || SaveCode(thread.entry))
            return Fail(error, "THRD entry is unknown or belongs to retired Save code");
        if ((!dormant || thread.started) && (!thread.started || !thread.has_ctx || thread.chain.empty()))
            return Fail(error, "THRD live thread lacks a complete context and call chain");
        if (!thread.has_ctx && !thread.chain.empty())
            return Fail(error, "THRD call chain has no context");
        if (thread.is_boundary) {
            if (dormant || !thread.started || !thread.has_ctx) return Fail(error, "incoming game owner is not live");
            ++owners;
        }
        if (thread.has_ctx) {
            const auto pc = thread.ctx.cpuRegs.pc, ra = thread.ctx.cpuRegs.GPR.r[31].UL[0];
            if (!Address(pc) || (ra && !Address(ra)) || SaveCode(pc) || SaveCode(ra))
                return Fail(error, "THRD context has an unknown or retired Save continuation");
        }
        for (const auto& frame : thread.chain)
            if (!Address(frame.func_addr) || (frame.ra && !Address(frame.ra)) ||
                SaveCode(frame.func_addr) || SaveCode(frame.ra))
                return Fail(error, "THRD chain retains an unknown or retired Save continuation");
        if (thread.wait_type == TSW_SEMA && thread.wait_id > 0 &&
            (thread.wait_id == I32(ram, kInitSema) || thread.wait_id == I32(ram, kCommandSema) ||
             thread.wait_id == I32(ram, kClient + 8)))
            return Fail(error, "THRD retains a memory-card semaphore waiter");
    }
    return owners == 1 || Fail(error, "THRD must identify exactly one incoming game owner");
}
}

bool SaveLegacyImport::Decode(const std::vector<Blob>& original, SaveBindings::Snapshot& out, std::string* error) {
    std::unordered_set<uint32_t> tags;
    for (const auto& blob : original)
        if (!tags.insert(blob.tag).second) return Fail(error, "duplicate section tag");
    if (Find(original, Tag('I', 'S', 'C', 'P')) || Find(original, Tag('I', 'N', 'P', 'T')) ||
        Find(original, Tag('S', 'V', 'S', 'T')))
        return Fail(error, "service checkpoint cannot use legacy Save migration");
    const auto* ram = Find(original, TAG_RAM);
    const auto* mchl = Find(original, TAG_MCHL);
    const auto* sifs = Find(original, TAG_SIFS);
    const auto* thrd = Find(original, TAG_THRD);
    if (!ram || !mchl || !sifs || !thrd)
        return Fail(error, "original RAM0, MCHL, SIFS and THRD are all required");
    if (!Version(*ram) || !Version(*mchl) || !Version(*sifs) || !Version(*thrd))
        return Fail(error, "unsupported legacy section version or flags");
    if (ram->payload.size() != kRamBytes) return Fail(error, "RAM0 must contain exactly 32 MiB");
    SaveBindings::Snapshot candidate;
    if (!Manager(ram->payload, candidate.storage, error) ||
        !CommandAndObservation(ram->payload, *mchl, candidate.storage, error) ||
        !ClientAndRpc(ram->payload, *sifs, error) || !OwnerThreads(ram->payload, *thrd, error) ||
        !SaveBindings::ValidateSnapshot(candidate, error)) return false;
    // No in-flight work crosses owners: generation 1 / sequence 0 start a
    // fresh request namespace. Retained completed results are copied above,
    // not converted to completions or replayed. Restore rebases the boundary.
    out = std::move(candidate);
    if (error) error->clear();
    return true;
}

}  // namespace title
