#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "engine/ee/cpu/guest_park.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {
constexpr std::array<uint32_t, 3> kEntries{
    MovieGraphics::kDecodeAndUpload, MovieGraphics::kQueuePicture, MovieGraphics::kDraw};
uint32_t Arg(const CpuContext& context) { return context.cpuRegs.GPR.r[4].UL[0]; }
void Return(CpuContext& context, int32_t result) { context.cpuRegs.GPR.r[2].SD[0] = result; }
bool SampleExtent(const ps2r::gs::DrawIr& draw, const ps2r::ipu::DecodedPicture& picture,
                  uint32_t& width, uint32_t& height) {
    const auto& t = draw.tex;
    if (!draw.tex_enabled || !draw.use_fst || !t.fst || draw.ctx != 1 || draw.prim_type != 6 ||
        t.psm || t.tw != 10 || t.th != 10 || t.wms || t.wmt || t.mmag || t.mmin || t.mxl ||
        draw.verts.empty() || draw.vertex_count != draw.verts.size() ||
        picture.width <= 0 || picture.height <= 0 ||
        picture.width > 4096 || picture.height > 4096 ||
        picture.rgba.size() != size_t(picture.width) * picture.height * 4) return false;
    uint32_t u = 0, v = 0;
    for (const auto& vertex : draw.verts) {
        u = std::max(u, uint32_t(vertex.uv_u));
        v = std::max(v, uint32_t(vertex.uv_v));
    }
    // ceil(maxUV - 0.5), in source FST's exact sixteenth-texel units.
    width = (u + 7) / 16;
    height = (v + 7) / 16;
    return width && height && width <= uint32_t(picture.width) &&
        height <= uint32_t(picture.height) &&
        ((width + 16) & ~15u) <= 4096 && ((height + 16) & ~15u) <= 4096;
}
}  // namespace

MovieGraphics::MovieGraphics(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder,
                             Graphics& graphics, Hooks hooks)
    : memory_(memory), decoder_(decoder), graphics_(&graphics), hooks_(std::move(hooks)) {}

MovieGraphics::MovieGraphics(ps2r::ipu::GuestMemory& memory, MovieDecoder& decoder,
                             Hooks hooks, OutputServices output)
    : memory_(memory), decoder_(decoder), hooks_(std::move(hooks)), output_(std::move(output)) {}

int MovieGraphics::Install(const MovieDecoder::Capture& capture,
                           const MovieDecoder::Installer& install, bool selected) {
    if (installed_ || !capture || !install || !decoder_.CanCheckpoint()) return 0;
    if (selected) {
        if (!hooks_.picture || !hooks_.present) return 0;
        if (graphics_) {
            if (!hooks_.register_image || !hooks_.record) return 0;
        } else if (!output_.upload || !output_.record || hooks_.register_image || hooks_.record) {
            return 0; // no ambiguous or incomplete output selection
        }
    }
    for (size_t i = 0; i < kEntries.size(); ++i) {
        originals_[i] = capture(kEntries[i]);
        if (!originals_[i]) return 0;
    }
    if (selected && !decoder_.SetPictureServices({
            [this](uint32_t d, uint32_t w, uint32_t h) { return Begin(d, w, h); },
            [this](CpuContext& c) { ReadPicture(c); }, [this] { Close(); }})) return 0;
    for (size_t i = 0; i < kEntries.size(); ++i) {
        if (!install(kEntries[i], [this, i](CpuContext& c, uint32_t e) { Dispatch(i, c, e); })) {
            for (size_t j = 0; j < i; ++j) install(kEntries[j], originals_[j]);
            if (selected) decoder_.SetPictureServices({});
            return 0;
        }
    }
    selected_ = selected;
    installed_ = true;
    return 3;
}

