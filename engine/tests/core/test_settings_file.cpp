// tests/core/test_settings_file.cpp -- SettingsFile, the settings writer the
// in-game options (MenuExtension) persist through. argv[1] = a scratch directory.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/core/config/config.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/config/settings_file.h"

using ps2r::Config;
using ps2r::ConfigLoader;
using ps2r::SettingsFile;

static int g_fail = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_fail++;                                                                \
        }                                                                            \
    } while (0)

static std::string Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    // Replace in place: flat form, comments and order preserved.
    {
        const std::string text = "# user settings\nvideo.scale = 2\nui.perf_overlay=off ; the overlay\naudio.volume = 80\n";
        bool changed = false;
        const std::string next = SettingsFile::SetInText(text, "ui.perf_overlay", "on", &changed);
        CHECK(changed);
        CHECK(next == "# user settings\nvideo.scale = 2\nui.perf_overlay = on\naudio.volume = 80\n");
        std::string v;
        CHECK(SettingsFile::GetFromText(next, "ui.perf_overlay", &v) && v == "on");
        CHECK(SettingsFile::GetFromText(next, "video.scale", &v) && v == "2");
        CHECK(!SettingsFile::GetFromText(next, "video.vsync", &v));
        // Unchanged value: text identical, changed=false.
        changed = false;
        CHECK(SettingsFile::SetInText(next, "ui.perf_overlay", "on", &changed) == next && !changed);
    }
    // Replace in place: a [section]-scoped short key keeps its spelling.
    {
        const std::string text = "[ui]\ngui_open = on\nperf_overlay = off\n[video]\nscale = 1\n";
        const std::string next = SettingsFile::SetInText(text, "ui.perf_overlay", "on");
        CHECK(next == "[ui]\ngui_open = on\nperf_overlay = on\n[video]\nscale = 1\n");
        // Append after a section: the flat key is written outside the section.
        const std::string app = SettingsFile::SetInText(next, "input.kbm_pad", "off");
        CHECK(app == "[ui]\ngui_open = on\nperf_overlay = on\n[video]\nscale = 1\n[]\ninput.kbm_pad = off\n");
        Config c;
        std::string err;
        CHECK(ConfigLoader::ApplyIniText(app, c, &err));   // the reader accepts what the writer wrote
        CHECK(c.ui.perf_overlay && !c.input.kbm_pad && c.video.scale == 1 && c.ui.gui_open);
    }
    // Append to an empty text.
    {
        const std::string next = SettingsFile::SetInText("", "ui.perf_overlay", "on");
        CHECK(next == "ui.perf_overlay = on\n");
    }
    // Writable: schema keys only, never harness plumbing.
    CHECK(SettingsFile::Writable("ui.perf_overlay"));
    CHECK(SettingsFile::Writable("input.kbm_pad"));
    CHECK(!SettingsFile::Writable("harness.frames"));
    CHECK(!SettingsFile::Writable("ui.not_a_key"));
    // The file route: create, replace, refuse, round-trip through the loader.
    {
        const std::string path = dir + "/settings_file_test.ini";
        std::remove(path.c_str());
        std::string err;
        CHECK(SettingsFile::Set(path, "ui.perf_overlay", "on", &err));
        CHECK(Read(path) == "ui.perf_overlay = on\n");
        CHECK(SettingsFile::Set(path, "audio.volume", "55", &err));
        CHECK(SettingsFile::Set(path, "ui.perf_overlay", "off", &err));
        CHECK(Read(path) == "ui.perf_overlay = off\naudio.volume = 55\n");
        // Game.language is a writable schema key (the title's Options menu persists it); the loader reads it back.
        CHECK(SettingsFile::Writable("game.language"));
        CHECK(SettingsFile::Set(path, "game.language", "french", &err));
        {
            Config lang;
            bool had = false;
            CHECK(ConfigLoader::ApplyIniFile(path, lang, &err, &had) && had && lang.game.language == "french");
        }
        CHECK(SettingsFile::Set(path, "game.language", "english", &err));
        CHECK(Read(path) == "ui.perf_overlay = off\naudio.volume = 55\ngame.language = english\n");
        CHECK(!SettingsFile::Set(path, "harness.frames", "3", &err) && err.find("harness.frames") != std::string::npos);
        CHECK(!SettingsFile::Set(path, "bogus.key", "3", &err));
        CHECK(Read(path) == "ui.perf_overlay = off\naudio.volume = 55\ngame.language = english\n");   // refusals leave the file alone
        // TwinsanityEvolved: display.fps_counter and the mods.<id>.enabled family are writable (the title's Options /
        // Mods pages persist them); the loader reads both back; a malformed mod id is refused like any unknown key.
        CHECK(SettingsFile::Writable("display.fps_counter"));
        CHECK(SettingsFile::Writable("mods.level_select.enabled") && SettingsFile::Writable("mods.speedrun.enabled"));
        CHECK(!SettingsFile::Writable("mods..enabled") && !SettingsFile::Writable("mods.Level-Select.enabled") &&
              !SettingsFile::Writable("mods.level_select.volume"));
        CHECK(SettingsFile::Set(path, "display.fps_counter", "on", &err));
        CHECK(SettingsFile::Set(path, "mods.speedrun.enabled", "on", &err));
        CHECK(!SettingsFile::Set(path, "mods.Bogus.enabled", "on", &err));
        {
            Config w3;
            bool had = false;
            CHECK(ConfigLoader::ApplyIniFile(path, w3, &err, &had) && had && w3.display.fps_counter && w3.mods.Enabled("speedrun") &&
                  !w3.mods.Enabled("level_select") && w3.mods.Any());
            std::string v;
            CHECK(w3.Get("mods.speedrun.enabled", &v) && v == "on");
            CHECK(w3.Get("mods.level_select.enabled", &v) && v == "off");
            CHECK(w3.Fingerprint().find("mods.speedrun.enabled=on;") != std::string::npos);
        }
        CHECK(SettingsFile::Set(path, "display.fps_counter", "off", &err));
        CHECK(SettingsFile::Set(path, "mods.speedrun.enabled", "off", &err));
        CHECK(Read(path) == "ui.perf_overlay = off\naudio.volume = 55\ngame.language = english\ndisplay.fps_counter = off\nmods.speedrun.enabled = off\n");
        Config c;
        bool existed = false;
        CHECK(ConfigLoader::ApplyIniFile(path, c, &err, &existed) && existed);
        CHECK(!c.ui.perf_overlay && c.audio.volume == 55);
        CHECK(Read(path + ".tmp").empty());   // no temporary left behind
        std::remove(path.c_str());
    }
    if (g_fail) {
        std::fprintf(stderr, "test_settings_file: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("test_settings_file: PASS\n");
    return 0;
}
