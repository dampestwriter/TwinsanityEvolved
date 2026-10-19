// te_mods.h -- the port's built-in MOD framework. A mod is a C object registered at start-up (compiled in; its
// manifest `mods/<id>/mod.json` -- id, name, version, hooks, settings -- embedded by CMake and checked against the
// registration), switched on and off from the title's own Options > MODS page (the seam: Platform::System::Mod*), all OFF
// by default, each switch persisted as `mods.<id>.enabled` in ps2r.ini. While ANY mod is on, the memory card the game
// writes is the mod-namespaced card (<mc_dir>-mods, seeded from the vanilla card the first time) -- a modded save never
// touches the vanilla card (te::Saves).
//
// Hook points (the C hook API; a Lua runtime is deferred): every frame boundary (the game controller's state, the
// player's chunk, paused), a level entered / left (the player's chunk path changed: the chunk name the trace logs), the
// menu built (the Options page asks for the rows), a card save / load (te::Saves' Write / Read). A mod may also offer a
// PAGE (Mods > <title>): rows the player chooses, answered with a ModAction the game performs (a warp). A mod without a
// switch (Mod::has_switch false) IS its page: one row on the Mods page opens it and counts the mod as on for the session.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace te::mods {

enum class Hook : uint32_t { Frame = 0, LevelEnter, LevelExit, MenuBuild, Save, Load, Count };
const char* HookName(Hook hook);

// What the frame hook sees (read from the game at the boundary; see mods.cpp)
struct FrameContext {
    uint64_t boundary = 0;
    unsigned state = 0;          // GameController::State (12 = Playing, 15 = Paused, 9 = LoadingLevel ...)
    bool paused = false;         // the pause menu / a notice is up (state Paused)
    bool playing = false;        // state Playing or Watching (the in-game clock runs)
    std::string chunk;           // the player's chunk path ("levels\\earth\\hub\\huba"; "" when there is no player)
};

// The actions the game performs for a chosen page row (include/platform/system.h ModAction)
enum Action : uint32_t { ActionNone = 0, ActionWarp = 1, ActionTitle = 2, ActionCredits = 3 };
struct WarpTarget { std::string chunk; uint32_t character = 6; };   // GameProgress::NoCharacter = 6

struct Mod {
    std::string id;        // [a-z0-9_]+ -- the ps2r.ini key mods.<id>.enabled and the manifest folder mods/<id>/
    std::string name;      // the Mods page row (lower case like the game's menu texts)
    std::string version;
    // Whether the Mods page shows an off / on switch for it (persisted). A mod WITHOUT one is always available: its row on
    // the Mods page opens its page at once and that turns it on for the SESSION (never persisted: the next boot starts
    // vanilla); ps2r.ini's mods.<id>.enabled is not read for it. --mod <id> still turns it on for a run.
    bool has_switch = true;
    // The C hooks (any may be null)
    std::function<void(bool on)> on_enable;                        // the switch changed (also at start-up when on)
    std::function<void(const FrameContext&)> on_frame;
    std::function<void(const FrameContext&, const std::string& chunk)> on_level_enter;
    std::function<void(const FrameContext&, const std::string& chunk)> on_level_exit;
    std::function<void()> on_menu_build;
    std::function<void(const std::string& save, const std::string& file)> on_save;
    std::function<void(const std::string& save, const std::string& file)> on_load;
    // The mod's page (none when page_rows is null or answers 0)
    std::string page_title;
    std::function<uint32_t()> page_rows;
    std::function<std::string(uint32_t row)> page_row_label;
    std::function<Action(uint32_t row, WarpTarget& warp)> page_row_chosen;
};

// Registration (the built-in mods, at start-up before the module runs). RegisterBuiltins only registers (no service is touched:
// the runner may call it before te::SetSettings to learn the ids whose ps2r.ini keys to read); InstallBuiltins registers and
// installs the frame hook on te::Clock (after te::SetSettings: constructing the services reads the settings).
void Register(const Mod& mod);
void RegisterBuiltins();
void InstallBuiltins();
uint32_t Count();
const Mod* At(uint32_t index);
const Mod* Find(const std::string& id);
// The switches: read from ps2r.ini by the runner (Configure), toggled by the menu (SetEnabled: applied + persisted),
// forced for one run (--mod <id>, never persisted).
void Configure(const std::vector<std::string>& enabled_ids);
bool Enabled(uint32_t index);
bool Enabled(const std::string& id);
void SetEnabled(uint32_t index, bool on, bool persist = true);
bool AnyEnabled();
// The manifests embedded at build time (mods/<id>/mod.json): the text of one, "" when none; every registered mod must have one
// whose id / name / version agree (checked at InstallBuiltins, logged).
std::string Manifest(const std::string& id);
// One line for the log.
std::string Describe();

// The hook sources call these
void OnBoundary(uint64_t boundary);            // te::Clock's boundary hook (installed by InstallBuiltins)
void OnMenuBuild();                            // Platform::System::ModCount (the Options page being built)
void OnSave(const std::string& save, const std::string& file);
void OnLoad(const std::string& save, const std::string& file);
const FrameContext& LastFrame();

}  // namespace te::mods
