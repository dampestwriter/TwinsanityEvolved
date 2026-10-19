#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_pass_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"

#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace {
using F = ShadowPassFixture;
using P = F::Pass;
using S = F::Source;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
template<class Call> bool Fails(Call call) { try { call(); return false; } catch (...) { return true; } }
void SourceOrder() {
    F fixture;
    auto& pass = *fixture.pass;
    const auto original = fixture.ViewScreen();
    CHECK(pass.CanCheckpoint());
    fixture.Run(S::kBegin);
    CHECK(!pass.CanCheckpoint() && fixture.ViewScreen() != original);
    for (unsigned i = 0; i < 5; ++i) fixture.Mesh(0x1100, i);
    fixture.Mesh(0x1120, 5);
    fixture.Run(S::kEnd);
    CHECK(fixture.ViewScreen() == original);
    CHECK(!fixture.batches[0]->Ready());
    fixture.Run(S::kBegin);
    // The source's matrix arguments are stack locals on the scratchpad
    // (the s3 play route: every default mesh's a1/a2 were 0x7000xxxx).
    for (unsigned i = 6; i < 11; ++i) fixture.Mesh(0x1100, i, true);
    fixture.Mesh(0x1120, 11, true);
    fixture.Run(S::kEnd);
    CHECK(fixture.batches.size() == 2 && !fixture.batches[1]->Ready());
    fixture.Run(P::kClose);
    CHECK(fixture.batches[0]->Ready() && fixture.batches[1]->Ready());
    const auto& draws = fixture.batches[0]->Draws();
    CHECK(draws.size() == 12 && fixture.batches[1]->Draws().empty());
    // Source first insertion: A's first eight (eye-sized instances close the
    // block: 88 + 14 >= 100), B's two, A's next block.
    // Later Begin/End does NOT own the material writer already inserted before it.
    constexpr std::array<unsigned, 12> order{0, 1, 2, 3, 4, 6, 7, 8, 5, 11, 9, 10};
    for (size_t i = 0; i < order.size(); ++i) {
        CHECK(draws[i].verts.size() == 3);
        CHECK(draws[i].verts[0].x == float(128 + order[i]));
    }
    CHECK(pass.Counts().instances == 12 && pass.Counts().blocks == 3);
    CHECK(pass.Counts().empty == 1); // the second bracket owned no mesh of its own
    // The empty census: one frame closed with two brackets; the empty one is
    // `redirected` (its meshes were accepted, A's writer sits in the first
    // bracket); the frame published, so no empty-frame range.
    CHECK(pass.Counts().frames_closed == 1 && pass.Counts().frames_many_brackets == 1 &&
          pass.Counts().frames_one_bracket == 0);
    CHECK(pass.Counts().empty_redirected == 1 && pass.Counts().empty_no_mesh == 0);
    CHECK(pass.Counts().empty_frames_total == 0 && pass.Counts().empty_frames.empty() &&
          pass.Counts().last_published_frame == fixture.batches[0]->frame.source_frame);
    CHECK(pass.Counts().begin.replaced == 2 && pass.Counts().end.replaced == 2 &&
          pass.Counts().mesh.replaced == 12);
    CHECK(pass.Counts().begin.model_calls == 0 && pass.Counts().mesh.model_calls == 0 &&
          pass.Counts().end.model_calls == 0);
    CHECK(fixture.original_calls[S::kBegin] == 0 && fixture.original_calls[S::kMesh] == 0 &&
          fixture.original_calls[S::kEnd] == 0 && fixture.original_calls[P::kClose] == 1);
    // The title's SyncV boundary lies here: the pass is CLOSED (its batches
    // Ready behind their bucket-21 predecessor) and not yet Linked/forgotten.
    // A closed boundary refuses; the capture-time settle (an open recording
    // at the ordered output) admits exactly this shape -- never a Building one.
    CHECK(!pass.CanCheckpoint() && pass.CanCheckpoint(/*unlinked_recording_open=*/true));
    fixture.Run(P::kForget);
    CHECK(!pass.CanCheckpoint()); // queue is still holding two real unfinished passes
    CHECK(pass.CanCheckpoint(true)); // ... which the settle attributes to the open recording
    CHECK(fixture.batches[0]->Finish(true) && fixture.batches[1]->Finish(true));
    CHECK(pass.CanCheckpoint() && fixture.original_calls[P::kForget] == 1);
    title::ShadowAssets::Snapshot snapshot;
    CHECK(fixture.assets.Capture(snapshot) == title::ShadowAssets::Result::Ok);
    CHECK(snapshot.assets.size() == 1);
    fixture.Run(S::kBegin);
    CHECK(pass.Counts().completed == 2);
    fixture.Run(S::kEnd);
    fixture.Run(P::kClose);
    fixture.Run(P::kForget);
    CHECK(fixture.batches.back()->Ready() && fixture.batches.back()->Draws().empty());
    CHECK(fixture.batches.back()->Finish(true) && pass.CanCheckpoint());
    // A frame whose only bracket saw no mesh: `no_mesh`, and the frame joins
    // the empty-frame ranges (the s4 report's "what the empty passes are").
    CHECK(pass.Counts().empty == 2 && pass.Counts().empty_no_mesh == 1 && pass.Counts().empty_redirected == 1);
    CHECK(pass.Counts().frames_closed == 2 && pass.Counts().frames_one_bracket == 1 &&
          pass.Counts().empty_frames_total == 1 && pass.Counts().empty_frames.size() == 1 &&
          pass.Counts().empty_frames[0].first == fixture.batches.back()->frame.source_frame &&
          pass.Counts().empty_frames[0].last == fixture.batches.back()->frame.source_frame);
}
void Refusals() {
    F early;
    early.Run(S::kBegin);
    CHECK(!early.batches[0]->Finish(true)); // no positive-success empty submission
    early.Run(S::kEnd);
    CHECK(Fails([&] { early.Run(P::kClose); }));
    CHECK(early.batches[0]->Failed() && !early.pass->CanCheckpoint());
    F writer;
    const auto before = writer.ViewScreen();
    writer.reserve_ok = false;
    writer.Run(S::kBegin); // a declined reservation refuses the bracket to the model
    CHECK(writer.batches.empty() && writer.ViewScreen() == before);
    CHECK(writer.original_calls[S::kBegin] == 1 && !writer.pass->CanCheckpoint());
    CHECK(!writer.pass->CanCheckpoint(true)); // an OPEN bracket is never an unlinked group
    CHECK(writer.pass->Counts().refused == 1 && writer.pass->Counts().faults == 0);
    writer.Run(S::kEnd);
    CHECK(writer.original_calls[S::kEnd] == 1 && writer.ViewScreen() == before);
    writer.reserve_ok = true;
    writer.Run(S::kBegin);
    CHECK(writer.batches.size() == 1 && writer.original_calls[S::kBegin] == 1);
    writer.Run(S::kEnd); writer.Run(P::kClose);
    CHECK(writer.batches[0]->Ready());
    F partial;
    partial.write_ok = false;
    CHECK(Fails([&] { partial.Run(S::kBegin); }));
    CHECK(partial.batches.size() == 1 && !partial.batches[0]->Ready());
    CHECK(partial.ViewScreen() == before && partial.pass->Counts().faults == 1);
    CHECK(Fails([&] { partial.Run(S::kBegin); }) && partial.batches.size() == 1);
    F missing;
    missing.Run(S::kBegin);
    missing.assets.Forget(0x1000);
    // A default mesh without an owned asset is a REFUSAL of that one call (see
    // RefusedMesh), never a fault of the owned bracket and never the model body.
    missing.Mesh(0x1100, 0);
    CHECK(missing.pass->Counts().mesh.model_calls == 0 && missing.pass->Counts().faults == 0 &&
          missing.pass->Counts().refused_meshes == 1 && missing.original_calls[S::kMesh] == 0);
    F unclosed;
    unclosed.Run(S::kBegin);
    unclosed.Run(S::kEnd);
    CHECK(Fails([&] { unclosed.Run(P::kForget); }));
    CHECK(!unclosed.pass->CanCheckpoint() && unclosed.original_calls[P::kForget] == 0);
    F failed;
    failed.Run(S::kBegin); failed.Mesh(0x1100, 0); failed.Run(S::kEnd);
    failed.Run(P::kClose); failed.Run(P::kForget);
    CHECK(!failed.batches[0]->Finish(false));
    CHECK(!failed.pass->CanCheckpoint());
    CHECK(Fails([&] { failed.Run(S::kBegin); }));
    CHECK(failed.pass->Counts().completed == 0);
}
// A selected bracket whose source frame is not capturable (the frame owner has
// no completed scene yet, or the matrix check fails) REFUSES to the model for
// the whole bracket: begin/meshes/end run the originals and count model_calls,
// nothing is reserved or written, no fault is raised, and the next bracket
// with a capturable frame is owned again. This is the refuse-not-abort rule
// for the first live arm (CaptureScene(21) was never available before).
void RefusedBracket() {
    F producer;
    const auto before = producer.ViewScreen();
    producer.frame_ok = false;
    producer.Run(S::kBegin);
    CHECK(producer.batches.empty() && producer.ViewScreen() == before);
    CHECK(producer.original_calls[S::kBegin] == 1 && producer.pass->Counts().refused == 1);
    CHECK(producer.pass->Counts().begin.calls == 1 && producer.pass->Counts().begin.model_calls == 1 &&
          producer.pass->Counts().begin.replaced == 0 && producer.pass->Counts().faults == 0);
    CHECK(!producer.pass->CanCheckpoint()); // the model bracket is open
    producer.Mesh(0x1100, 0);
    producer.Mesh(0x1100, 1);
    CHECK(producer.original_calls[S::kMesh] == 2 && producer.pass->Counts().mesh.model_calls == 2 &&
          producer.pass->Counts().instances == 0 && producer.pass->Counts().blocks == 0);
    producer.Run(S::kEnd);
    CHECK(producer.original_calls[S::kEnd] == 1 && producer.pass->Counts().end.model_calls == 1);
    CHECK(producer.ViewScreen() == before);
    producer.Run(P::kClose);
    producer.Run(P::kForget);
    CHECK(producer.batches.empty() && producer.pass->CanCheckpoint());
    CHECK(producer.original_calls[P::kClose] == 1 && producer.original_calls[P::kForget] == 1);
    // The frame returns: the next bracket is owned, with no carry-over.
    producer.frame_ok = true;
    producer.Run(S::kBegin);
    producer.Mesh(0x1100, 0);
    producer.Run(S::kEnd);
    producer.Run(P::kClose);
    CHECK(producer.batches.size() == 1 && producer.batches[0]->Ready());
    CHECK(producer.pass->Counts().begin.replaced == 1 && producer.pass->Counts().mesh.replaced == 1 &&
          producer.pass->Counts().end.replaced == 1 && producer.pass->Counts().refused == 1);
    CHECK(producer.original_calls[S::kBegin] == 1 && producer.original_calls[S::kMesh] == 2);
    CHECK(producer.batches[0]->Finish(true));
    producer.Run(P::kForget);
    CHECK(producer.pass->Counts().completed == 1 && producer.pass->CanCheckpoint());
    // A refusal never applies to an interior continuation or a faulted pass;
    // and a bracket that is refused twice in a row stays attributable.
    F twice;
    twice.frame_ok = false;
    twice.Run(S::kBegin); twice.Run(S::kEnd);
    twice.Run(S::kBegin); twice.Run(S::kEnd);
    twice.Run(P::kClose); twice.Run(P::kForget);
    CHECK(twice.pass->Counts().refused == 2 && twice.pass->Counts().begin.model_calls == 2 &&
          twice.pass->Counts().faults == 0 && twice.pass->CanCheckpoint());
    // The model arm never counts a refusal: it owns nothing to refuse.
    F retained(false);
    retained.frame_ok = false;
    retained.Run(S::kBegin); retained.Run(S::kEnd); retained.Run(P::kClose); retained.Run(P::kForget);
    CHECK(retained.pass->Counts().refused == 0 && retained.pass->Counts().begin.model_calls == 1);
}
// Play-session s2 (all-on 966187d1, frame 5194): the first in-level default
// mesh had no owned asset for its model and the pass FAULTED the run
// ("Shadow default mesh has no checked owned input"). The s2 fix ran the model
// body for that one mesh; the s3 play session showed what that draws: the
// owned begin had bypassed StartShadows' half-size target set-up, so the
// model's volume strips (colour 8/248, additive) landed in the SCENE -- the
// tall white streak over Crash. The rule now: a declined mesh inside an owned
// bracket is the owner's call (mesh.replaced), drawn by NEITHER arm, attributed
// through Hooks::refuse (producer kMesh, detail = check | result << 8) and in
// refused_meshes/last_mesh_refusal; the owned bracket, its other meshes and its
// delivery are unchanged; the model body never runs inside an owned bracket.
void RefusedMesh() {
    using R = title::GraphicsRefusals::Reason;
    using Check = S::MeshCheck;
    F producer;
    producer.Run(S::kBegin);
    producer.Mesh(0x1100, 0); // owned
    // The second mesh object's model is not owned by this asset owner.
    producer.Store(0x1120 + 12, 0x1400); // mesh.model -> an unconverted RigidModelData
    producer.Store(0x1404, 0xd8d37a1c); producer.Store(0x1408, 1);
    producer.Mesh(0x1120, 1);
    CHECK(producer.pass->Counts().faults == 0 && producer.pass->Counts().refused == 0);
    CHECK(producer.pass->Counts().refused_meshes == 1);
    CHECK(producer.pass->Counts().mesh.calls == 2 && producer.pass->Counts().mesh.replaced == 2 &&
          producer.pass->Counts().mesh.model_calls == 0);
    CHECK(producer.original_calls[S::kMesh] == 0); // no model strip into the scene
    CHECK(producer.refusals.size() == 1 && producer.refusals[0].reason == R::MissingResource &&
          producer.refusals[0].producer == S::kMesh &&
          producer.refusals[0].detail == S::Detail(S::Result::UnsupportedSource, Check::Asset));
    const auto& last = producer.pass->Counts().last_mesh_refusal;
    CHECK(last.result == S::Result::UnsupportedSource && last.detail.check == Check::Asset);
    CHECK(last.detail.object == 0x1120 && last.detail.geometry == 0x1400 &&
          last.detail.model_id == 0xd8d37a1c && last.detail.asset_id == 0);
    // The bracket is still owned: a later checked mesh is recorded and the pass
    // closes and delivers the owned instances only.
    producer.Mesh(0x1100, 2);
    producer.Run(S::kEnd);
    producer.Run(P::kClose);
    producer.Run(P::kForget);
    CHECK(producer.batches.size() == 1 && producer.batches[0]->Ready());
    CHECK(producer.batches[0]->Draws().size() == 2 && producer.pass->Counts().instances == 2);
    CHECK(producer.pass->Counts().begin.replaced == 1 && producer.pass->Counts().end.replaced == 1);
    CHECK(producer.original_calls[S::kBegin] == 0 && producer.original_calls[S::kEnd] == 0);
    CHECK(producer.batches[0]->Finish(true) && producer.pass->CanCheckpoint());
    // Every declined check is a refusal with its own detail, not a fault:
    // a foreign shape (Shape: an open instance block; a billboard shader), an
    // identity disagreement (Identity), a forgotten asset (Asset), an unreadable
    // object (Object). None runs the model body.
    F declined;
    declined.Run(S::kBegin);
    declined.Store(0x1120 + 16, 0x5000); // instances.block open: a foreign writer
    declined.Mesh(0x1120, 0);
    declined.Store(0x1120 + 16, 0);
    declined.Store(0x1120 + 20, 0x101); // billboard: a turned instance matrix
    declined.Mesh(0x1120, 1);
    declined.Store(0x1120 + 20, 1);
    declined.Store(0x1120 + 28, 0x11111111); // mesh.modelId disagrees with the model
    declined.Mesh(0x1120, 2);
    declined.Store(0x1120 + 28, 0xd8d37a1c);
    declined.assets.Forget(0x1000);
    declined.Mesh(0x1120, 3);
    declined.Mesh(0x3ffffc, 4); // object words beyond the readable fixture
    CHECK(declined.pass->Counts().faults == 0 && declined.pass->Counts().refused_meshes == 5 &&
          declined.pass->Counts().mesh.model_calls == 0 && declined.original_calls[S::kMesh] == 0 &&
          declined.pass->Counts().mesh.replaced == 5);
    CHECK(declined.refusals.size() == 5);
    CHECK(declined.refusals[0].reason == R::UnsupportedState &&
          declined.refusals[0].detail == S::Detail(S::Result::UnsupportedSource, Check::Shape));
    CHECK(declined.refusals[1].reason == R::UnsupportedState &&
          declined.refusals[1].detail == S::Detail(S::Result::UnsupportedSource, Check::Shape));
    CHECK(declined.refusals[2].reason == R::UnsupportedState &&
          declined.refusals[2].detail == S::Detail(S::Result::UnsupportedSource, Check::Identity));
    CHECK(declined.refusals[3].reason == R::MissingResource &&
          declined.refusals[3].detail == S::Detail(S::Result::UnsupportedSource, Check::Asset));
    CHECK(declined.refusals[4].reason == R::UnsupportedState &&
          declined.refusals[4].detail == S::Detail(S::Result::InvalidMemory, Check::Object));
    declined.Run(S::kEnd); declined.Run(P::kClose); declined.Run(P::kForget);
    CHECK(declined.batches.size() == 1 && declined.batches[0]->Ready() && declined.batches[0]->Draws().empty());
    CHECK(declined.pass->Counts().empty == 1 && declined.pass->Counts().instances == 0);
    CHECK(declined.batches[0]->Finish(true) && declined.pass->CanCheckpoint());
    // A mesh outside any bracket is still the pass's fault (ordering), and
    // the model arm counts no refusal of its own.
    F outside;
    CHECK(Fails([&] { outside.Mesh(0x1100, 0); }));
    CHECK(outside.pass->Counts().faults == 1 && outside.pass->Counts().refused_meshes == 0);
    F retained(false);
    retained.assets.Forget(0x1000);
    retained.Run(S::kBegin); retained.Mesh(0x1100, 0); retained.Run(S::kEnd);
    CHECK(retained.pass->Counts().refused_meshes == 0 && retained.refusals.empty() &&
          retained.pass->Counts().mesh.model_calls == 1);
}
// Source SetPlacedModelRenderDMA closes a block when its size + PlacedRoom 14
// reaches the capacity 100: the eighth eye-sized instance (11 quadwords, every
// type-0B shader answers NeedsEye) or the ninth clipped one (10 quadwords); a
// shape whose eye answer changes inside an open block is a source fault.
void BlockCapacity() {
    F eye;
    eye.Run(S::kBegin);
    for (unsigned i = 0; i < 8; ++i) eye.Mesh(0x1100, i);
    CHECK(eye.pass->Counts().blocks == 1 && eye.pass->Counts().refused_meshes == 0);
    eye.Mesh(0x1100, 8);
    CHECK(eye.pass->Counts().blocks == 2 && eye.pass->Counts().instances == 9);
    F clipped;
    clipped.Store(0x1100 + 20, 0); // needsEye 0: PlacedClippedSize instances
    clipped.Run(S::kBegin);
    for (unsigned i = 0; i < 9; ++i) clipped.Mesh(0x1100, i);
    CHECK(clipped.pass->Counts().blocks == 1 && clipped.pass->Counts().refused_meshes == 0);
    clipped.Mesh(0x1100, 9);
    CHECK(clipped.pass->Counts().blocks == 2 && clipped.pass->Counts().instances == 10);
    F changed;
    changed.Run(S::kBegin);
    changed.Mesh(0x1100, 0);
    changed.Store(0x1100 + 20, 0);
    CHECK(Fails([&] { changed.Mesh(0x1100, 1); }) && changed.pass->Counts().faults == 1);
}
void Selection() {
    F retained(false);
    retained.Run(S::kBegin); retained.Mesh(0x1100, 0); retained.Run(S::kEnd);
    retained.Run(P::kClose); retained.Run(P::kForget);
    CHECK(retained.batches.empty() && retained.pass->CanCheckpoint());
    CHECK(retained.original_calls[S::kBegin] == 1 && retained.original_calls[S::kMesh] == 1 &&
          retained.original_calls[S::kEnd] == 1);
    CHECK(retained.pass->Counts().begin.model_calls == 1 && retained.pass->Counts().mesh.replaced == 0);
}
void SceneDelivery() {
    EffectsFixture world;
    F producer;
    producer.frame = world.frame;
    world.pipeline.SetGfxEpoch(9001);
    CHECK(world.pipeline.DrawGeneric(world.OrdinaryDraw()).drew);
    const auto attachments = world.backend.last_params;
    const auto* target = world.cache.Find(world.frame.color_buffer);
    CHECK(target);
    const auto depth_sequence = target->coherence.last_z_seq;
    const std::vector<uint8_t> memory(world.memory.Bytes(),
                                     world.memory.Bytes() + ps2r::gs::VramImage::kBytes);
    producer.Run(S::kBegin);
    producer.Mesh(0x1100, 0);
    producer.Mesh(0x1120, 1);
    producer.Run(S::kEnd);
    producer.Run(P::kClose);
    producer.Run(P::kForget);
    CHECK(producer.batches.size() == 1 && producer.batches[0]->Ready());
    const auto batch = producer.batches[0];
    CHECK(batch->Draws().size() == 2 && !producer.pass->CanCheckpoint());
    // Queued render work retains values and geometry, not mutable producer RAM.
    producer.Store(S::kView, 0);
    producer.Store(0x1100, 0);
    producer.frame.width = 0;
    CHECK(batch->frame.width == 512 && batch->frame.epoch == world.frame.epoch);
    CHECK(world.effects.Shadows(batch->frame, batch->Draws()) == title::EffectsScene::Result::Ok);
    CHECK(batch->Finish(true) && producer.pass->CanCheckpoint());
    CHECK(world.effects.Counts().shadow_passes == 1 && world.effects.ShadowCounts().strips == 2);
    CHECK(world.pipeline.DrawSeq() == 2 && target->coherence.main_author_seq == 2);
    CHECK(target->coherence.last_z_seq == depth_sequence);
    CHECK(world.pipeline.GfxEpoch() == 9001 && target->coherence.last_draw_gfx_epoch == 9001);
    CHECK(world.backend.last_params.color.id == attachments.color.id &&
          world.backend.last_params.depth.id == attachments.depth.id);
    CHECK(!world.backend.last_params.do_frame_clear && world.backend.last_params.scale == 2);
    CHECK(world.backend.readbacks == 0 && world.backend.uploads == 0);
    CHECK(std::memcmp(memory.data(), world.memory.Bytes(), memory.size()) == 0);
    CHECK(producer.pass->Counts().mesh.replaced == 2 &&
          producer.pass->Counts().mesh.model_calls == 0);
}
// allon_w2 (2026-10-11): with the owned sky's VU1 program residency missing,
// the ordered output stalled behind ~1 M raw PATH1 kicks and the pass FAULTED
// the run at its 64th undelivered reservation ("Shadow pass limit", frame
// ~5000, 3 GB resident). The rule now: a bracket past kPendingLimit undelivered
// reservations is REFUSED to the model (begin/meshes/end run the originals,
// nothing reserved or written), counted in refused + limit_refusals and
// attributed (kBegin, kLimitDetail, ordering); the next bracket after the
// output catches up is owned again. The stall is the output's to explain
// (graphics_owned_output pending), never a fault of this pass.
void PendingLimit() {
    using R = title::GraphicsRefusals::Reason;
    F stalled;
    const auto before = stalled.ViewScreen();
    for (size_t frame = 0; frame < P::kPendingLimit; ++frame) {
        stalled.Run(S::kBegin); stalled.Mesh(0x1100, 0); stalled.Run(S::kEnd);
        stalled.Run(P::kClose); stalled.Run(P::kForget);
    }
    CHECK(stalled.batches.size() == P::kPendingLimit && stalled.pass->Counts().refused == 0);
    for (const auto& batch : stalled.batches) CHECK(batch->Ready() && !batch->Complete());
    // The 65th bracket: refused, not faulted; the model runs the whole bracket.
    stalled.Run(S::kBegin);
    CHECK(stalled.batches.size() == P::kPendingLimit && stalled.ViewScreen() == before);
    CHECK(stalled.original_calls[S::kBegin] == 1 && stalled.pass->Counts().faults == 0);
    CHECK(stalled.pass->Counts().refused == 1 && stalled.pass->Counts().limit_refusals == 1);
    CHECK(stalled.refusals.size() == 1 && stalled.refusals[0].reason == R::Ordering &&
          stalled.refusals[0].producer == S::kBegin && stalled.refusals[0].detail == P::kLimitDetail);
    stalled.Mesh(0x1100, 0); stalled.Run(S::kEnd); stalled.Run(P::kClose); stalled.Run(P::kForget);
    CHECK(stalled.original_calls[S::kMesh] == 1 && stalled.original_calls[S::kEnd] == 1);
    CHECK(stalled.pass->Counts().begin.model_calls == 1 && stalled.pass->Counts().begin.replaced == P::kPendingLimit);
    // The output delivers: the pass collects and owns the next bracket again.
    for (const auto& batch : stalled.batches) CHECK(batch->Finish(true));
    stalled.Run(S::kBegin); stalled.Mesh(0x1100, 0); stalled.Run(S::kEnd); stalled.Run(P::kClose); stalled.Run(P::kForget);
    CHECK(stalled.batches.size() == P::kPendingLimit + 1 && stalled.pass->Counts().completed == P::kPendingLimit);
    CHECK(stalled.pass->Counts().begin.replaced == P::kPendingLimit + 1 && stalled.pass->Counts().refused == 1 &&
          stalled.pass->Counts().limit_refusals == 1 && stalled.pass->Counts().faults == 0);
    CHECK(stalled.original_calls[S::kBegin] == 1);
}
}
int main() {
    try {
        SourceOrder(); Refusals(); BlockCapacity(); Selection(); RefusedBracket(); RefusedMesh(); SceneDelivery();
        PendingLimit();
        std::printf("ShadowPass: %u source grouping, selection, publication and queue pins\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
