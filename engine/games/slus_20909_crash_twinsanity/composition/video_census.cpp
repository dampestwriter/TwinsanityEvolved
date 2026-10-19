#include "games/slus_20909_crash_twinsanity/composition/video_census.h"

#include <cstdio>

#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/verify/run_meta.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/hle/queue/draw_ir_thread_queue.h"

namespace ps2r::app {

void VideoCensus::Record(verify::RunMeta& meta, VideoStack& video,
                         gs::DrawIrThreadQueue* queue, gs::DrawIrQueueConsumer* consumer,
                         const std::string& output_directory) {
    const bool video_live = video.Active();
    meta.Set("gfx_vram_epoch", video.Vram() ? std::to_string(video.Vram()->UploadEpoch()) : "none");
    meta.Set("video", video_live ? "gl" : "stub");
    meta.Set("video_scale", std::to_string(video.Scale()));
    if (video_live) meta.Set("video_texture_filter", video.TextureFilterCensus());
    if (queue) {
        const ps2r::gs::DrawIrThreadQueue::Counters q = queue->Count();
        meta.Set("gs_queue", std::string("mode=thread,policy=") +
                                  (queue->policy() == ps2r::gs::DrawIrThreadQueue::Policy::Lockstep ? "lockstep" : "pipelined") +
                                  ",enqueued=" + std::to_string(q.enqueued) + ",consumed=" + std::to_string(q.consumed) +
                                  ",frames=" + std::to_string(q.frames) + ",syncs=" + std::to_string(q.syncs) +
                                  ",frame_waits=" + std::to_string(q.frame_waits) + ",max_depth=" + std::to_string(q.max_depth) +
                                  ",frame_skip=" + (queue->FrameSkip() ? "1" : "0") +
                                  ",ordered_waits=" + std::to_string(q.ordered_waits) + ",ordered_wait_ms=" + std::to_string(q.ordered_wait_us / 1000) +
                                  ",sync_wait_ms=" + std::to_string(q.sync_wait_us / 1000) +
                                  ",payload_bytes=" + std::to_string(q.payload_bytes) +
                                  ",vram_ops=" + std::to_string(q.vram_ops) + ",vram_op_bytes=" + std::to_string(q.vram_op_bytes) +
                                  ",split_image=" + (queue->MirrorTarget() ? "1" : "0") +
                                  ",deferred_resizes=" + std::to_string(video.DeferredResizes()));
        meta.Set("frames_dropped", std::to_string(q.frames_dropped));
        // The play-mode local->local source (ServeLocalToLocalSource): served from the
        // EE image / from the completed-frame snapshot (with the staleness in frames),
        // unserved (the arming call), and the snapshots the consumer published.
        meta.Set("gs_queue_l2l", "ee_served=" + std::to_string(q.l2l_ee_served) +
                                     ",snapshot_served=" + std::to_string(q.l2l_snapshot_served) +
                                     ",stale_frames=" + std::to_string(q.l2l_stale_frames) +
                                     ",stale_max=" + std::to_string(q.l2l_stale_max) +
                                     ",unserved=" + std::to_string(q.l2l_unserved) +
                                     ",snapshots=" + std::to_string(q.snapshots));
    } else {
        meta.Set("frames_dropped", "0");
        meta.Set("gs_queue", video_live ? "mode=inline" : "mode=none");
    }
    if (consumer) {
        const ps2r::gs::DrawIrQueueConsumer::Counters& c = consumer->Count();
        meta.Set("presents", std::to_string(c.presents));
        meta.Set("grab_empty", std::to_string(c.grab_empty));
        meta.Set("grab_forgotten", std::to_string(c.forgotten_frames));
        meta.Set("gfx_consumer", "draws=" + std::to_string(c.draws) + ",jobs=" + std::to_string(c.jobs) +
                                      ",frames=" + std::to_string(c.frames) + ",uploads=" + std::to_string(c.uploads) +
                                      ",uploads_dropped=" + std::to_string(c.uploads_dropped) +
                                      ",resizes=" + std::to_string(c.resizes) +
                                      ",rebinds=" + std::to_string(video.ContextRebinds()) +
                                      ",warmed=" + std::to_string(video.WarmedPrograms()));
        // The texture resolver's decode-fallback census: lookups, where they
        // resolved, and the VRAM bytes copied before the lookup (the key's
        // inputs) versus for a decode (the composed view, miss path only).
        if (video.Pipe()) {
            const ps2r::gs::TextureResolver::Census& t = video.Pipe()->Resolver().Stats();
            meta.Set("texres", "lookups=" + std::to_string(t.lookups) + ",l1_hits=" + std::to_string(t.l1_hits) +
                                    ",l2_hits=" + std::to_string(t.l2_hits) + ",decodes=" + std::to_string(t.decodes) +
                                    ",prep_bytes=" + std::to_string(t.prep_bytes) +
                                    ",view_bytes=" + std::to_string(t.view_bytes) +
                                    ",sig_hits=" + std::to_string(t.sig_hits) +
                                    ",owned_serves=" + std::to_string(t.owned_serves) +
                                    ",owned_misses=" + std::to_string(t.owned_misses));
            // Textures at load: the slot binds the consumer received, the reads
            // they served, the misses (a read over a bound slot the bind does not
            // describe -- the tripwire), how binds retired, the surface cache.
            const ps2r::gs::OwnedTextureSlots::Census& o = video.Pipe()->Resolver().Owned().Stats();
            meta.Set("texres_owned", "binds=" + std::to_string(o.binds) + ",rebinds=" + std::to_string(o.rebinds) +
                                          ",serves=" + std::to_string(o.serves) + ",misses=" + std::to_string(o.misses) +
                                          ",miss_tuple=" + std::to_string(o.miss_tuple) +
                                          ",miss_clut=" + std::to_string(o.miss_clut) +
                                          ",miss_mips=" + std::to_string(o.miss_mips) +
                                          ",evicted_draw=" + std::to_string(o.evicted_draw) +
                                          ",evicted_cpu=" + std::to_string(o.evicted_cpu) +
                                          ",released=" + std::to_string(o.released) +
                                          ",surfaces_built=" + std::to_string(o.surfaces_built) +
                                          ",surface_uploads=" + std::to_string(o.surface_uploads) +
                                          ",surfaces_evicted=" + std::to_string(o.surfaces_evicted) +
                                          ",surfaces=" + std::to_string(o.surfaces) +
                                          ",surface_bytes=" + std::to_string(o.surface_bytes));
        }
        // Draw-route census: one line per draw of the last completed frame --
        // texture owner (decoded / surface / none), blend arm (ff / ff-rt / sw),
        // destination-copy state -- as <out>/draw_routes.txt, next to the frame
        // image the frames lane hashed (DrawRouteTrace; diagnostics only).
        if (video.Pipe() && !output_directory.empty()) {
            const std::string rp = output_directory + "/draw_routes.txt";
            if (std::FILE* rf = std::fopen(rp.c_str(), "w")) {
                video.Pipe()->RouteTrace().Write(rf);
                std::fclose(rf);
                meta.Set("draw_routes", rp + ",lines=" + std::to_string(video.Pipe()->RouteTrace().LastFrame().size()));
            }
        }
        // Class census: how many draws the class registry routed into each
        // class (ClassClassifier::ClassName order). A stamped draw whose
        // handler name did not resolve lands in hle-other.
        if (const ps2r::gs::ClassRegistry* reg = video.Pipe() ? video.Pipe()->Registry() : nullptr) {
            std::string classes;
            for (uint8_t cls = 0; cls < ps2r::gs::ClassClassifier::kClassCount; ++cls) {
                if (!classes.empty()) classes += ",";
                classes += std::string(ps2r::gs::ClassClassifier::ClassName(cls)) + "=" +
                           std::to_string(reg->Count().seen[cls]);
            }
            meta.Set("classes", classes);
        }
    } else {
        meta.Set("presents", "0");
        meta.Set("grab_empty", "0");
    }
}

}  // namespace ps2r::app
