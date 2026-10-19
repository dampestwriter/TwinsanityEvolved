#include <cstdio>
#include <string>

#include "app/host/live_restore_gate.h"
#include "games/slus_20909_crash_twinsanity/composition/startup_ownership.h"
#include "engine/core/config/config.h"

// LiveRestoreGate (F8 live restore): the route under the all-on
// compiled defaults is LIVE; the gate admits a clean composition and refuses
// each asynchronous owner BY NAME with its count; the relaunch stays reachable
// by setting and as the named fallback when startup is retained.
int main() {
    using ps2r::app::LiveRestoreGate;
    using ps2r::app::StartupOwnership;
    using Route = LiveRestoreGate::Route;
    using S = title::ServiceStartup;
    int checks = 0, failures = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL LiveRestoreGate %s\n", label); }
    };
    const auto has = [](const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; };

    // 1. The route under the all-on compiled defaults: Config{} is live, the
    //    service-only startup owns the composition -> F8 restores in-process.
    const ps2r::Config defaults;
    check(defaults.save.restore_mode == ps2r::RestoreMode::Live, "compiled default is save.restore_mode=live");
    check(defaults.hle.startup == ps2r::StartupMode::Native, "compiled default is hle.startup=native");
    S::Inputs all;
    all.stream_files = all.sound_banks = all.sound_controls = all.music = all.music_buffers = true;
    all.disc_files = all.output = all.movie_pcm = all.movie_cursor = true;
    all.transport = all.effects = all.bank_residency = true;
    const StartupOwnership owned(S::Inspect(all));
    const StartupOwnership retained(S::Inspect(S::Inputs{}));
    check(owned.Owned() && !retained.Owned(), "fixture: one owned and one retained composition");
    const Route live = LiveRestoreGate::Choose(defaults.save.restore_mode, defaults.harness.live_restore, owned.Owned(), false);
    check(live == Route::Live && LiveRestoreGate::Live(live), "all-on defaults + owned startup -> live");
    check(std::string(LiveRestoreGate::RouteCensus(live)) == "live", "census f8=live");
    // 2. The fallback: a composition that retains IOP state relaunches, named.
    const Route fallback = LiveRestoreGate::Choose(ps2r::RestoreMode::Live, false, retained.Owned(), false);
    check(fallback == Route::RelaunchFallback && !LiveRestoreGate::Live(fallback), "retained startup -> relaunch fallback");
    check(std::string(LiveRestoreGate::RouteCensus(fallback)) == "relaunch,fallback=startup_retained", "census names the fallback");
    check(LiveRestoreGate::Choose(ps2r::RestoreMode::Live, false, true, true) == Route::RelaunchFallback,
          "the IOP having booted (irx entry ran) -> relaunch fallback");
    // 3. By setting: relaunch, unless the root opt-in forces live.
    check(LiveRestoreGate::Choose(ps2r::RestoreMode::Relaunch, false, true, false) == Route::RelaunchBySetting,
          "save.restore_mode=relaunch -> relaunch by setting");
    check(std::string(LiveRestoreGate::RouteCensus(Route::RelaunchBySetting)) == "relaunch,setting", "census names the setting");
    check(LiveRestoreGate::Choose(ps2r::RestoreMode::Relaunch, true, true, false) == Route::Live,
          "harness.live_restore opt-in overrides the setting");

    // 4. The gate: a clean composition admits; each term refuses by name.
    LiveRestoreGate::Probes probes;
    bool mtvu_ok = true, events = false;
    size_t rpc = 0, deliveries = 0;
    bool owners_ok = true;
    probes.mtvu_quiesce = [&](std::string* why) { if (!mtvu_ok && why) *why = "MTVU pipeline did not drain within the quiesce budget"; return mtvu_ok; };
    probes.events_pending = [&] { return events; };
    probes.rpc_pending = [&] { return rpc; };
    probes.deliveries_pending = [&] { return deliveries; };
    probes.owners = [&](std::string& error) { if (!owners_ok) error = "game_save: Save storage has a transfer in flight"; return owners_ok; };
    LiveRestoreGate::Census census;
    std::string error;
    check(LiveRestoreGate::Inspect(probes, census, error) && error.empty(), "clean composition admits");
    check(census.Render() == "mtvu=0,events=0,rpc=0,deliveries=0,owners=0", "nothing counted on admission");

    mtvu_ok = false;
    check(!LiveRestoreGate::Inspect(probes, census, error), "mtvu refuses");
    check(has(error, "live restore refused: mtvu worker holds state: MTVU pipeline did not drain"), "mtvu refusal names the worker and its reason");
    check(census.mtvu == 1, "mtvu counted");
    mtvu_ok = true;

    events = true;
    check(!LiveRestoreGate::Inspect(probes, census, error) && error == "live restore refused: ee events pending", "events refuse by name");
    events = false;

    rpc = 3;
    check(!LiveRestoreGate::Inspect(probes, census, error) && error == "live restore refused: rpc pending=3", "rpc refuses with its count");
    rpc = 0;

    deliveries = 2;
    check(!LiveRestoreGate::Inspect(probes, census, error) && error == "live restore refused: disc deliveries pending=2",
          "deliveries refuse with their count");
    deliveries = 0;

    owners_ok = false;
    check(!LiveRestoreGate::Inspect(probes, census, error) && has(error, "Save storage has a transfer in flight"),
          "an owner's predicate refuses with the owner's own text");
    owners_ok = true;
    check(census.Render() == "mtvu=1,events=1,rpc=1,deliveries=1,owners=1", "every refusal counted once");

    // 5. The order is the owners' order: the first refusing term names the refusal.
    mtvu_ok = false; rpc = 7;
    check(!LiveRestoreGate::Inspect(probes, census, error) && has(error, "mtvu worker"), "the first term in order names the refusal");
    mtvu_ok = true; rpc = 0;
    // 6. No MTVU worker in the composition (the deterministic profile): the term is absent.
    probes.mtvu_quiesce = nullptr;
    check(LiveRestoreGate::Inspect(probes, census, error), "no worker -> no mtvu term");

    std::printf("LiveRestoreGate: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
