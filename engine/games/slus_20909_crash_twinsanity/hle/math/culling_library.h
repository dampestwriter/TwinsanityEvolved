#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_state.h"

namespace title {

// Complete source chunk-view boundaries. Selection/install and checkpoint
// composition must switch every producer and reader together.
class CullingLibrary {
public:
    enum class Operation {
        SetUpChunk, SetUpPortal, LoadView, ConstructLinked, LoadModel, TestCell,
        TestBoxAt, CellResult, DrawnModelResult, MeshResult, InstanceResult
    };
    struct Entry {
        uint32_t address;
        const char* name;
        Operation operation;
    };
    static const std::array<Entry, 11>& Entries();
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using CaptureBody = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct Counters {
        std::array<uint64_t, 11> owned_calls;
        std::array<uint64_t, 11> generated_body_calls;
        uint64_t unsupported_interiors;
        uint64_t unsupported_state;
        // Legacy images without CULL admitted by CullingCheckpoint's contract:
        // the owned state became the source's boundary state (no live view).
        uint64_t legacy_imports;
    };

    explicit CullingLibrary(ps2r::ipu::GuestMemory& memory);
    ~CullingLibrary();
    CullingLibrary(const CullingLibrary&) = delete;
    CullingLibrary& operator=(const CullingLibrary&) = delete;
    // Publish the whole producer/reader group only after every bind succeeds.
    // Failed rollback wrappers remain inactive and call their captured body.
    // Control calls captured originals, including interior entries. Selected
    // interiors require checkpoint migration and refuse explicitly; never run
    // an original reader against an owned result (or the opposite) — a refused
    // selected call therefore parks the leg visibly (GuestPark) instead of
    // falling back to one incoherent original call, and never aborts.
    bool Install(const CaptureBody& capture, const Installer& installer, bool selected);
    Counters Counts() const;
    std::string Census() const;
    // Complete-entry bridge. Invalid object ranges or missing source state
    // leave source objects/context unchanged and return false.
    bool Invoke(Operation operation, CpuContext& context);
    CullingState::Snapshot Capture() const { return state_.Capture(); }
    bool Restore(const CullingState::Snapshot& snapshot, std::string* error = nullptr) {
        return state_.Restore(snapshot, error);
    }
    void Reset() { state_.Reset(); }
    // CullingCheckpoint's legacy import: the admitted image proved no chunk
    // view is alive, so the owned state is the source's boundary state (empty).
    void ImportLegacy();

private:
    class CountsState;
    class Installation;
    template<class T> std::optional<T> Read(uint32_t address, size_t alignment = 16);
    template<class T> void Write(uint32_t address, const T& value);
    bool Range(uint32_t address, size_t size, size_t alignment = 16);
    bool SetUp(CpuContext& context, bool portal);
    bool LoadView(CpuContext& context);
    bool ConstructLinked(CpuContext& context);
    bool Produce(CpuContext& context, Operation operation);
    bool ReadResult(CpuContext& context, Operation operation);
    static uint32_t Visibility(const CullingMath::Classification& result);

    ps2r::ipu::GuestMemory& memory_;
    CullingState state_;
    std::atomic<uint64_t> legacy_imports_{0};
    std::shared_ptr<CountsState> counts_;
    std::shared_ptr<Installation> installation_;
};

}  // namespace title
