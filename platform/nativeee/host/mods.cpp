// platform/nativeee (host side) -- te_mods.h: the built-in mod framework. The registry, the switches (ps2r.ini
// mods.<id>.enabled through the engine's SettingsFile), the hook dispatch (the frame boundary, level entry / exit from the
// player's chunk, the menu build, the card's save / load) and the seam's Platform::System::Mod* imports the title's
// Options > MODS page is built from (ten: the count, label, switch, has-switch, the page's rows / label / row label / chosen
// row, the warp target). The manifests (mods/<id>/mod.json) are embedded by CMake (TE_GEN/mods/mods_manifests.cpp)
// and checked against the registrations.
#include "te_mods.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/core/config/settings_file.h"
#include "ps2e_seam.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_services.h"

// Generated: the manifests (id, json) and their count
struct te_mod_manifest { const char* id; const char* json; };
extern "C" const te_mod_manifest te_mod_manifests[];
extern "C" const unsigned te_mod_manifest_count;

namespace te::mods {

namespace {

struct Entry {
    Mod mod;
    bool enabled = false;
    bool forced = false;   // --mod <id>: on for this run, never persisted
};
std::vector<Entry>& Entries() { static std::vector<Entry> e; return e; }
bool g_installed = false;
FrameContext g_last;
std::string g_last_chunk;
bool g_have_player = false;

// The player's chunk, read as the authoring probe reads it (runner/te_probe.cpp; PAL retail globals, game/symbol_addrs.txt):
// G_UnkPlayableCharObjInstCxt -> Agent::instance -> ReferencedObject::chunk -> ChunkData::path (a game String)
constexpr uint32_t kPlayerCharacter = 0x003098FC;
constexpr uint32_t kAgentInstance = 0x00, kObjectChunk = 0xA0, kChunkPath = 0x138;

uint32_t Rd32(uint32_t address) {
    const uint8_t* p = GuestPointer(address, 4);
    if (!p) return 0;
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string PlayerChunk(bool* have_player) {
    *have_player = false;
    const uint32_t agent = Rd32(kPlayerCharacter);
    if (!agent) return "";
    const uint32_t instance = Rd32(agent + kAgentInstance);
    if (!instance || !GuestPointer(instance, 0x160)) return "";
    *have_player = true;
    const uint32_t chunk = Rd32(instance + kObjectChunk);
    if (!chunk || !GuestPointer(chunk, 0x1F0)) return "";
    const uint32_t text = Rd32(chunk + kChunkPath), length = Rd32(chunk + kChunkPath + 4);
    if (!text || length == 0 || length > 256) return "";
    const uint8_t* p = GuestPointer(text, length);
    if (!p) return "";
    return std::string(reinterpret_cast<const char*>(p), length);
}

void Persist(const Entry& e) {
    const std::string& path = GetSettings().settings_path;
    if (path.empty()) return;
    std::string err;
    const std::string key = "mods." + e.mod.id + ".enabled";
    if (!ps2r::SettingsFile::Set(path, key, e.enabled ? "on" : "off", &err))
        std::fprintf(stderr, "[twinsanityevolved] mods: %s not written to %s: %s\n", key.c_str(), path.c_str(), err.c_str());
}

// A flat JSON string field ("key": "value") of a manifest; "" when absent
std::string JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return "";
    at = json.find('"', at);
    if (at == std::string::npos) return "";
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

}  // namespace

const char* HookName(Hook hook) {
    static const char* const names[] = {"frame", "level_enter", "level_exit", "menu_build", "save", "load"};
    return uint32_t(hook) < uint32_t(Hook::Count) ? names[uint32_t(hook)] : "?";
}

void Register(const Mod& mod) {
    for (const Entry& e : Entries())
        if (e.mod.id == mod.id) { std::fprintf(stderr, "[twinsanityevolved] mods: %s registered twice\n", mod.id.c_str()); return; }
    Entry e;
    e.mod = mod;
    Entries().push_back(e);
}

uint32_t Count() { return uint32_t(Entries().size()); }
const Mod* At(uint32_t index) { return index < Entries().size() ? &Entries()[index].mod : nullptr; }
const Mod* Find(const std::string& id) {
    for (const Entry& e : Entries()) if (e.mod.id == id) return &e.mod;
    return nullptr;
}

std::string Manifest(const std::string& id) {
    for (unsigned i = 0; i < te_mod_manifest_count; ++i)
        if (id == te_mod_manifests[i].id) return te_mod_manifests[i].json;
    return "";
}

void Configure(const std::vector<std::string>& enabled_ids) {
    for (Entry& e : Entries()) {
        if (!e.mod.has_switch) continue;   // no switch: nothing persisted applies; the session's click turns it on
        const bool on = std::find(enabled_ids.begin(), enabled_ids.end(), e.mod.id) != enabled_ids.end();
        if (on && !e.enabled) {
            e.enabled = true;
            if (e.mod.on_enable) e.mod.on_enable(true);
        }
    }
}

bool Enabled(uint32_t index) { return index < Entries().size() && Entries()[index].enabled; }
bool Enabled(const std::string& id) {
    for (const Entry& e : Entries()) if (e.mod.id == id) return e.enabled;
    return false;
}

void SetEnabled(uint32_t index, bool on, bool persist) {
    if (index >= Entries().size()) return;
    Entry& e = Entries()[index];
    if (!e.mod.has_switch) persist = false;   // a switchless mod is on for the session only
    if (!persist) e.forced = on;
    if (e.enabled == on) { if (persist) Persist(e); return; }
    e.enabled = on;
    if (e.mod.on_enable) e.mod.on_enable(on);
    if (persist) Persist(e);
    std::fprintf(stderr, "[twinsanityevolved] mods: %s %s (%s)\n", e.mod.id.c_str(), on ? "ON" : "off", Describe().c_str());
}

bool AnyEnabled() {
    for (const Entry& e : Entries()) if (e.enabled) return true;
    return false;
}

std::string Describe() {
    std::string s = "mods:";
    if (Entries().empty()) return s + " none";
    for (const Entry& e : Entries()) {
        s += " " + e.mod.id + "=" + (e.enabled ? "on" : "off");
        if (e.forced) s += e.mod.has_switch ? "(--mod)" : "(session)";
    }
    s += AnyEnabled() ? "; saves -> the -mods card" : "; saves -> the vanilla card";
    return s;
}

const FrameContext& LastFrame() { return g_last; }

void OnBoundary(uint64_t boundary) {
    if (Entries().empty()) return;
    FrameContext ctx;
    ctx.boundary = boundary;
    unsigned state = 0, last = 0, next = 0;
    if (harness::ReadGameState(&state, &last, &next)) ctx.state = state;
    ctx.paused = ctx.state == 15;                       // StatePaused
    ctx.playing = ctx.state == 12 || ctx.state == 13;   // StatePlaying, StateWatching
    bool have_player = false;
    ctx.chunk = PlayerChunk(&have_player);
    // A level entered / left: the player's chunk path changed (the chunk name the trace logs). Leaving play (no player,
    // the title) leaves the last chunk.
    if (ctx.chunk != g_last_chunk) {
        if (!g_last_chunk.empty())
            for (Entry& e : Entries()) if (e.enabled && e.mod.on_level_exit) e.mod.on_level_exit(ctx, g_last_chunk);
        if (!ctx.chunk.empty())
            for (Entry& e : Entries()) if (e.enabled && e.mod.on_level_enter) e.mod.on_level_enter(ctx, ctx.chunk);
        g_last_chunk = ctx.chunk;
    }
    g_have_player = have_player;
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_frame) e.mod.on_frame(ctx);
    g_last = ctx;
}

void OnMenuBuild() {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_menu_build) e.mod.on_menu_build();
}

