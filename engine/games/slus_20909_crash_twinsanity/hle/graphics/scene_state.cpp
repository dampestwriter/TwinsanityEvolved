#include "games/slus_20909_crash_twinsanity/hle/graphics/scene_state.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Graphics scene state: ") + why;
    return false;
}
void Put(std::vector<uint8_t>& bytes, uint64_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
}
void Rect(std::vector<uint8_t>& bytes, const ps2r::gs::LogicalRect& r) {
    for (auto n : {r.x0, r.y0, r.x1, r.y1}) Put(bytes, uint32_t(n));
}
bool Bounded(const ps2r::gs::LogicalRect& r) {
    // Unclipped triangle bounds can extend beyond the target. Preserve those
    // values, including empty rectangles, without unbounded signed arithmetic.
    for (auto n : {r.x0,r.y0,r.x1,r.y1})
        if (n < -65536 || n > 65536) return false;
    return true;
}
}

struct SceneState::Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;
    uint64_t Get(unsigned width = 4) {
        if (!ok || width > bytes.size() - at) { ok = false; return 0; }
        uint64_t n = 0;
        for (unsigned i = 0; i < width; ++i) n |= uint64_t(bytes[at++]) << (8 * i);
        return n;
    }
    ps2r::gs::LogicalRect Rect() {
        ps2r::gs::LogicalRect r;
        r.x0 = int32_t(Get()); r.y0 = int32_t(Get());
        r.x1 = int32_t(Get()); r.y1 = int32_t(Get());
        return r;
    }
    bool Raw(std::vector<uint8_t>& out, size_t size) {
        if (!ok || size > bytes.size() - at) { ok = false; return false; }
        out.assign(bytes.begin() + at, bytes.begin() + at + size);
        at += size;
        return true;
    }
};

bool SceneState::ValidateViewport(const Viewport& v, std::string* error) {
    if (!v.width || v.width > 16384 || !v.height || v.height > 16384 ||
        uint64_t(v.width) * v.height > 16 * 1024 * 1024 ||
        bool(v.content_width) != bool(v.content_height) ||
        v.content_width > 2048 || v.content_height > 2048 ||
        (v.color_key != UINT32_MAX && (v.color_key >= 4 * 1024 * 1024 || v.color_key % 8192)) ||
        v.buffer_width > 2048 || v.buffer_width % 64 ||
        (v.format != 0 && v.format != 1 && v.format != 2 && v.format != 10))
        return Bad(error, "unsupported host viewport");
    if (error) error->clear();
    return true;
}

bool SceneState::ValidateTarget(const ps2r::gs::TargetIdentity& id, const ps2r::gs::CoherenceState& h,
                                uint64_t order, std::string* error) {
    if (id.fbp >= 4 * 1024 * 1024 || id.fbp % 8192 ||
        id.width <= 0 || id.width > 2048 || id.width % 64 ||
        id.height <= 0 || id.height > 2048 || !id.scale || id.scale > 8 ||
        (id.render_psm != 0 && id.render_psm != 1 && id.render_psm != 2 && id.render_psm != 10) ||
        (id.render_fbw && (id.render_fbw > 2048 || id.render_fbw % 64)))
        return Bad(error, "unsupported target identity");
    // The alt16 companion's authorship/lanes/drawn rectangle name a second
    // attachment the codec does not carry; the main view's 16-bit keep mask
    // and lane record are the target's own (v3).
    if (h.alt_author_seq || h.lane_alt.seq || h.lane_alt.frame ||
        h.alt16_drawn.x0 || h.alt16_drawn.x1 || h.alt16_drawn.y0 || h.alt16_drawn.y1)
        return Bad(error, "unsupported history dependency (alt16 companion authorship)");
    if (h.last_z_seq > order) return Bad(error, "unsupported history dependency (z sequence beyond the draw order)");
    if (h.main_author_seq == UINT64_MAX || h.lane_main.seq > h.main_author_seq)
        return Bad(error, "unsupported history dependency (main lane authorship)");
    if (h.vram_coherent_class > 2 || h.last_zpsm > 0x3f ||
        (h.last_zbp != UINT32_MAX && (h.last_zbp >= 4 * 1024 * 1024 || h.last_zbp % 8192)) ||
        h.cd_copy.last_route < -1 || h.cd_copy.last_route > 0)
        return Bad(error, "unsupported history dependency (coherence class, Z buffer or copy route)");
    if (!Bounded({h.cd_copy.dirty_x0,h.cd_copy.dirty_y0,h.cd_copy.dirty_x1,h.cd_copy.dirty_y1}))
        return Bad(error,"destination copy bounds");
    for (const auto& r : {h.drawn_since_sync, h.z_drawn_since_sync, h.valid_rect,
                          h.alt16_stale, h.alt16_drawn})
        if (!Bounded(r)) return Bad(error, "rectangle bound");
    if (error) error->clear();
    return true;
}