void MovieGraphics::Dispatch(size_t index, CpuContext& context, uint32_t entry) {
    if (!selected_) {
        ++counts_.original_calls[index];
        originals_[index](context, entry);
        return;
    }
    // Selection owns these picture IDs for its entire lifetime, including
    // before Start and after Close. A stale call cannot reinterpret an ID as
    // a packet address. Neither an inactive owner nor a split continuation
    // is permission to return to retained transport.
    if (!Active() || (entry != 0 && entry != kEntries[index])) Fail(entry);
    if (index == 0) DecodeAndUpload(context);
    else if (index == 1) QueuePicture(context);
    else Draw(context);
}

[[noreturn]] void MovieGraphics::Fail(uint32_t entry) {
    ++counts_.errors;
    GuestPark::Forever("movie_graphics_invalid_intent", entry);
}

bool MovieGraphics::Begin(uint32_t decoder, uint32_t width, uint32_t height) {
    if (decoder < 0x80 || !memory_.Translate(decoder - 0x80, 0x1e8) ||
        width == 0 || height == 0 || width > 4096 || height > 4096 ||
        (width & 15) || (height & 15)) return false;
    Close();
    controller_ = decoder - 0x80;
    width_ = width;
    height_ = height;
    for (uint32_t offset : {0x38u, 0x3cu, 0x40u, 0x44u}) memory_.Write32(controller_ + offset, 0);
    return true;
}

void MovieGraphics::CollectPictures() {
    const uint32_t q0 = memory_.Read32(controller_ + 0x38);
    const uint32_t q1 = memory_.Read32(controller_ + 0x3c);
    const uint32_t pending = memory_.Read32(controller_ + 0x44);
    for (auto it = pictures_.begin(); it != pictures_.end();) {
        if (it->first == q0 || it->first == q1 || it->first == pending || it->first == latest_id_) ++it;
        else it = pictures_.erase(it);
    }
}

void MovieGraphics::ReadPicture(CpuContext& context) {
    if (!Active() || Arg(context) != controller_ + 0x80 || next_id_ == 0)
        Fail(kDecodeAndUpload);
    CollectPictures();
    if (pictures_.size() > 4) Fail(kDecodeAndUpload);
    ps2r::ipu::DecodedPicture picture;
    hooks_.picture(context, picture);
    if (context.cpuRegs.GPR.r[2].SL[0] < 0) return;
    if (picture.width != int(width_) || picture.height != int(height_) ||
        picture.rgba.size() != size_t(width_) * height_ * 4) Fail(kDecodeAndUpload);
    // The existing RGB32 writer sets alpha 0x80, independently of FFmpeg's
    // opaque alpha. Retain those actual uploaded bytes in the owned image.
    for (size_t alpha = 3; alpha < picture.rgba.size(); alpha += 4) picture.rgba[alpha] = 0x80;
    const uint32_t id = next_id_++;
    pictures_.emplace(id, std::make_shared<const Image>(Image{id, std::move(picture)}));
    latest_id_ = id;
    ++counts_.decoded;
}

void MovieGraphics::DecodeAndUpload(CpuContext& context) {
    const uint32_t output = Arg(context);
    if (!Active() || output != controller_ + 0x44) Fail(kDecodeAndUpload);
    const uint32_t prior = latest_id_;
    decoder_.DecodePicture(context);
    if (context.cpuRegs.GPR.r[2].UL[0] == 0) return;
    if (latest_id_ == prior || !pictures_.count(latest_id_)) Fail(kDecodeAndUpload);
    memory_.Write32(output, latest_id_);
    CollectPictures();
    Return(context, 1);
}

bool MovieGraphics::QueueDecoded(uint32_t player) {
    if (!Active() || player != controller_) Fail(kDecodeAndUpload);
    const uint32_t queued = memory_.Read32(player + 0x40);
    const uint32_t decoded = memory_.Read32(player + 0x44);
    if (queued > 2) Fail(kDecodeAndUpload);
    if (!decoded || queued == 2) return false;
    if (!pictures_.count(decoded)) Fail(kDecodeAndUpload);
    memory_.Write32(player + 0x38 + queued * 4, decoded);
    memory_.Write32(player + 0x40, queued + 1);
    return true;
}

