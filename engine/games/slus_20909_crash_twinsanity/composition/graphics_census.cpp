#include "games/slus_20909_crash_twinsanity/composition/graphics_census.h"

namespace ps2r::app {
namespace {
template<class T> void Producer(T& current, const T& old) {
    current.calls += old.calls; current.replaced += old.replaced;
    current.model_calls += old.model_calls; current.failed += old.failed;
    current.refused += old.refused;
}
void Sky(::title::SkyPass::Counts& current, const ::title::SkyPass::Counts& old) {
    current.calls += old.calls; current.replaced += old.replaced;
    current.model_calls += old.model_calls; current.interior_calls += old.interior_calls;
}
void Mesh(::title::MeshPass::Count& current, const ::title::MeshPass::Count& old) {
    current.calls += old.calls; current.replaced += old.replaced;
    current.model_calls += old.model_calls; current.interior += old.interior;
}
void Meshes(MeshProducer::Counters& current, const MeshProducer::Counters& old) {
    Mesh(current.pass.rigid, old.pass.rigid); Mesh(current.pass.placed, old.pass.placed);
    Mesh(current.pass.direct, old.pass.direct); Mesh(current.pass.flush, old.pass.flush);
    current.pass.blocks += old.pass.blocks; current.pass.instances += old.pass.instances; current.pass.faults += old.pass.faults;
    current.model_reads += old.model_reads; current.models_loaded += old.models_loaded;
    current.models_invalid += old.models_invalid; current.models_unsupported += old.models_unsupported;
    current.models_limit += old.models_limit; current.model_destroys += old.model_destroys;
    current.lazy_loads += old.lazy_loads; current.lazy_misses += old.lazy_misses;
    current.material_reads += old.material_reads; current.receipts += old.receipts; current.unhashed += old.unhashed;
    current.owns_calls += old.owns_calls; current.certified += old.certified; current.no_receipt += old.no_receipt;
    current.unknown_material += old.unknown_material; current.unknown_record += old.unknown_record;
    current.unowned_consumer += old.unowned_consumer; current.unverified_archive += old.unverified_archive;
    current.texture_disagrees += old.texture_disagrees; current.bucket_declined += old.bucket_declined;
    current.receipt_stale += old.receipt_stale;
    current.texture_lookups += old.texture_lookups; current.texture_missing += old.texture_missing;
    current.texture_mismatch += old.texture_mismatch; current.requests += old.requests; current.reserved += old.reserved;
    current.refused_environment += old.refused_environment; current.refused_view += old.refused_view;
    current.refused_slot += old.refused_slot; current.delivered_draws += old.delivered_draws;
    current.pass.frames_admitted += old.pass.frames_admitted; current.pass.frames_whole_model += old.pass.frames_whole_model;
    current.receipts_restored += old.receipts_restored; current.receipts_dropped += old.receipts_dropped;
    current.frames_admitted += old.frames_admitted; current.frames_whole_model += old.frames_whole_model;
    current.table_unreadable += old.table_unreadable; current.mixed_frames += old.mixed_frames;
    // receipts_live / models_live / unreceipted_live describe only the live owner.
}
}

GraphicsProducers::Counters GraphicsCensus::Producers(GraphicsProducers::Counters current) const {
    Producer(current.sprites, producers_.sprites);
    current.sprites.empty += producers_.sprites.empty;
    Producer(current.text, producers_.text);
    current.text.empty_queues += producers_.text.empty_queues;
    Sky(current.sky.start, producers_.sky.start);
    Sky(current.sky.model, producers_.sky.model);
    Sky(current.sky.finish, producers_.sky.finish);
    current.sky.models_loaded += producers_.sky.models_loaded;
    current.sky.textures_loaded += producers_.sky.textures_loaded;
    current.sky.load_declines += producers_.sky.load_declines;
    current.sky.declines_memory += producers_.sky.declines_memory;
    current.sky.declines_asset += producers_.sky.declines_asset;
    current.sky.declines_limit += producers_.sky.declines_limit;
    current.sky.declines_converted += producers_.sky.declines_converted;
    current.sky.textures_converted += producers_.sky.textures_converted;
    current.sky.faults += producers_.sky.faults;
    current.sky.refused += producers_.sky.refused;
    current.sky.programs_uploaded += producers_.sky.programs_uploaded;
    Meshes(current.meshes, producers_.meshes);
    current.flush_calls += producers_.flush_calls;
    current.flush_replaced += producers_.flush_replaced;
    current.flush_model_calls += producers_.flush_model_calls;
    current.recorded += producers_.recorded;
    current.completed += producers_.completed;
    current.failures += producers_.failures;
    current.cancelled += producers_.cancelled;
    current.refused_recordings += producers_.refused_recordings;
    // pending and text_materials describe only the live owner.
    return current;
}
::title::TextureBindings::Counters GraphicsCensus::Bindings(::title::TextureBindings::Counters current) const {
    current.calls += bindings_.calls;
    current.loaded += bindings_.loaded;
    current.destroyed += bindings_.destroyed;
    current.declined += bindings_.declined;
    current.failed += bindings_.failed;
    current.refused += bindings_.refused;
    return current;
}
::title::TextureAssets::Counters GraphicsCensus::Assets(::title::TextureAssets::Counters current) const {
    current.loaded += assets_.loaded;
    current.released += assets_.released;
    current.uploaded += assets_.uploaded;
    current.failed += assets_.failed;
    // retained_bytes/current/versions are live allocation gauges.
    return current;
}
void GraphicsCensus::Retire(const GraphicsProducers::Counters& p, const ::title::TextureBindings::Counters& b,
                            const ::title::TextureAssets::Counters& a) {
    producers_ = Producers(p);
    bindings_ = Bindings(b);
    assets_ = Assets(a);
}

}  // namespace ps2r::app