bool SceneState::Validate(const Snapshot& s, std::string* error) {
    if (s.targets.size() > 64 || s.order == UINT64_MAX) return Bad(error, "owner/order bound");
    if (!ValidateViewport(s.viewport,error)) return false;
    uint64_t total = 68;
    uint32_t previous = 0;
    bool recent = s.recent == UINT32_MAX;
    for (size_t i = 0; i < s.targets.size(); ++i) {
        const auto& t = s.targets[i];
        const auto& id = t.identity;
        const auto& h = t.history;
        if (i && id.fbp <= previous) return Bad(error, "unsupported target identity");
        if (!ValidateTarget(id, h, s.order, error)) return false;
        previous = id.fbp;
        recent |= s.recent == id.fbp;
        const auto pixels = uint64_t(id.width) * id.height * id.scale * id.scale;
        total += 216 + pixels * (h.cd_copy.valid ? 12 : 8);
        if (total > kMaxBytes || t.color.size() != pixels * 4 ||
            t.depth.size() != pixels || t.copy.size() != (h.cd_copy.valid ? pixels * 4 : 0))
            return Bad(error, "attachment extent/budget");
        for (float f : t.depth)
            if (!std::isfinite(f) || f < 0 || f > 1) return Bad(error, "depth outside host range");
    }
    if (!recent) return Bad(error, "recent target is absent");
    if (s.order && s.targets.empty()) return Bad(error,"draw history has no shared targets");
    if (error) error->clear();
    return true;
}

bool SceneState::Encode(const Snapshot& s, std::vector<uint8_t>& out, std::string* error) {
    if (!Validate(s, error)) return false;
    std::vector<uint8_t> bytes;
    Put(bytes,kTag); Put(bytes,kVersion); Put(bytes,s.targets.size());
    Put(bytes,s.display_frame); Put(bytes,s.game_frame); Put(bytes,s.recent);
    Put(bytes,s.order,8); Put(bytes,s.epoch,8);
    const auto& v = s.viewport;
    for (auto n : {v.width,v.height,v.content_width,v.content_height,
                   v.color_key,v.buffer_width,v.format}) Put(bytes,n);
    for (const auto& t : s.targets) {
        const auto& id = t.identity;
        const auto& h = t.history;
        for (auto n : {id.fbp,uint32_t(id.width),uint32_t(id.height),id.render_fbw}) Put(bytes,n);
        Put(bytes,id.scale);
        const uint32_t flags = uint32_t(id.has_32bit_content) | uint32_t(id.was_32bit) << 1 |
            uint32_t(h.has_content) << 2 | uint32_t(h.needs_frame_clear) << 3 |
            uint32_t(h.ever_composited) << 4 | uint32_t(h.last_z_wrote) << 5 |
            uint32_t(h.cd_copy.valid) << 6 | uint32_t(id.was_16bit) << 7;
        Put(bytes,flags);
        Put(bytes,id.render_psm); Put(bytes,h.sync_keep_mask16); // v3
        for (const auto& r : {h.drawn_since_sync,h.z_drawn_since_sync,h.valid_rect,h.alt16_stale}) Rect(bytes,r);
        Put(bytes,h.last_draw_frame); Put(bytes,h.last_draw_gfx_epoch,8);
        Put(bytes,h.last_clear_frame); Put(bytes,h.last_zbp); Put(bytes,h.last_zpsm);
        Put(bytes,h.last_z_seq,8); Put(bytes,h.last_z_write_frame);
        Put(bytes,h.main_author_seq,8); Put(bytes,h.lane_main.seq,8); Put(bytes,h.lane_main.frame);
        Put(bytes,h.vram_coherent_epoch,8); Put(bytes,h.vram_coherent_class);
        for (auto n : {h.cd_copy.dirty_x0,h.cd_copy.dirty_y0,h.cd_copy.dirty_x1,h.cd_copy.dirty_y1,
                       int32_t(h.cd_copy.last_route)}) Put(bytes,uint32_t(n));
        bytes.insert(bytes.end(),t.color.begin(),t.color.end());
        for (float f : t.depth) { uint32_t bits; std::memcpy(&bits,&f,4); Put(bytes,bits); }
        bytes.insert(bytes.end(),t.copy.begin(),t.copy.end());
    }
    out = std::move(bytes);
    return true;
}

