#include "games/slus_20909_crash_twinsanity/hle/graphics/presentation_state.h"

namespace title {
namespace {
bool Bad(std::string* error,const char* why) {
    if (error) *error = std::string("Graphics presentation state: ") + why;
    return false;
}
bool Empty(const FrameEnvironment::SceneFrame& f) {
    return !f.width && !f.height && !f.color_buffer && !f.depth_buffer && !f.source_frame && !f.epoch;
}
void Put(std::vector<uint8_t>& out,uint64_t value,unsigned bytes = 4) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(uint8_t(value>>(8*i)));
}
void Frame(std::vector<uint8_t>& out,const FrameEnvironment::SceneFrame& f) {
    for (auto word : {f.color_buffer,f.depth_buffer,f.width,f.height,f.source_frame}) Put(out,word);
    Put(out,f.epoch,8);
}
}
struct PresentationState::Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;
    uint64_t Get(unsigned size = 4) {
        if (!ok || size > bytes.size()-at) { ok = false; return 0; }
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i) result |= uint64_t(bytes[at++])<<(8*i);
        return result;
    }
    FrameEnvironment::SceneFrame Frame() {
        FrameEnvironment::SceneFrame f;
        f.color_buffer = uint32_t(Get()); f.depth_buffer = uint32_t(Get());
        f.width = uint32_t(Get()); f.height = uint32_t(Get());
        f.source_frame = uint32_t(Get()); f.epoch = Get(8);
        return f;
    }
    bool Image(PresentationState::Image& image) {
        image.kind = Kind(Get()); image.frame = Frame();
        image.width = uint32_t(Get()); image.height = uint32_t(Get()); image.scale = uint32_t(Get());
        const auto length = Get();
        if (!ok || length > bytes.size()-at) return false;
        image.rgba.assign(bytes.begin()+at,bytes.begin()+at+length);
        at += length;
        return true;
    }
};
bool PresentationState::Validate(const Image& i,std::string* error) {
    if (i.kind == Kind::None) {
        if (!Empty(i.frame) || i.width || i.height || i.scale || !i.rgba.empty())
            return Bad(error,"inactive image contains omitted state");
        return true;
    }
    const auto& f = i.frame;
    if ((i.kind != Kind::Scene && i.kind != Kind::Image) || !f.epoch ||
        !f.width || f.width > 2048 || f.width % 64 || !f.height || f.height > 2048 ||
        f.color_buffer >= 4*1024*1024 || f.color_buffer % 8192 ||
        f.depth_buffer >= 4*1024*1024 || f.depth_buffer % 8192 ||
        !i.width || !i.height || !i.scale || i.scale > 8 || i.width > 8192 || i.height > 8192 ||
        i.width % i.scale || i.height % i.scale ||
        uint64_t(f.width)*i.scale > i.width || uint64_t(f.height)*i.scale > i.height)
        return Bad(error,"image identity or dimensions are unsupported");
    const auto extent = uint64_t(i.width)*i.height*4;
    if (extent > kMaxBytes-132 ||
        i.rgba.size() != (i.kind == Kind::Image ? extent : 0))
        return Bad(error,"image contents or budget differ");
    return true;
}
bool PresentationState::Validate(const Snapshot& s,std::string* error) {
    if (!Validate(s.movie,error) || !Validate(s.display,error)) return false;
    if (s.display.kind == Kind::None) {
        if (!Empty(s.display_source)) return Bad(error,"inactive display has a source");
    } else {
        const auto& f = s.display.frame;
        const auto& source = s.display_source;
        const uint64_t pixels = uint64_t(f.width)*f.height;
        if (s.display.kind != Kind::Image || f.color_buffer ||
            source.width != f.width || source.height != f.height ||
            source.source_frame != f.source_frame || source.epoch != f.epoch ||
            source.depth_buffer != f.depth_buffer ||
            source.color_buffer != pixels*2/8192*8192 ||
            source.depth_buffer != pixels*6/8192*8192 ||
            pixels*10 > 4*1024*1024)
            return Bad(error,"converted image has no matching source frame");
    }
    if (s.movie.rgba.size()+s.display.rgba.size() > kMaxBytes-132)
        return Bad(error,"combined image budget exceeded");
    if (error) error->clear();
    return true;
}
bool PresentationState::Encode(const Snapshot& s,std::vector<uint8_t>& out,std::string* error) {
    if (!Validate(s,error)) return false;
    std::vector<uint8_t> next;
    Put(next,kTag); Put(next,kVersion); Frame(next,s.display_source);
    for (const auto* image : {&s.movie,&s.display}) {
        Put(next,uint32_t(image->kind)); Frame(next,image->frame);
        for (auto value : {image->width,image->height,image->scale,uint32_t(image->rgba.size())})
            Put(next,value);
        next.insert(next.end(),image->rgba.begin(),image->rgba.end());
    }
    out = std::move(next);
    return true;
}
bool PresentationState::Decode(const std::vector<uint8_t>& bytes,Snapshot& out,std::string* error) {
    if (bytes.size() < 132 || bytes.size() > kMaxBytes) return Bad(error,"section extent differs");
    Reader reader{bytes};
    if (reader.Get() != kTag || reader.Get() != kVersion) return Bad(error,"tag/version differs");
    Snapshot next;
    next.display_source = reader.Frame();
    if (!reader.Image(next.movie) || !reader.Image(next.display) ||
        !reader.ok || reader.at != bytes.size() || !Validate(next,error))
        return Bad(error,"invalid image encoding");
    out = std::move(next);
    return true;
}
} // namespace title
