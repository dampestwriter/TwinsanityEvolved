// savestate.cpp -- see savestate.h.
#include "savestate.h"

#include <cstdio>
#include <mutex>

namespace te::stream { class StreamService; StreamService& Service(); }
namespace te::audio { class AudioService; AudioService& Service(); }
namespace te::movie { class MoviePlayer; MoviePlayer& Service(); }
namespace te::gfx { void RegisterStateSection(); }

namespace te::savestate {

void PrepareOwners() {
    te::stream::Service();
    te::audio::Service();
    te::movie::Service();
    te::gfx::RegisterStateSection();
}

ps2r::savestate::SectionRegistry& Registry() {
    static ps2r::savestate::SectionRegistry registry;
    return registry;
}

void Register(uint32_t tag, const char* owner, bool required,
              std::function<void(std::vector<uint8_t>&)> serialize,
              std::function<bool(const Blob&, std::string*)> restore) {
    SectionSpec spec;
    spec.tag = tag;
    spec.version = 1;
    spec.required = required;
    spec.owner = owner;
    spec.serialize = std::move(serialize);
    spec.restore = std::move(restore);
    Registry().Register(spec);
}

void PutString(std::vector<uint8_t>& v, const std::string& s) {
    Put(v, static_cast<uint32_t>(s.size()));
    PutBytes(v, s.data(), s.size());
}

bool GetString(PayloadReader& in, std::string& s, size_t max) {
    uint32_t n = 0;
    if (!in.Get(&n) || n > max || n > in.Remaining()) return false;
    s.resize(n);
    return n == 0 || in.GetBytes(s.data(), n);
}

bool GetVector(PayloadReader& in, std::vector<uint8_t>& out, size_t max) {
    uint32_t n = 0;
    if (!in.Get(&n) || n > max || n > in.Remaining()) return false;
    out.resize(n);
    return n == 0 || in.GetBytes(out.data(), n);
}

namespace {
Handler g_handler = nullptr;
std::string g_requested, g_load_requested;
bool g_has_request = false, g_has_load_request = false;
}  // namespace

void Checkpoint(uint64_t boundary) {
    if (g_handler) g_handler(boundary);
}

void SetCheckpointHandler(Handler handler) { g_handler = handler; }

void RequestSave(const std::string& path) {
    g_requested = path;
    g_has_request = true;
}

bool TakeSaveRequest(std::string& path) {
    if (!g_has_request) return false;
    g_has_request = false;
    path = g_requested;
    return true;
}

void RequestLoad(const std::string& path) {
    g_load_requested = path;
    g_has_load_request = true;
}

bool TakeLoadRequest(std::string& path) {
    if (!g_has_load_request) return false;
    g_has_load_request = false;
    path = g_load_requested;
    return true;
}

}  // namespace te::savestate