bool SceneState::Decode(const std::vector<uint8_t>& bytes, Snapshot& out, std::string* error) {
    if (bytes.size() < 68 || bytes.size() > kMaxBytes) return Bad(error,"payload extent");
    Reader r{bytes};
    if (r.Get() != kTag) return Bad(error,"tag/version");
    const auto version = uint32_t(r.Get());
    if (!Accepts(version)) return Bad(error,"tag/version");
    const auto count = r.Get();
    if (count > 64) return Bad(error,"target count");
    Snapshot s;
    s.display_frame = uint32_t(r.Get()); s.game_frame = uint32_t(r.Get()); s.recent = uint32_t(r.Get());
    s.order = r.Get(8); s.epoch = r.Get(8);
    auto& v = s.viewport;
    v.width = uint32_t(r.Get()); v.height = uint32_t(r.Get());
    v.content_width = uint32_t(r.Get()); v.content_height = uint32_t(r.Get());
    v.color_key = uint32_t(r.Get()); v.buffer_width = uint32_t(r.Get()); v.format = uint32_t(r.Get());
    for (uint32_t i = 0; i < count; ++i) {
        Target t;
        auto& id = t.identity;
        auto& h = t.history;
        id.fbp = uint32_t(r.Get()); id.width = int32_t(r.Get()); id.height = int32_t(r.Get());
        id.render_fbw = uint32_t(r.Get());
        const auto scale = r.Get(), flags = r.Get();
        const uint32_t flag_mask = version >= 3 ? 255u : 127u;
        if (!r.ok || !scale || scale > 8 || flags & ~flag_mask ||
            id.width <= 0 || id.width > 2048 || id.height <= 0 || id.height > 2048)
            return Bad(error,"target header");
        id.scale = uint8_t(scale);
        id.has_32bit_content = flags & 1; id.was_32bit = flags & 2; h.has_content = flags & 4;
        h.needs_frame_clear = flags & 8; h.ever_composited = flags & 16;
        h.last_z_wrote = flags & 32; h.cd_copy.valid = flags & 64;
        id.was_16bit = flags & 128;
        id.render_psm = version >= 3 ? uint32_t(r.Get()) : 0; // v2: 32-bit targets only
        const auto keep16 = version >= 3 ? r.Get() : 0;
        if (keep16 > 0xffff) return Bad(error,"target header");
        h.sync_keep_mask16 = uint16_t(keep16);
        h.drawn_since_sync = r.Rect(); h.z_drawn_since_sync = r.Rect();
        h.valid_rect = r.Rect(); h.alt16_stale = r.Rect();
        h.last_draw_frame = uint32_t(r.Get()); h.last_draw_gfx_epoch = r.Get(8);
        h.last_clear_frame = uint32_t(r.Get()); h.last_zbp = uint32_t(r.Get()); h.last_zpsm = uint32_t(r.Get());
        h.last_z_seq = r.Get(8); h.last_z_write_frame = uint32_t(r.Get());
        h.main_author_seq = r.Get(8); h.lane_main.seq = r.Get(8); h.lane_main.frame = uint32_t(r.Get());
        h.vram_coherent_epoch = r.Get(8);
        const auto coherent = r.Get();
        h.cd_copy.dirty_x0 = int32_t(r.Get()); h.cd_copy.dirty_y0 = int32_t(r.Get());
        h.cd_copy.dirty_x1 = int32_t(r.Get()); h.cd_copy.dirty_y1 = int32_t(r.Get());
        const auto route = int32_t(r.Get());
        if (!r.ok || coherent > 2 || route < -1 || route > 0) return Bad(error,"copy history");
        h.vram_coherent_class = uint8_t(coherent); h.cd_copy.last_route = int8_t(route);
        const auto pixels = size_t(id.width) * id.height * id.scale * id.scale;
        if (pixels > (bytes.size() - r.at) / (h.cd_copy.valid ? 12 : 8))
            return Bad(error,"truncated attachments");
        if (!r.Raw(t.color,pixels * 4)) return Bad(error,"color extent");
        t.depth.resize(pixels);
        for (auto& f : t.depth) { const auto bits = uint32_t(r.Get()); std::memcpy(&f,&bits,4); }
        if (h.cd_copy.valid && !r.Raw(t.copy,pixels * 4)) return Bad(error,"copy extent");
        s.targets.push_back(std::move(t));
    }
    if (!r.ok || r.at != bytes.size() || !Validate(s,error)) return Bad(error,"noncanonical payload");
    out = std::move(s);
    return true;
}
} // namespace title