void MovieGraphics::QueuePicture(CpuContext& context) {
    if (!Active() || Arg(context) != controller_) Fail(kQueuePicture);
    const uint32_t queued = memory_.Read32(controller_ + 0x40);
    if (queued == 0) { ++counts_.empty_queues; return; }
    if (queued > 2) Fail(kQueuePicture);
    const auto picture = pictures_.find(memory_.Read32(controller_ + 0x38));
    if (picture == pictures_.end()) Fail(kQueuePicture);
    if (graphics_) {
        Graphics::Handle next;
        const ps2r::render::SurfaceDesc description{
            width_, height_, ps2r::render::SurfaceFormat::Rgba8, false, 1};
        if (!hooks_.register_image(description, next)) Fail(kQueuePicture);
        if (graphics_->RecordUpload(graphics_->End(kBucket), next,
                {0, 0, int(width_), int(height_)}, size_t(width_) * 4,
                picture->second->pixels.rgba) != Graphics::Result::Ok) {
            graphics_->ReleaseResource(next);
            Fail(kQueuePicture);
        }
        if (has_image_) graphics_->ReleaseResource(image_);
        image_ = next;
    } else {
        if (!output_.upload(picture->second)) Fail(kQueuePicture);
    }
    displayed_ = picture->second;
    if (sampled_ && graphics_) graphics_->ReleaseResource(view_image_);
    view_image_ = {};
    sampled_.reset();
    has_image_ = true;
    memory_.Write32(controller_ + 0x38, memory_.Read32(controller_ + 0x3c));
    memory_.Write32(controller_ + 0x3c, 0);
    memory_.Write32(controller_ + 0x40, queued - 1);
    ++counts_.uploaded;
    CollectPictures();
}

bool MovieGraphics::BuildDraw(const View& view, ps2r::gs::DrawIr& output) {
    // Bound before arithmetic/conversions. Public source draws at most 1024
    // pixels; movie dimensions are the format-audited 16-aligned MPEG planes.
    if (view.screen_width < 32 || view.screen_width > 1024 ||
        view.screen_height == 0 || view.screen_height > 1024 ||
        view.movie_width == 0 || view.movie_width > 4096 || (view.movie_width & 15) ||
        view.movie_height == 0 || view.movie_height > 4096 || (view.movie_height & 15)) return false;
    int x = 0, y = 0, w = int(view.screen_width), h = int(view.screen_height);
    if (view.tv_aspect > 0.0f) {
        // The selected shape: the picture keeps its own (TR movie.cpp:224's
        // rule for any TV ratio), the frame being anamorphic at tv_aspect.
        const float picture = view.wide_movie ? 16.0f / 9.0f : 4.0f / 3.0f;
        if (picture < view.tv_aspect - 1e-4f) {
            w = (int(float(view.screen_width) * picture / view.tv_aspect) / 32) * 32;
            x = (int(view.screen_width) - w) / 2;
        } else if (picture > view.tv_aspect + 1e-4f) {
            h = int(float(view.screen_height) * view.tv_aspect / picture);
            y = (int(view.screen_height) - h) / 2;
        }
    } else if (view.wide_tv && !view.wide_movie) {
        x = int(float(view.screen_width) * .5f - float(view.screen_width) * .375f);
        w = int(float(view.screen_width) * .75f);
    } else if (!view.wide_tv && view.wide_movie) {
        y = int(float(view.screen_height) * .5f - float(view.screen_width) * .28125f);
        h = int(float(view.screen_width) * .5625f);
    }
    if (x < 0 || y < 0 || w < 32 || h == 0) return false;
    const uint32_t strips = uint32_t(w) / 32;
    const uint32_t step = view.movie_width / strips;
    const uint32_t skipped = view.movie_height > view.screen_height
        ? (view.movie_height - view.screen_height) / 2 : 0;
    if (step == 0) return false;
    ps2r::gs::DrawIr draw;
    draw.prim_type = 6;
    draw.ctx = 1;
    draw.tex_enabled = draw.use_fst = draw.tex.fst = true;
    draw.tex.tbw = ((view.movie_width + 63) / 64) * 64;
    draw.tex.tw = draw.tex.th = 10;
    draw.tex.tcc = draw.tex.tfx = draw.tex.cld = 1;
    draw.raster.frame.fbw = 512;
    draw.raster.frame.fbmsk = 0xff000000;
    draw.raster.zbuf.zmsk = true;
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 2;
    draw.raster.scissor.x1 = draw.raster.scissor.y1 = 1024;
    draw.raster.colclamp = true;
    draw.raster.dimx = 0x6071243571603524ull;
    for (uint32_t strip = 0; strip < strips; ++strip) {
        const auto vertex = [&](bool right, bool bottom) {
            ps2r::gs::VertexRec v{};
            v.x = float(x + int(strip * 32) + (right ? 32 : 0));
            v.y = float(y + (bottom ? h : 0));
            v.z = float(std::numeric_limits<uint32_t>::max());
            v.uv_u = uint16_t((strip + (right ? 1 : 0)) * step * 16 + 8);
            v.uv_v = uint16_t((bottom ? view.movie_height - skipped : skipped) * 16 + 8);
            v.s = float(v.uv_u) / 16;
            v.t = float(v.uv_v) / 16;
            v.q = 1;
            v.r = v.g = v.b = v.a = 0x80;
            v.draw_kick = 1;
            return v;
        };
        const auto tl = vertex(false, false), tr = vertex(true, false);
        const auto bl = vertex(false, true), br = vertex(true, true);
        draw.verts.insert(draw.verts.end(), {tl, tr, bl, tr, br, bl});
    }
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.verts_incl = 1;
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    output = std::move(draw);
    return true;
}