void OnSave(const std::string& save, const std::string& file) {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_save) e.mod.on_save(save, file);
}

void OnLoad(const std::string& save, const std::string& file) {
    for (Entry& e : Entries()) if (e.enabled && e.mod.on_load) e.mod.on_load(save, file);
}

// The built-in mods register themselves here (one function per mod source file)
void RegisterLevelSelect();
void RegisterSpeedrun();

void RegisterBuiltins() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    RegisterLevelSelect();
    RegisterSpeedrun();
}

void InstallBuiltins() {
    if (g_installed) return;
    g_installed = true;
    RegisterBuiltins();
    // Every registration has a manifest that agrees with it
    for (const Entry& e : Entries()) {
        const std::string json = Manifest(e.mod.id);
        if (json.empty()) { std::fprintf(stderr, "[twinsanityevolved] mods: %s has no mods/%s/mod.json\n", e.mod.id.c_str(), e.mod.id.c_str()); continue; }
        const std::string id = JsonString(json, "id"), name = JsonString(json, "name"), version = JsonString(json, "version");
        if (id != e.mod.id || name != e.mod.name || version != e.mod.version)
            std::fprintf(stderr, "[twinsanityevolved] mods: mods/%s/mod.json (%s / %s / %s) disagrees with the built-in (%s / %s / %s)\n",
                         e.mod.id.c_str(), id.c_str(), name.c_str(), version.c_str(), e.mod.id.c_str(), e.mod.name.c_str(), e.mod.version.c_str());
    }
    Services::Get().clock().AddBoundaryHook("mods", OnBoundary);
}

}  // namespace te::mods

