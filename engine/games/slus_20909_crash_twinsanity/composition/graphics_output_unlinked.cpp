// GraphicsOutput: the GXPD section -- the owned groups a boundary capture
// could not carry (the title's OPEN recording at SyncV: recorded before
// Present, Linked by StartNextFrame after it, TR renderer.cpp:436-440). The
// digest is published so the restored session attributes the one Link that
// draws without them; nothing is re-queued (the closures are host-side).
#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"

#include <stdexcept>

#include "engine/core/log/log.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"

namespace ps2r::app {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Graphics unlinked groups: ") + why;
    return false;
}
void Put(std::vector<uint8_t>& out, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
constexpr uint32_t kBuckets = ::title::Graphics::kBucketCount;
constexpr size_t kExtent = 4 * 6 + 4 * kBuckets; // tag, version, frame, groups, items, owned, per bucket
}  // namespace

bool GraphicsOutput::EncodeUnlinked(const Unlinked& u, std::vector<uint8_t>& out, std::string* error) {
    uint64_t sum = 0;
    for (const auto count : u.bucket_items) sum += count;
    if (sum != u.items || u.owned_items > u.items || (u.items && !u.groups) || (!u.items && u.groups))
        return Bad(error, "digest is inconsistent");
    std::vector<uint8_t> bytes;
    Put(bytes, kUnlinkedTag, 4); Put(bytes, kUnlinkedVersion, 4);
    Put(bytes, u.source_frame, 4); Put(bytes, u.groups, 4); Put(bytes, u.items, 4); Put(bytes, u.owned_items, 4);
    for (const auto count : u.bucket_items) Put(bytes, count, 4);
    out = std::move(bytes);
    if (error) error->clear();
    return true;
}

bool GraphicsOutput::DecodeUnlinked(const std::vector<uint8_t>& bytes, Unlinked& out, std::string* error) {
    if (bytes.size() != kExtent) return Bad(error, "section extent differs");
    size_t at = 0;
    const auto take = [&] {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[at++]) << (i * 8);
        return value;
    };
    if (take() != kUnlinkedTag || take() != kUnlinkedVersion) return Bad(error, "tag/version differs");
    Unlinked u;
    u.source_frame = take(); u.groups = take(); u.items = take(); u.owned_items = take();
    uint64_t sum = 0;
    for (auto& count : u.bucket_items) { count = take(); sum += count; }
    if (sum != u.items || u.owned_items > u.items || (u.items && !u.groups) || (!u.items && u.groups))
        return Bad(error, "digest is inconsistent");
    out = u;
    if (error) error->clear();
    return true;
}

bool GraphicsOutput::RegisterUnlinkedState(StateRestorer& restorer) {
    savestate::SectionSpec spec;
    spec.tag = kUnlinkedTag;
    spec.version = kUnlinkedVersion;
    spec.owner = "Graphics unlinked owned groups";
    // Optional both ways: a frozen anchor or an image captured with a closed
    // recording carries none (absent = nothing was unlinked).
    spec.required = false;
    spec.capture_required = false;
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        std::string error;
        if (!SettleOwnedForCapture(&error)) throw std::runtime_error("Graphics unlinked groups capture: " + error);
        if (!EncodeUnlinked(unlinked_, bytes, &error)) throw std::runtime_error("Graphics unlinked groups capture: " + error);
    };
    spec.validate = [](const savestate::Blob& blob, std::string* error) {
        Unlinked u;
        return blob.version == kUnlinkedVersion && DecodeUnlinked(blob.payload, u, error);
    };
    spec.restore = [this](const savestate::Blob& blob, std::string* error) {
        Unlinked u;
        if (!DecodeUnlinked(blob.payload, u, error)) return false;
        restored_unlinked_ = u;
        if (!u.items) return true;
        // The image's frame was recorded before its Present and never Linked
        // in this session: the first restored Link draws it without these
        // groups. Attributed once per item at the Link producer (the same
        // route as a title-discarded group), so the exit census and the
        // triage bundle name the omission instead of a frame glitch.
        for (uint32_t i = 0; i < u.items; ++i)
            refusals_.Refuse(::title::GraphicsRefusals::Family::Strips, ::title::GraphicsRefusals::Reason::Ordering,
                             ::title::GraphicsService::kLinkAddress, ::title::GraphicsService::kRestoredUnlinked);
        PS2R_LOG(HOST, Info, "Graphics: the restored boundary left %u owned groups (%u items, %u owned) of source frame %u "
                 "unlinked; the first Link draws without them (GXPD)", u.groups, u.items, u.owned_items, u.source_frame);
        return true;
    };
    spec.absent = [this] { restored_unlinked_ = Unlinked{}; };
    return restorer.Registry().Register(spec);
}

}  // namespace ps2r::app