bool MovieGraphics::BuildImageView(const ps2r::gs::DrawIr& draw,
                                  const ps2r::ipu::DecodedPicture& source,
                                  ps2r::ipu::DecodedPicture& output) {
    uint32_t width = 0, height = 0;
    if (!SampleExtent(draw, source, width, height)) return false;
    ps2r::ipu::DecodedPicture view;
    // One replicated tap is necessary even for a full-picture view. Keeping
    // this allocation 16-aligned also meets GraphicsTargets' movie contract.
    view.width = int((width + 16) & ~15u);
    view.height = int((height + 16) & ~15u);
    const size_t stride = size_t(view.width) * 4;
    view.rgba.resize(stride * view.height);
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = view.rgba.data() + size_t(y) * stride;
        std::memcpy(row, source.rgba.data() + size_t(y) * source.width * 4, size_t(width) * 4);
        for (uint32_t x = width; x < uint32_t(view.width); ++x)
            std::memcpy(row + size_t(x) * 4, row + size_t(width - 1) * 4, 4);
    }
    for (uint32_t y = height; y < uint32_t(view.height); ++y)
        std::memcpy(view.rgba.data() + size_t(y) * stride,
                    view.rgba.data() + size_t(height - 1) * stride, stride);
    output = std::move(view);
    return true;
}

