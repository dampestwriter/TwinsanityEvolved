#include "games/slus_20909_crash_twinsanity/composition/graphics_producer_draws.h"

#include <map>
#include <stdexcept>
#include <utility>

namespace ps2r::app {
namespace {
using Graphics = ::title::Graphics;
using Resources = ::title::GraphicsResources;
using Renderer = ::title::GraphicsRenderer;
using Scene = ::title::GraphicsScene;
using Targets = ::title::GraphicsTargets;
using Textures = ::title::TextureAssets;
// The owned scene's draw result at the ordered slot: Ok drew; every other
// pre-draw result (unclosed dependency, stale/missing scene, resource failure)
// is a REFUSAL of the slot (false: nothing published, the group's slot is
// released); DrawFailure means the renderer ran and failed -- a fault, thrown.
bool Drawn(Scene::Result result) {
    if (result == Scene::Result::DrawFailure)
        throw std::runtime_error("Graphics producer draw failed at its ordered slot");
    return result == Scene::Result::Ok;
}
bool SameOwner(const Graphics::Lease& a, const Graphics::Lease& b) {
    return a == b && a && !a.owner_before(b) && !b.owner_before(a);
}
bool Bind(Scene& scene, Graphics& queue, Targets& targets,
          const Scene::Frame& frame, std::vector<Graphics::Lease>& out, bool sky = false,
          bool preserve = false) {
    Scene::Binding binding;
    const auto result = sky ? scene.BindSky(queue, targets, frame, binding, preserve)
                            : scene.Bind(queue, targets, frame, binding);
    if (result != Scene::Result::Ok) return false;
    for (const auto& handle : binding.handles) {
        Graphics::Lease lease;
        if (queue.RetainResource(handle, &lease) != Graphics::Result::Ok) return false;
        out.push_back(std::move(lease));
    }
    return true;
}
bool Picture(const ::title::MovieGraphics::ImageLease& image) {
    if (!image || !image->id || image->pixels.width <= 0 || image->pixels.height <= 0 ||
        image->pixels.width > 4096 || image->pixels.height > 4096 ||
        image->pixels.width % 16 || image->pixels.height % 16) return false;
    return image->pixels.rgba.size() == size_t(image->pixels.width) * image->pixels.height * 4;
}
}  // namespace

struct GraphicsProducerDraws::Impl {
    struct Image {
        std::weak_ptr<const void> source;
        Graphics::Handle handle;
        Graphics::Lease lease;
    };
    Resources& resources;
    Renderer& renderer;
    Scene& scene;
    Textures& assets;
    bool synthesize;
    Graphics images;
    std::map<const void*, Image> pictures, skies;
    std::map<const Graphics::Resource*, std::weak_ptr<const Graphics::Resource>> uploaded;

