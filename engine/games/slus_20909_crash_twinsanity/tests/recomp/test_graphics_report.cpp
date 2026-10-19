// GraphicsReport: the title-owned run.meta publication of the Graphics
// replaced-model census. Pins (1) the exact keys/`name=value,...` grammar the
// retirement gate parses (verify/retirement_artifacts.py counter()), (2) that every
// member the reviewed catalog names is published, and (3) that the published
// numbers are the composed counters, unchanged.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_report.h"

namespace {
int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", what); }
}

// The gate's grammar: comma-separated `key=value` members, values decimal
// without sign or leading zeros, keys unique.
bool Members(const std::string& row, std::map<std::string, unsigned long long>& out) {
    out.clear();
    std::stringstream parts(row);
    std::string part;
    while (std::getline(parts, part, ',')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= part.size()) return false;
        const std::string key = part.substr(0, eq), value = part.substr(eq + 1);
        if (out.count(key)) return false;
        if (value.size() > 1 && value[0] == '0') return false;
        for (char c : value) if (c < '0' || c > '9') return false;
        out[key] = std::stoull(value);
    }
    return !out.empty();
}

std::map<std::string, std::string> Keys(const ps2r::verify::RunMeta& meta) {
    std::map<std::string, std::string> out;
    std::stringstream lines(meta.Render());
    std::string line;
    while (std::getline(lines, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}
}  // namespace

int main() {
    using title::GraphicsReport;
    ps2r::verify::RunMeta meta("", {});

    title::GraphicsService::Counters strips;
    for (size_t i = 0; i < strips.variants.size(); ++i) {
        strips.variants[i].calls = 10 + i;
        strips.variants[i].replaced = 20 + i;
        strips.variants[i].model_calls = 30 + i;
        strips.variants[i].interior_fallback = 40 + i;
    }
    GraphicsReport::Strips(meta, strips);
    title::SpriteService::Counters sprites{101, 102, 103, 104, 105, 106};  // {calls, replaced, model_calls, failed, empty, refused}
    GraphicsReport::Sprites(meta, sprites);
    title::TextService::Counters text;  // {calls, replaced, empty_queues, model_calls, failed}
    text.calls = 201; text.replaced = 202; text.empty_queues = 203; text.model_calls = 204; text.failed = 205;
    text.refused = 206;
    GraphicsReport::Text(meta, text);
    title::SkyPass::Counters sky;
    sky.start = {301, 302, 303, 304};
    sky.model = {311, 312, 313, 314};
    sky.finish = {321, 322, 323, 324};
    sky.models_loaded = 331; sky.textures_loaded = 332; sky.load_declines = 333;
    sky.faults = 334; sky.refused = 335; sky.last_refusal = title::SkyPass::Refusal::ModelMissing;
    sky.declines_memory = 336; sky.declines_asset = 337; sky.declines_limit = 338;
    sky.programs_uploaded = 339; sky.declines_converted = 340; sky.textures_converted = 341;
    GraphicsReport::Sky(meta, sky);
    title::FrameService::Counters frame;
    frame.setup = {401, 402, 403};
    frame.send = {411, 412, 413};
    frame.interrupt = {421, 422, 423};
    frame.clear = {431, 432, 433};
    frame.layout = {441, 442, 443};
    GraphicsReport::Frame(meta, frame);

    const auto keys = Keys(meta);
    std::map<std::string, unsigned long long> m;

    // (1) exact rows: the grammar and byte layout the gate and run.meta readers see
    Check(keys.at("graphics_strip_plain") == "calls=10,replaced=20,model_calls=30,interior=40", "strip plain row");
    Check(keys.at("graphics_strip_tinted") == "calls=11,replaced=21,model_calls=31,interior=41", "strip tinted row");
    Check(keys.at("graphics_strip_placed") == "calls=12,replaced=22,model_calls=32,interior=42", "strip placed row");
    Check(keys.at("graphics_strip_placed_tinted") == "calls=13,replaced=23,model_calls=33,interior=43", "strip placed_tinted row");
    Check(keys.at("graphics_sprites") == "replaced=102,model_calls=103,failed=104,empty=105,refused=106", "sprites row");
    Check(keys.at("graphics_text") == "replaced=202,model_calls=204,failed=205,refused=206", "text row");
    Check(keys.at("graphics_sky_start") == "replaced=302,model_calls=303,interior_calls=304", "sky start row");
    Check(keys.at("graphics_sky_model") == "replaced=312,model_calls=313,interior_calls=314", "sky model row");
    Check(keys.at("graphics_sky_finish") == "replaced=322,model_calls=323,interior_calls=324", "sky finish row");
    Check(keys.at("graphics_sky_assets") == "models_loaded=331,textures_loaded=332,load_declines=333,declines_memory=336,declines_asset=337,"
          "declines_limit=338,declines_converted=340,textures_converted=341,refused=335,last_refusal=4,faults=334,"
          "programs_uploaded=339",
          "sky assets row");
    Check(keys.at("graphics_frame_setup") == "calls=401,replaced=402,model_calls=403", "frame setup row");
    Check(keys.at("graphics_frame_send") == "calls=411,replaced=412,model_calls=413", "frame send row");
    Check(keys.at("graphics_frame_interrupt") == "calls=421,replaced=422,model_calls=423", "frame interrupt row");
    Check(keys.at("graphics_frame_clear") == "calls=431,replaced=432,model_calls=433", "frame clear row");
    Check(keys.at("graphics_frame_layout") == "calls=441,replaced=442,model_calls=443", "frame layout row");

    // (2) every published row parses under the gate grammar and carries the
    // replaced-model member `model_calls` the catalog selects
    const char* model_keys[] = {
        "graphics_strip_plain", "graphics_strip_tinted", "graphics_strip_placed", "graphics_strip_placed_tinted",
        "graphics_sprites", "graphics_text", "graphics_sky_start", "graphics_sky_model", "graphics_sky_finish",
        "graphics_frame_setup", "graphics_frame_send", "graphics_frame_interrupt", "graphics_frame_clear",
        "graphics_frame_layout"};
    for (const char* key : model_keys) {
        Check(keys.count(key) == 1, key);
        Check(Members(keys.at(key), m) && m.count("model_calls") == 1, "gate grammar + model_calls member");
    }
    Check(Members("a=1,b=0", m) && m.size() == 2, "grammar accepts canonical members");
    Check(!Members("a=01", m) && !Members("a=1,a=2", m) && !Members("a=", m) && !Members("=1", m) && !Members("", m),
          "grammar rejects leading zeros, duplicates, empty value/key, empty row");

    // (3) zero counters publish zeros, never absent keys (missing is not zero at the gate)
    ps2r::verify::RunMeta zero("", {});
    GraphicsReport::Sprites(zero, {});
    GraphicsReport::Frame(zero, {});
    const auto z = Keys(zero);
    Check(z.at("graphics_sprites") == "replaced=0,model_calls=0,failed=0,empty=0,refused=0", "zero sprites row");
    Check(z.at("graphics_frame_send") == "calls=0,replaced=0,model_calls=0", "zero frame send row");

    // Re-publishing replaces the row in place (RunMeta::Set semantics), so a
    // later Record() never leaves two rows for one key.
    sprites.model_calls = 7;
    GraphicsReport::Sprites(meta, sprites);
    Check(Keys(meta).at("graphics_sprites") == "replaced=102,model_calls=7,failed=104,empty=105,refused=106", "republish replaces");
    size_t rows = 0;
    { std::stringstream lines(meta.Render()); std::string line;
      while (std::getline(lines, line)) rows += line.rfind("graphics_sprites=", 0) == 0; }
    Check(rows == 1, "one row per key");

    std::printf("GraphicsReport: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