void MovieGraphics::SelectView(const ps2r::gs::DrawIr& draw) {
    uint32_t width = 0, height = 0;
    if (!displayed_ || !SampleExtent(draw, displayed_->pixels, width, height)) Fail(kDraw);
    if (sampled_ && sampled_->source_id == displayed_->id &&
        sampled_->sample_width == width && sampled_->sample_height == height) return;
    ps2r::ipu::DecodedPicture pixels;
    if (next_id_ == 0 || !BuildImageView(draw, displayed_->pixels, pixels)) Fail(kDraw);
    auto view = std::make_shared<const Image>(
        Image{next_id_++, std::move(pixels), displayed_->id, width, height});
    if (graphics_) {
        Graphics::Handle next;
        const auto& p = view->pixels;
        if (!hooks_.register_image({uint32_t(p.width), uint32_t(p.height),
                ps2r::render::SurfaceFormat::Rgba8, false, 1}, next)) Fail(kDraw);
        if (graphics_->RecordUpload(graphics_->End(kBucket), next,
                {0, 0, p.width, p.height}, size_t(p.width) * 4, p.rgba) != Graphics::Result::Ok) {
            graphics_->ReleaseResource(next);
            Fail(kDraw);
        }
        if (sampled_) graphics_->ReleaseResource(view_image_);
        view_image_ = next;
    }
    sampled_ = std::move(view);
    ++counts_.views;
}

void MovieGraphics::Draw(CpuContext& context) {
    if (!Active() || Arg(context) != controller_) Fail(kDraw);
    if (memory_.Read32(controller_ + 0x1e4) < 2) { ++counts_.priming_draws; return; }
    if (!has_image_) Fail(kDraw);
    const auto* wide = memory_.Translate(kWidescreenTv, 1);
    if (!wide) Fail(kDraw);
    View view{memory_.Read32(kDisplayWidth), memory_.Read32(kDisplayHeight),
        memory_.Read32(controller_ + 4), memory_.Read32(controller_ + 8),
        *wide != 0, (memory_.Read32(controller_) & 0x40) != 0};
    // The selected ratio supersedes the title's flag; 4:3 leaves the source rule.
    const double ratio = aspect_ ? aspect_() : 0.0;
    if (ratio > 4.0 / 3.0 + 1e-6) view.tv_aspect = float(ratio);
    // The public player copied these dimensions from this software decoder.
    if (view.movie_width != width_ || view.movie_height != height_) Fail(kDraw);
    ps2r::gs::DrawIr draw;
    if (!BuildDraw(view, draw)) Fail(kDraw);
    SelectView(draw);
    if (graphics_) {
        if (!hooks_.record(graphics_->End(kBucket), draw, view_image_)) Fail(kDraw);
    } else if (!output_.record(draw, sampled_)) {
        Fail(kDraw);
    }
    ++counts_.draws;
}

bool MovieGraphics::Present() {
    // PresentFromInterrupt precedes QueuePicture. The cleared scene is valid
    // during priming too; waiting for an uploaded image would skip that present.
    if (!Active() || !hooks_.present || !hooks_.present()) return false;
    ++counts_.presents;
    return true;
}

void MovieGraphics::Close() {
    if (has_image_ && graphics_) graphics_->ReleaseResource(image_);
    if (sampled_ && graphics_) graphics_->ReleaseResource(view_image_);
    image_ = {};
    view_image_ = {};
    displayed_.reset();
    sampled_.reset();
    has_image_ = false;
    if (Active()) ++counts_.closes;
    controller_ = width_ = height_ = latest_id_ = 0;
    pictures_.clear();
    // IDs never repeat across Close/reopen. A stale queue cannot name a new image.
}

bool MovieGraphics::CanCheckpoint(std::string* reason) const {
    if (!Active() && counts_.errors == 0) return true;
    if (reason) *reason = "Movie pictures are active or an output intent has failed";
    return false;
}

std::string MovieGraphics::Census() const {
    return "decoded=" + std::to_string(counts_.decoded) +
        ",upload_intents=" + std::to_string(counts_.uploaded) +
        ",view_intents=" + std::to_string(counts_.views) +
        ",draw_intents=" + std::to_string(counts_.draws) +
        ",presents=" + std::to_string(counts_.presents) +
        ",decode_original=" + std::to_string(counts_.original_calls[0]) +
        ",queue_original=" + std::to_string(counts_.original_calls[1]) +
        ",draw_original=" + std::to_string(counts_.original_calls[2]) +
        ",errors=" + std::to_string(counts_.errors);
}

}  // namespace title