    Impl(Resources& r, Renderer& draw, Scene& target, Textures& textures, bool tail)
        : resources(r), renderer(draw), scene(target), assets(textures), synthesize(tail) {}
    void Collect() {
        for (auto* cache : {&pictures, &skies}) {
            for (auto it = cache->begin(); it != cache->end();) {
                if (it->second.source.expired()) {
                    images.ReleaseResource(it->second.handle);
                    it = cache->erase(it);
                } else ++it;
            }
        }
        for (auto it = uploaded.begin(); it != uploaded.end();) {
            if (it->second.expired()) it = uploaded.erase(it);
            else ++it;
        }
    }
    Graphics::Lease ImageFor(std::map<const void*, Image>& cache,
        const std::shared_ptr<const void>& source, uint32_t width, uint32_t height,
        const std::vector<uint8_t>& pixels) {
        Collect();
        if (const auto found = cache.find(source.get()); found != cache.end()) {
            const auto held = found->second.source.lock();
            if (!held || held.owner_before(source) || source.owner_before(held)) return {};
            return found->second.lease;
        }
        if (!source || !width || !height || width > 4096 || height > 4096 ||
            pixels.size() != uint64_t(width) * height * 4 || cache.size() >= 256) return {};
        Image image;
        image.source = source;
        if (resources.Register(images, {width, height, render::SurfaceFormat::Rgba8, false, 1},
                               &image.handle) != Resources::Result::Ok) return {};
        if (images.RetainResource(image.handle, &image.lease) != Graphics::Result::Ok) {
            images.ReleaseResource(image.handle);
            return {};
        }
        Graphics::Upload upload{image.lease, {0, 0, int32_t(width), int32_t(height)}, size_t(width) * 4, pixels, 0};
        if (resources.Upload(upload) != Resources::Result::Ok) {
            images.ReleaseResource(image.handle);
            return {};
        }
        const auto lease = image.lease;
        cache.emplace(source.get(), std::move(image));
        return lease;
    }
    // 0 = uploaded (or already resident); otherwise the refusal: 1 = the view
    // carries no lease/image, 2 = the asset owner does not know the lease
    // (TextureAssets::Find result + 0x10), 3 = the view and the owner's version
    // disagree, 4 + TextureAssets::Result = the upload itself refused.
    uint32_t UploadAsset(const Textures::View& view, uint32_t frame) {
        Textures::View owned;
        if (!view.lease || !view.image || view.image->levels.empty()) return 1;
        const auto found = assets.Find(view.lease, owned);
        if (found != Textures::Result::Ok) return 0x10 | uint32_t(found);
        if (!SameOwner(view.lease, owned.lease) || view.image != owned.image ||
            view.token.object != owned.token.object || view.token.generation != owned.token.generation)
            return 3;
        const auto at = uploaded.find(view.lease.get());
        if (at != uploaded.end() && SameOwner(at->second.lock(), view.lease)) return 0;
        const auto& base = view.image->Pixels()[0];   // the pack's pixels when a pack replaced it
        const Graphics::Upload upload{view.lease, {0, 0, int32_t(base.width), int32_t(base.height)},
                                      size_t(base.width) * 4, base.rgba, frame};
        const auto result = assets.Upload(upload, synthesize);
        if (result != Textures::Result::Ok) return 0x20 | uint32_t(result);
        uploaded[view.lease.get()] = view.lease;
        return 0;
    }
};

GraphicsProducerDraws::GraphicsProducerDraws(Resources& resources, Renderer& renderer,
    Scene& scene, Textures& assets, bool tail)
    : impl_(new Impl(resources, renderer, scene, assets, tail)) {}
GraphicsProducerDraws::~GraphicsProducerDraws() = default;

bool GraphicsProducerDraws::Assets(const Frame& frame, const std::vector<AssetDraw>& draws, uint32_t* why) {
    auto& p = *impl_;
    const auto refuse = [why](uint32_t code) {
        if (why) *why = code;
        return false;
    };
    return p.renderer.WithTextureLevels([&p](const Graphics::Lease& lease) {
        Textures::View view;
        return p.assets.Find(lease, view) == Textures::Result::Ok
            ? uint8_t(view.image->levels.size()) : uint8_t(0);
    }, [&] {
    p.Collect();
    Graphics queue;
    Targets targets(queue);
    std::vector<Graphics::Lease> bindings;
    if (!Bind(p.scene, queue, targets, frame, bindings)) return refuse(kBindRefused);
    for (const auto& item : draws) {
        // An untextured draw (the flat Sprite: one type-0E shader, textured bit
        // clear) carries no asset view: nothing to upload or bind.
        if (item.draw.tex_enabled)
            if (const auto upload = p.UploadAsset(item.texture, frame.source_frame))
                return refuse(kUploadRefused | upload << 8);
        auto draw = item.draw;
        draw.src_frame = frame.source_frame;
        bindings.resize(3);
        if (draw.tex_enabled) bindings.push_back(item.texture.lease);
        const auto result = p.scene.Draw(draw, bindings);
        if (!Drawn(result)) return refuse(kDrawRefused | uint32_t(result));
    }
    return true;
    });
}

bool GraphicsProducerDraws::UploadMovie(const ::title::MovieGraphics::ImageLease& image) {
    if (!Picture(image)) return false;
    auto& p = *impl_;
    return bool(p.ImageFor(p.pictures, image, uint32_t(image->pixels.width),
                           uint32_t(image->pixels.height), image->pixels.rgba));
}

bool GraphicsProducerDraws::Movie(const Frame& frame, const gs::DrawIr& source,
    const ::title::MovieGraphics::ImageLease& image) {
    if (!Picture(image) || !image->source_id || !image->sample_width || !image->sample_height ||
        image->sample_width >= uint32_t(image->pixels.width) ||
        image->sample_height >= uint32_t(image->pixels.height)) return false;
    auto& p = *impl_;
    Graphics queue;
    Targets targets(queue);
    std::vector<Graphics::Lease> bindings;
    if (!Bind(p.scene, queue, targets, frame, bindings)) return false;
    auto lease = p.ImageFor(p.pictures, image, uint32_t(image->pixels.width),
                           uint32_t(image->pixels.height), image->pixels.rgba);
    if (!lease) return false;
    bindings.push_back(std::move(lease));
    auto draw = source;
    draw.src_frame = frame.source_frame;
    return Drawn(p.scene.Draw(draw, bindings));
}

bool GraphicsProducerDraws::Sky(const Frame& frame, const ::title::SkyPass::Output& output) {
    auto& p = *impl_;
    if (frame.width != output.setup.width || frame.height != output.setup.height ||
        output.finish.width != frame.width || output.finish.height != frame.height) return false;
    Graphics queue;
    Targets targets(queue);
    std::vector<Graphics::Lease> scene, sky;
    // Source StartSkyBuffer does not clear. An empty pass must sample prior
    // authored content; a new zero-filled allocation would invent an image.
    // Check that dependency before allocating the destination scene as well.
    if (!Bind(p.scene, queue, targets, frame, sky, true, output.draws.empty()) ||
        !Bind(p.scene, queue, targets, frame, scene)) return false;
    for (const auto& item : output.draws) {
        if (!item.texture) return false;
        auto texture = p.ImageFor(p.skies, item.texture, item.texture->width,
                                  item.texture->height, item.texture->rgba);
        if (!texture) return false;
        auto bindings = sky;
        bindings.push_back(std::move(texture));
        auto draw = item.intent;
        draw.src_frame = frame.source_frame;
        if (!Drawn(p.scene.Draw(draw, bindings))) return false;
    }
    scene.push_back(sky[0]);
    auto finish = output.finish.draw;
    finish.src_frame = frame.source_frame;
    return Drawn(p.scene.Draw(finish, scene));
}

void GraphicsProducerDraws::ResetAfterQuiescence() {
    auto& p = *impl_;
    p.pictures.clear();
    p.skies.clear();
    p.uploaded.clear();
    p.images.Reset();
}

}  // namespace ps2r::app
