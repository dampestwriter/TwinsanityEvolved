#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "engine/cdvd/iso_fs.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {

Files::Files(ps2r::ipu::GuestMemory& memory, const IsoFs& disc) : memory_(memory), disc_(disc) {}

bool Files::DiscPath(const std::string& path) {
    if (path.empty() || path.size() >= kMaxPath || path.find('\0') != std::string::npos) return false;
    const auto colon = path.find(':');
    // Platform::Files builds cdrom0: paths. Relative paths are useful for
    // source-level callers; other devices must not resolve against this disc.
    return colon == std::string::npos || path.compare(0, colon + 1, "cdrom0:") == 0;
}

bool Files::Path(uint32_t address, std::string& path) const {
    if (!address) return false;
    path.clear();
    for (size_t i = 0; i < kMaxPath; ++i) {
        if (i > std::numeric_limits<uint32_t>::max() - address) return false;
        const auto* byte = memory_.Translate(address + static_cast<uint32_t>(i), 1);
        if (!byte) return false;
        if (!*byte) return true;
        path.push_back(static_cast<char>(*byte));
    }
    return false;
}

Files::Slot* Files::Find(int32_t handle) {
    if (handle < 0 || static_cast<size_t>(handle) >= slots_.size()) return nullptr;
    auto& slot = slots_[handle];
    return slot.generation ? &slot : nullptr;
}

int32_t Files::Error(int32_t result) { ++counts_.errors; return result; }

int32_t Files::Open(const std::string& path, int32_t flags) {
    ++counts_.opens;
    if (!DiscPath(path)) return Error(-19);
    if (flags != 1) return Error((flags & 2) || (flags & 0x700) ? -30 : -22);
    IsoFs::Entry entry;
    if (!disc_.Find(path, entry) || entry.directory) return Error(-2);
    const uint64_t end = uint64_t(entry.lba) * IsoFs::kSectorSize + entry.size;
    if (end > disc_.ImageBytes() || entry.size > uint32_t(std::numeric_limits<int32_t>::max()))
        return Error(-5);
    for (uint32_t handle = 0; handle < slots_.size(); ++handle) {
        auto& slot = slots_[handle];
        if (slot.generation) continue;
        if (next_generation_ == std::numeric_limits<uint64_t>::max()) return Error(-24);
        slot.file = {handle, entry.lba, entry.size, 0, path};
        slot.generation = next_generation_++;
        return static_cast<int32_t>(handle);
    }
    return Error(-24);
}

int32_t Files::Close(int32_t handle) {
    ++counts_.closes;
    auto* slot = Find(handle);
    if (!slot) return Error(-9);
    *slot = {};
    return 0;
}

int32_t Files::Read(int32_t handle, uint32_t destination, int32_t bytes) {
    ++counts_.reads;
    auto* slot = Find(handle);
    if (!slot) return Error(-9);
    if (bytes < 0) return Error(-22);
    auto& file = slot->file;
    const uint32_t want = std::min(static_cast<uint32_t>(bytes), file.size - file.position);
    if (!want) return 0;
    auto* output = destination ? memory_.Translate(destination, want) : nullptr;
    if (!output) return Error(-14);
    // A provider error cannot publish a partial record or advance its cursor.
    // Normal EOF is clipped above, so a short provider read is a media error.
    std::vector<uint8_t> data(want);
    const uint64_t offset = uint64_t(file.location) * IsoFs::kSectorSize + file.position;
    if (disc_.ReadBytes(offset, want, data.data()) != want) return Error(-5);
    std::memcpy(output, data.data(), want);
    file.position += want;
    counts_.bytes += want;
    return static_cast<int32_t>(want);
}

int32_t Files::Seek(int32_t handle, int32_t offset, uint32_t whence) {
    ++counts_.seeks;
    auto* slot = Find(handle);
    if (!slot) return Error(-9);
    const auto& file = slot->file;
    if (whence > 2) return Error(-22);
    const int64_t base = whence == 0 ? 0 : whence == 1 ? file.position : file.size;
    const int64_t position = base + offset;
    if (position < 0 || position > file.size) return Error(-22);
    slot->file.position = static_cast<uint32_t>(position);
    return static_cast<int32_t>(position);
}

int32_t Files::Write(int32_t handle) {
    ++counts_.writes_refused;
    return Error(Find(handle) ? -30 : -9);
}

void Files::Reset() {
    ++counts_.resets;
    for (auto& slot : slots_) slot = {};
}

uint64_t Files::Generation(int32_t handle) const {
    if (handle < 0 || static_cast<size_t>(handle) >= slots_.size()) return 0;
    return slots_[handle].generation;
}

bool Files::Install(const Capture& capture, const Installer& install, bool selected) {
    if (installed_ || !capture || !install) return false;
    constexpr std::array<uint32_t, 6> entries{kReset, kOpen, kClose, kSeek, kRead, kWrite};
    std::array<Body, entries.size()> originals;
    for (size_t i = 0; i < entries.size(); ++i) {
        originals[i] = capture(entries[i]);
        if (!originals[i]) return false;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto address = entries[i];
        if (!install(address, [this, i, address, selected, original = originals[i]]
                (CpuContext& context, uint32_t entry) {
            if (!selected || (entry && entry != address)) {
                ++counts_.original_calls[i];
                original(context, entry);
            } else Call(i, context);
        })) {
            for (size_t j = 0; j < i; ++j) install(entries[j], originals[j]);
            return false;
        }
    }
    installed_ = true; selected_ = selected;
    return true;
}

void Files::Call(size_t index, CpuContext& context) {
    const auto a = context.cpuRegs.GPR.r[4].SL[0];
    const auto b = context.cpuRegs.GPR.r[5].SL[0];
    const auto c = context.cpuRegs.GPR.r[6].SL[0];
    int32_t result = -22;
    switch (index) {
        case 0: Reset(); result = 0; break;
        case 1: {
            std::string path;
            result = Path(static_cast<uint32_t>(a), path) ? Open(path, b) : Error(-14);
            break;
        }
        case 2: result = Close(a); break;
        case 3: result = Seek(a, b, static_cast<uint32_t>(c)); break;
        case 4: result = Read(a, static_cast<uint32_t>(b), c); break;
        case 5: result = Write(a); break;
    }
    context.cpuRegs.GPR.r[2].SD[0] = result;
}

std::string Files::Census() const {
    uint64_t originals = 0, open = 0;
    for (auto count : counts_.original_calls) originals += count;
    for (const auto& slot : slots_) if (slot.generation) ++open;
    return "selected=" + std::to_string(selected_) + ",opens=" + std::to_string(counts_.opens) +
        ",closes=" + std::to_string(counts_.closes) + ",reads=" + std::to_string(counts_.reads) +
        ",bytes=" + std::to_string(counts_.bytes) + ",seeks=" + std::to_string(counts_.seeks) +
        ",resets=" + std::to_string(counts_.resets) + ",errors=" + std::to_string(counts_.errors) +
        ",writes_refused=" + std::to_string(counts_.writes_refused) +
        ",open=" + std::to_string(open) + ",original_calls=" + std::to_string(originals);
}

} // namespace title
