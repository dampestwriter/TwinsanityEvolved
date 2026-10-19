// tests/title/menu/test_menu_extension.cpp -- MenuExtension over a fake title:
// a fake RAM and a fake implementation of the title's menu constructors with
// the layout MenuLayout documents (TR menus.h/menus.cpp), so the extension's
// registration at OptionsPage::Construct, its text-table extension, the
// page/item construction, the Options-page link order, the value sync in both
// directions, the restore re-validation and the refusals are pinned without a
// game process. The game-side truth is the Root pin RootMenuPerfOverlay.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_extension.h"
#include "games/slus_20909_crash_twinsanity/tests/title/menu/fake_title.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_layout.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_strings.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_text_table.h"

using title::MenuExtension;
using title::MenuGuest;
using title::MenuLayout;
using title::MenuPageSpec;
using title::MenuStrings;
using title::MenuWidget;
using menu_test::FakeTitle;
using menu_test::kBasePageVTable;
using menu_test::kBackVTable;
using menu_test::kChoiceVTable;
using menu_test::kLinkVTable;
using menu_test::kOriginalLines;
using menu_test::kToggleVTable;
using menu_test::kValueVTable;
using L = MenuLayout;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

namespace {

struct Host {
    int32_t perf = 0, fullscreen = 0, kbm = 1, filter = 2, scale = 3, key = 1;
    std::vector<std::pair<std::string, int32_t>> applied;
};

MenuWidget Toggle(MenuStrings::Id label, const char* key, int32_t* value, Host& host) {
    MenuWidget w;
    w.kind = MenuWidget::Kind::Toggle;
    w.label = label;
    w.key = key;
    w.read = [value] { return *value; };
    w.apply = [&host, key](int32_t v) { host.applied.emplace_back(key, v); };
    return w;
}

void Declare(MenuExtension& ext, Host& host) {
    MenuPageSpec display;
    display.title = MenuStrings::Id::Display;
    display.widgets.push_back(Toggle(MenuStrings::Id::PerfOverlay, "ui.perf_overlay", &host.perf, host));
    display.widgets.push_back(Toggle(MenuStrings::Id::Fullscreen, "video.fullscreen", &host.fullscreen, host));
    MenuWidget filter;
    filter.kind = MenuWidget::Kind::Enum;
    filter.label = MenuStrings::Id::Display;
    filter.key = "video.filter";
    filter.choices = {MenuStrings::Id::Display, MenuStrings::Id::Controls, MenuStrings::Id::Fullscreen};
    filter.read = [&host] { return host.filter; };
    filter.apply = [&host](int32_t v) { host.applied.emplace_back("video.filter", v); };
    display.widgets.push_back(filter);
    MenuWidget scale;
    scale.kind = MenuWidget::Kind::Slider;
    scale.label = MenuStrings::Id::Display;
    scale.key = "video.scale";
    scale.minimum = 1;
    scale.maximum = 4;
    scale.wraps = false;
    scale.read = [&host] { return host.scale; };
    scale.apply = [&host](int32_t v) { host.applied.emplace_back("video.scale", v); };
    display.widgets.push_back(scale);
    ext.AddPage(display);
    MenuPageSpec controls;
    controls.title = MenuStrings::Id::Controls;
    controls.widgets.push_back(Toggle(MenuStrings::Id::KeyboardMouse, "input.kbm_pad", &host.kbm, host));
    MenuWidget binding;
    binding.kind = MenuWidget::Kind::KeyBinding;
    binding.label = MenuStrings::Id::Controls;
    binding.key = "input.jump";
    binding.choices = {MenuStrings::Id::Display, MenuStrings::Id::Controls};
    binding.read = [&host] { return host.key; };
    binding.apply = [&host](int32_t v) { host.applied.emplace_back("input.jump", v); };
    controls.widgets.push_back(binding);
    ext.AddPage(controls);
}

// Installs the extension over a fake "original" that returns the Options page
// in v0, then runs the installed body once (the title's OLEG::StartUp call).
MenuExtension::Body InstallOver(MenuExtension& ext, FakeTitle& t, bool* installed_ok) {
    MenuExtension::Body installed;
    const bool ok = ext.Install(
        [&t](uint32_t addr) -> MenuExtension::Body {
            if (addr != L::kOptionsPageConstruct) return {};
            return [&t](CpuContext& ctx, uint32_t) { ctx.cpuRegs.GPR.r[2].UL[0] = t.options; ctx.cpuRegs.GPR.r[8].UL[0] = 0x1234; };
        },
        [&installed](uint32_t addr, MenuExtension::Body body) {
            if (addr != L::kOptionsPageConstruct) return false;
            installed = std::move(body);
            return true;
        });
    if (installed_ok) *installed_ok = ok;
    return installed;
}

void TestConstruction() {
    FakeTitle t;
    t.SeedTitle();
    Host host;
    MenuExtension ext(t.Guest());
    Declare(ext, host);
    bool ok = false;
    MenuExtension::Body body = InstallOver(ext, t, &ok);
    CHECK(ok && body);
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[4].UL[0] = t.options;
    body(ctx, 0);
    // The caller sees exactly the constructor's registers (v0 = the page, t0 untouched).
    CHECK(ctx.cpuRegs.GPR.r[2].UL[0] == t.options && ctx.cpuRegs.GPR.r[8].UL[0] == 0x1234);
    CHECK(ext.GetState() == MenuExtension::State::Registered);
    CHECK(ext.OptionsPage() == t.options);
    const MenuExtension::Counters& c = ext.Counts();
    CHECK(c.constructs == 1 && c.pages == 2 && c.items == 6 && c.texts == MenuStrings::kCount && c.banks == 1);
    // The text table: every original line copied, the strings appended at kOriginalLines.
    CHECK(ext.TextBase() == kOriginalLines);
    CHECK(t.R(L::kTextLineCounts) == kOriginalLines + MenuStrings::kCount);
    const uint32_t table = t.R(L::kTexts);
    CHECK(table != t.lines && t.R(t.R(L::kLanguageTexts)) == table);
    for (uint32_t i = 0; i < kOriginalLines; i += 37) CHECK(t.Str(t.R(table + i * 4)) == "line" + std::to_string(i));
    CHECK(t.Str(t.R(table + (kOriginalLines + 0) * 4)) == "display");
    CHECK(t.Str(t.R(table + (kOriginalLines + 1) * 4)) == "controls");
    CHECK(t.Str(t.R(table + (kOriginalLines + 2) * 4)) == "fps overlay");
    CHECK(t.Str(t.R(table + (kOriginalLines + 4) * 4)) == "keyboard and mouse");
    // The Options page: 3 title links, Display, Controls, then the title's back item.
    CHECK(t.R(t.options + L::kPageItemsCount) == 6);
    const uint32_t data = t.R(t.options + L::kPageItemsData);
    const uint32_t link_display = t.R(data + 3 * 4), link_controls = t.R(data + 4 * 4);
    CHECK(t.R(data + 5 * 4) == t.title_back);
    CHECK(t.R(link_display + L::kItemVTable) == kLinkVTable);
    CHECK(L::ItemTextIndex(t.R(link_display + L::kItemBits)) == kOriginalLines + 0);
    CHECK(L::ItemTextIndex(t.R(link_controls + L::kItemBits)) == kOriginalLines + 1);
    CHECK(L::ItemId(t.R(link_display + L::kItemBits)) >= L::kFirstExtensionItemId);
    // The Display page: titled, parented to Options, 4 widgets + a back item to Options.
    const uint32_t display = t.R(link_display + L::kItemTarget);
    CHECK(L::IsRam(display) && t.R(display + L::kPageParent) == t.options);
    CHECK((t.R(display + L::kPageFlags) & 0x3ff) == kOriginalLines + 0);
    CHECK(t.R(display + L::kPageVTable) == kBasePageVTable);
    CHECK(t.R(display + L::kPageItemsCount) == 5);
    const uint32_t ditems = t.R(display + L::kPageItemsData);
    const uint32_t perf = t.R(ditems + 0), full = t.R(ditems + 4), filter = t.R(ditems + 8), scale = t.R(ditems + 12),
                   back = t.R(ditems + 16);
    CHECK(perf == ext.ItemAddress(0, 0) && full == ext.ItemAddress(0, 1));
    CHECK(t.R(perf + L::kItemVTable) == kToggleVTable);
    CHECK(L::ItemTextIndex(t.R(perf + L::kItemBits)) == kOriginalLines + 2);
    CHECK(L::ItemId(t.R(perf + L::kItemBits)) == L::kFirstExtensionItemId);
    CHECK(t.R(t.R(perf + L::kChoiceItemTexts)) == L::kOffText && t.R(t.R(perf + L::kChoiceItemTexts) + 4) == L::kOnText);
    CHECK(t.R(perf + L::kValueItemValues) == 0 && t.R(full + L::kValueItemValues) == 0);
    CHECK(t.R(filter + L::kItemVTable) == kChoiceVTable && t.R(filter + L::kChoiceItemChoices) == 3);
    CHECK(t.R(filter + L::kValueItemValues) == 2 && t.R(filter + L::kValueItemMaximum) == 2);
    CHECK(t.R(t.R(filter + L::kChoiceItemTexts) + 8) == kOriginalLines + 3);   // choice 2 = Fullscreen's text
    CHECK(t.R(scale + L::kItemVTable) == kValueVTable && t.R(scale + L::kValueItemMinimum) == 1 &&
          t.R(scale + L::kValueItemMaximum) == 4 && t.R(scale + L::kValueItemValues) == 3 && *t.At(scale + L::kValueItemWraps) == 0);
    CHECK(t.R(back + L::kItemVTable) == kBackVTable && t.R(back + L::kItemTarget) == t.options &&
          L::ItemId(t.R(back + L::kItemBits)) == L::kSubPageBackItem);
    // The Controls page: the keyboard toggle (value 1) and the binding (2 choices).
    const uint32_t controls = t.R(link_controls + L::kItemTarget);
    CHECK(t.R(controls + L::kPageItemsCount) == 3);
    const uint32_t citems = t.R(controls + L::kPageItemsData);
    CHECK(t.R(t.R(citems) + L::kValueItemValues) == 1);
    CHECK(t.R(t.R(citems + 4) + L::kChoiceItemChoices) == 2 && t.R(t.R(citems + 4) + L::kValueItemValues) == 1);
    // Every allocation went through the title's allocators (nothing host-owned in RAM).
    CHECK(t.calls[L::kMemoryAllocate] > 0 && t.calls[L::kMemoryAllocate2] > 0);
    CHECK(t.calls[L::kMenuPageConstructTitled] == 3 && t.calls[L::kMenuPageAdd] == 4 + 5 + 3 + 2);

    // --- the value sync -------------------------------------------------------
    for (int i = 0; i < 3; ++i) ext.OnBoundary();
    CHECK(ext.Counts().changes == 0 && ext.Counts().pushes == 0 && ext.Counts().boundaries == 3);
    // The user steps the fps overlay on in the title's menu: apply runs once.
    t.W(perf + L::kValueItemValues, 1);
    ext.OnBoundary();
    CHECK(ext.Counts().changes == 1 && host.applied.size() == 1 && host.applied[0].first == "ui.perf_overlay" && host.applied[0].second == 1);
    // The host has not applied it yet: no push back of the old value.
    ext.OnBoundary();
    CHECK(ext.Counts().pushes == 0 && t.R(perf + L::kValueItemValues) == 1);
    host.perf = 1;   // the presenter applied it
    ext.OnBoundary();
    ext.OnBoundary();
    CHECK(ext.Counts().pushes == 0 && ext.Counts().changes == 1);
    // Changed outside the menu: the item follows the host.
    host.perf = 0;
    ext.OnBoundary();
    CHECK(ext.Counts().pushes == 1 && t.R(perf + L::kValueItemValues) == 0);
    // A slider step and an enum step apply with their values.
    t.W(scale + L::kValueItemValues, 4);
    t.W(filter + L::kValueItemValues, 0);
    ext.OnBoundary();
    CHECK(ext.Counts().changes == 3 && host.applied.size() == 3);
    CHECK(host.applied[1].first == "video.filter" && host.applied[1].second == 0);   // page order
    CHECK(host.applied[2].first == "video.scale" && host.applied[2].second == 4);

    // --- restore re-validation ------------------------------------------------
    ext.NoteRestored();
    ext.OnBoundary();
    CHECK(ext.GetState() == MenuExtension::State::Registered && ext.Counts().revalidations == 1);
    t.W(t.options + L::kPageVTable, 0);   // an image without the extension's Options page
    ext.NoteRestored();
    ext.OnBoundary();
    CHECK(ext.GetState() == MenuExtension::State::Dormant);
    t.W(perf + L::kValueItemValues, 1);
    ext.OnBoundary();
    CHECK(ext.Counts().changes == 3);   // dormant: nothing applied
    t.W(t.options + L::kPageVTable, L::kOptionsPageVTable);
    ext.NoteRestored();
    ext.OnBoundary();
    CHECK(ext.GetState() == MenuExtension::State::Registered && ext.Refusal().empty());
    // The restored image's menu value (1) yields to the host's setting (0): no apply.
    CHECK(t.R(perf + L::kValueItemValues) == 0 && ext.Counts().changes == 3);

    ps2r::verify::RunMeta meta("/dev/null", {});
    ext.Record(meta);
    CHECK(meta.Render().find("menu_ext=state=registered") != std::string::npos);
    CHECK(meta.Render().find("ui.perf_overlay=0") != std::string::npos);
    const std::string census = ext.CensusLine();
    CHECK(census.find("state=registered") == 0 && census.find(",pages=2,items=6,texts=" + std::to_string(MenuStrings::kCount) + ",banks=1,text_base=228,") != std::string::npos);
    std::printf("census: menu_ext=%s\n", census.c_str());
}

void TestRefusals() {
    {   // no language bank loaded: refused, the Options page untouched
        FakeTitle t;
        t.SeedTitle(false);
        Host host;
        MenuExtension ext(t.Guest());
        Declare(ext, host);
        MenuExtension::Body body = InstallOver(ext, t, nullptr);
        CpuContext ctx{};
        body(ctx, 0);
        CHECK(ext.GetState() == MenuExtension::State::Refused && ext.Refusal() == "no language bank is loaded");
        CHECK(t.R(t.options + L::kPageItemsCount) == 4);
        ext.OnBoundary();
        CHECK(ext.Counts().boundaries == 0);
    }
    {   // the page returned is not an Options page: refused
        FakeTitle t;
        t.SeedTitle();
        t.W(t.options + L::kPageVTable, kBasePageVTable);
        Host host;
        MenuExtension ext(t.Guest());
        Declare(ext, host);
        MenuExtension::Body body = InstallOver(ext, t, nullptr);
        CpuContext ctx{};
        body(ctx, 0);
        CHECK(ext.GetState() == MenuExtension::State::Refused && ext.Refusal() == "options page vtable mismatch");
        CHECK(t.R(L::kTextLineCounts) == kOriginalLines);
    }
    {   // the allocator refuses: refused, nothing registered
        FakeTitle t;
        t.SeedTitle();
        t.allocator_fails = true;
        Host host;
        MenuExtension ext(t.Guest());
        Declare(ext, host);
        MenuExtension::Body body = InstallOver(ext, t, nullptr);
        CpuContext ctx{};
        body(ctx, 0);
        CHECK(ext.GetState() == MenuExtension::State::Refused);
    }
    {   // an interior entry of the constructor (a continuation) never registers
        FakeTitle t;
        t.SeedTitle();
        Host host;
        MenuExtension ext(t.Guest());
        Declare(ext, host);
        MenuExtension::Body body = InstallOver(ext, t, nullptr);
        CpuContext ctx{};
        body(ctx, L::kOptionsPageConstruct + 0x48);
        CHECK(ext.GetState() == MenuExtension::State::Unregistered && ext.Counts().constructs == 0);
    }
    {   // nothing declared: Install refuses
        FakeTitle t;
        MenuExtension ext(t.Guest());
        bool ok = true;
        InstallOver(ext, t, &ok);
        CHECK(!ok);
    }
    // The strings: every language bank the table knows has a text for every id.
    uint32_t n = 0;
    const char* const* langs = MenuStrings::Languages(&n);
    CHECK(n == 6);
    for (uint32_t l = 0; l < n; ++l)
        for (uint32_t i = 0; i < MenuStrings::kCount; ++i) CHECK(*MenuStrings::Text(static_cast<MenuStrings::Id>(i), langs[l]) != 0);
    CHECK(std::string(MenuStrings::Text(MenuStrings::Id::PerfOverlay, "American")) == "fps overlay");
    CHECK(std::string(MenuStrings::Text(MenuStrings::Id::PerfOverlay, "Klingon")) == "fps overlay");   // English fallback
    CHECK(std::string(MenuStrings::Text(MenuStrings::Id::Display, "German")) == "anzeige");
}

}  // namespace

int main() {
    TestConstruction();
    TestRefusals();
    if (g_fail) {
        std::fprintf(stderr, "test_menu_extension: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("test_menu_extension: PASS\n");
    return 0;
}
