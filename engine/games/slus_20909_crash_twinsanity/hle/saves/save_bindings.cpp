#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/mem/scratchpad.h"

namespace title {
namespace {
// Verified complete NTSC bodies in recompiled_shard_028.cpp. PAL names in
// retired.txt identify the source replacement; arguments and op numbers below
// come from NTSC loads/stores, not positional mapping.
constexpr uint32_t kEntries[] = {
    0x2a8668, // Initialise(region=a0, product=a1)
    0x2a6b68, // Update(port=a0, slot=a1, info=a2)
    0x2a8638, // GetResult(operation=a0, output=a1), explicit output pointer
    0x2a69f0, // Format(port=a0, slot=a1)
    0x2a8728, // Create(port, slot, save=a2)
    0x2a88c0, // Measure(port, slot, save=a2, output=a3)
    0x2a8a60, // Find(port, slot, file=a2, save=a3, size=t0, entry=t1)
    0x2a8c28, // Write(port, slot, save=a2, file=a3, data=t0, size=t1)
    0x2a8df0, // Read(port, slot, save=a2, file=a3, data=t0, size=t1)
    0x2c5080, // Memory-card service startup, called by Main before save-manager Initialise
};
uint32_t Arg(const CpuContext& context, unsigned index) { return context.cpuRegs.GPR.r[4 + index].UL[0]; }
void Return(CpuContext& context, int32_t value) { context.cpuRegs.GPR.r[2].SD[0] = value; }
}
SaveBindings::SaveBindings(SaveStorage& storage, Hooks hooks) : storage_(storage), hooks_(std::move(hooks)) {}
// Game memory the entries read or write: main RAM in any segment, and the
// scratchpad. The title's main thread runs on the scratchpad (TR
// src/platform/ps2/entry.cpp:8-9,83): Poll 0x2a2430 hands GetResult its
// stack slot as the output, so the result is a scratchpad write (the gate-5
// cold-boot abort "game output no longer writable" at the title card probe).
bool SaveBindings::RamRange(uint32_t address, size_t size) {
    if (address == 0) return false;
    if (Scratchpad::Contains(address)) return size <= Scratchpad::kSize - Scratchpad::Offset(address);
    const uint32_t physical = address & 0x1fffffffu;
    return physical < 0x2000000u && size <= 0x2000000u - physical;
}
bool SaveBindings::String(uint32_t address, size_t max, std::string& out) const {
    out.clear();
    for (size_t i = 0; i <= max; ++i) {
        uint8_t value = 0;
        if (address > UINT32_MAX - i || !RamRange(address + static_cast<uint32_t>(i), 1) ||
            !hooks_.read(address + static_cast<uint32_t>(i), &value, 1)) return false;
        if (!value) return true;
        out += static_cast<char>(value);
    }
    return false;
}
void SaveBindings::Write(uint32_t address, const void* bytes, size_t size) const {
    if (!size) return;
    if (!RamRange(address, size) || !hooks_.write(address, bytes, size))
        throw std::runtime_error("SaveStorage: game output no longer writable");
}
bool SaveBindings::Install(const Installer& install) {
    if (!install || !hooks_.read || !hooks_.write || !hooks_.boundary) return false;
    for (const auto function : kEntries) {
        if (!install(function, [this, function](CpuContext& context, uint32_t entry) {
            if (entry != 0 && entry != function)
                throw std::runtime_error("SaveStorage: obsolete interior memory-card continuation");
            Invoke(function, context);
        })) return false;
    }
    return true;
}
void SaveBindings::Publish() {
    auto completion = storage_.TakeCompletion();
    if (!completion) return;
    // Only the game execution owner calls this. All payload writes precede
    // return from Update, so gameplay cannot observe completion first.
    if (completion->result.succeeded == 1) {
        if (outputs_.data && !completion->bytes.empty())
            Write(outputs_.data, completion->bytes.data(), completion->bytes.size());
        if (outputs_.value) Write(outputs_.value, &completion->value, sizeof(completion->value));
        if (outputs_.entry && completion->entry_present)
            Write(outputs_.entry, &completion->entry, sizeof(completion->entry));
    }
    outputs_ = {};
    ++counts_.published;
}
void SaveBindings::Invoke(uint32_t function, CpuContext& context) {
    ++counts_.calls;
    if (function == 0x2c5080) {
        ++counts_.starts;
        const bool ready = storage_.Start();
        if (!ready) ++counts_.start_failures;
        Return(context, ready ? 0 : -1);
        return;
    }
    if (function == 0x2a8668) {
        std::string region, product;
        const bool valid = String(Arg(context, 0), 7, region) && String(Arg(context, 1), 15, product);
        Return(context, valid && storage_.Initialise(region, product));
        return;
    }
    if (function == 0x2a6b68) {
        const uint32_t info_address = Arg(context, 2);
        if (info_address && !RamRange(info_address, sizeof(SaveStorage::Info)))
            throw std::runtime_error("SaveStorage: invalid storage info output");
        const auto operation = storage_.Update(Arg(context, 0), Arg(context, 1), hooks_.boundary());
        Publish();
        if (info_address) {
            const auto info = storage_.StorageInfo(Arg(context, 0), Arg(context, 1));
            Write(info_address, &info, sizeof(info)); // exact 12 bytes; no stack-derived pointer
        }
        Return(context, static_cast<int32_t>(operation));
        return;
    }
    if (function == 0x2a8638) {
        const auto result = storage_.GetResult(static_cast<SaveStorage::Operation>(Arg(context, 0)));
        Write(Arg(context, 1), &result, sizeof(result));
        // NTSC copies words 0/1 through v0 and word 2 through v1.
        context.cpuRegs.GPR.r[2].UL[0] = static_cast<uint32_t>(result.succeeded);
        context.cpuRegs.GPR.r[2].UL[1] = static_cast<uint32_t>(result.step);
        context.cpuRegs.GPR.r[3].SD[0] = result.error;
        return;
    }
    SaveStorage::Request request;
    request.port = Arg(context, 0);
    request.slot = Arg(context, 1);
    Outputs outputs;
    bool valid = true;
    switch (function) {
        case 0x2a69f0: request.operation = SaveStorage::Operation::Format; break;
        case 0x2a8728:
            request.operation = SaveStorage::Operation::Create;
            valid = String(Arg(context, 2), 31, request.save);
            break;
        case 0x2a88c0:
            request.operation = SaveStorage::Operation::Measure;
            valid = String(Arg(context, 2), 31, request.save) && RamRange(Arg(context, 3), 4);
            outputs.value = Arg(context, 3);
            break;
        case 0x2a8a60:
            request.operation = SaveStorage::Operation::Find;
            valid = String(Arg(context, 2), 31, request.file) && String(Arg(context, 3), 31, request.save);
            outputs.value = Arg(context, 4);
            outputs.entry = Arg(context, 5);
            valid = valid && (!outputs.value || RamRange(outputs.value, 4)) &&
                    (!outputs.entry || RamRange(outputs.entry, sizeof(SaveStorage::FileEntry)));
            break;
        case 0x2a8c28:
        case 0x2a8df0:
            request.operation = function == 0x2a8c28 ? SaveStorage::Operation::Write : SaveStorage::Operation::Read;
            request.size = Arg(context, 5); // EE t1, never sp+0x14
            valid = String(Arg(context, 2), 31, request.save) && String(Arg(context, 3), 31, request.file) &&
                    request.size <= SaveStorage::kMaxBytes && (!request.size || RamRange(Arg(context, 4), request.size));
            if (valid && function == 0x2a8c28) {
                request.bytes.resize(request.size);
                if (request.size) valid = hooks_.read(Arg(context, 4), request.bytes.data(), request.size);
            } else if (valid) {
                outputs.data = Arg(context, 4);
                outputs.size = request.size;
            }
            break;
        default: throw std::invalid_argument("SaveStorage: unregistered entry");
    }
    if (valid && storage_.Submit(std::move(request), hooks_.boundary())) {
        outputs_ = outputs;
        ++counts_.submitted;
        Return(context, 1);
    } else {
        ++counts_.rejected;
        Return(context, 0); // prior request and output ownership remain intact
    }
}
std::optional<SaveBindings::Snapshot> SaveBindings::Capture() const {
    auto storage = storage_.Capture();
    if (!storage) return std::nullopt;
    return Snapshot{1, std::move(*storage), outputs_};
}
bool SaveBindings::ValidateSnapshot(const Snapshot& snapshot, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    const auto& out = snapshot.outputs;
    if (!SaveStorage::ValidateSnapshot(snapshot.storage, error)) return false;
    if (snapshot.version != 1 || (out.value && !RamRange(out.value, 4)) ||
        (out.entry && !RamRange(out.entry, sizeof(SaveStorage::FileEntry))) ||
        out.size > SaveStorage::kMaxBytes || (out.data && !RamRange(out.data, out.size)))
        return fail("SaveBindings: invalid output range or version");
    if (snapshot.storage.completion && !snapshot.storage.completion->bytes.empty() &&
        (!out.data || snapshot.storage.completion->bytes.size() > out.size))
        return fail("SaveBindings: completed read exceeds its output");
    if (snapshot.storage.active != SaveStorage::Operation::None &&
        snapshot.storage.pending.operation == SaveStorage::Operation::Read &&
        (snapshot.storage.pending.size != out.size || (out.size && !out.data)))
        return fail("SaveBindings: pending read disagrees with its output");
    const auto operation = snapshot.storage.active != SaveStorage::Operation::None ?
        snapshot.storage.pending.operation : (snapshot.storage.completion ?
        snapshot.storage.completion->operation : SaveStorage::Operation::None);
    if ((operation != SaveStorage::Operation::Read && (out.data || out.size)) ||
        (operation != SaveStorage::Operation::Find && out.entry) ||
        (operation != SaveStorage::Operation::Find && operation != SaveStorage::Operation::Measure && out.value) ||
        (operation == SaveStorage::Operation::Measure && !out.value))
        return fail("SaveBindings: output belongs to a different operation");
    if (error) error->clear();
    return true;
}
bool SaveBindings::Restore(const Snapshot& snapshot, uint64_t boundary) {
    if (!ValidateSnapshot(snapshot)) return false;
    if (!storage_.Restore(snapshot.storage, boundary)) return false;
    outputs_ = snapshot.outputs;
    return true;
}
void SaveBindings::Reset() {
    outputs_ = {};
    storage_.Reset();
}

}  // namespace title