// ---- the seam: Platform::System::Mod* (include/platform/system.h; the generator maps the header's u32 to ps2e_s32) ------
namespace {
void CopyLabel(const std::string& label, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, size_t(size));
    if (!p) return;
    const size_t n = std::min(label.size(), size_t(size) - 1);
    std::memcpy(p, label.data(), n);
    p[n] = 0;
}
const te::mods::Mod* ModAt(ps2e_s32 mod) { return mod >= 0 ? te::mods::At(uint32_t(mod)) : nullptr; }
te::mods::WarpTarget g_warp;
}  // namespace

extern "C" {

ps2e_s32 ps2e_system_mod_count(void) {
    te::mods::OnMenuBuild();
    return ps2e_s32(te::mods::Count());
}

void ps2e_system_mod_label(ps2e_s32 mod, ps2e_u32 out, ps2e_s32 size) {
    const te::mods::Mod* m = ModAt(mod);
    CopyLabel(m ? m->name : "", out, size);
}

ps2e_s32 ps2e_system_mod_enabled(ps2e_s32 mod) { return mod >= 0 && te::mods::Enabled(uint32_t(mod)) ? 1 : 0; }

ps2e_s32 ps2e_system_mod_has_switch(ps2e_s32 mod) {
    const te::mods::Mod* m = ModAt(mod);
    return !m || m->has_switch ? 1 : 0;
}

void ps2e_system_set_mod_enabled(ps2e_s32 mod, ps2e_s32 enabled) {
    if (mod >= 0) te::mods::SetEnabled(uint32_t(mod), enabled != 0, true);
}

ps2e_s32 ps2e_system_mod_page_rows(ps2e_s32 mod) {
    const te::mods::Mod* m = ModAt(mod);
    return m && m->page_rows ? ps2e_s32(m->page_rows()) : 0;
}

void ps2e_system_mod_page_label(ps2e_s32 mod, ps2e_u32 out, ps2e_s32 size) {
    const te::mods::Mod* m = ModAt(mod);
    CopyLabel(m ? m->page_title : "", out, size);
}

void ps2e_system_mod_page_row_label(ps2e_s32 mod, ps2e_s32 row, ps2e_u32 out, ps2e_s32 size) {
    const te::mods::Mod* m = ModAt(mod);
    CopyLabel(m && m->page_row_label && row >= 0 ? m->page_row_label(uint32_t(row)) : "", out, size);
}

ps2e_s32 ps2e_system_mod_page_row_chosen(ps2e_s32 mod, ps2e_s32 row) {
    const te::mods::Mod* m = ModAt(mod);
    if (!m || !m->page_row_chosen || row < 0 || !te::mods::Enabled(uint32_t(mod))) return te::mods::ActionNone;
    g_warp = te::mods::WarpTarget{};
    const te::mods::Action action = m->page_row_chosen(uint32_t(row), g_warp);
    std::fprintf(stderr, "[twinsanityevolved] mods: %s row %d chosen -> action %u%s%s'\n", m->id.c_str(), row, unsigned(action),
                 action == te::mods::ActionWarp ? " warp to '" : " '", g_warp.chunk.c_str());
    return ps2e_s32(action);
}

void ps2e_system_mod_warp_target(ps2e_u32 chunk, ps2e_s32 size, ps2e_u32 character) {
    CopyLabel(g_warp.chunk, chunk, size);
    if (uint8_t* p = te::GuestPointer(character, 4)) std::memcpy(p, &g_warp.character, 4);
}

}  // extern "C"
