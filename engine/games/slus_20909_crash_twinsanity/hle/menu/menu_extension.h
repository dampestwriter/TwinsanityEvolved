#pragma once
// MenuExtension -- the owned extension of the title's options menu:
// the added 'Display' and 'Controls' pages, built from
// the title's OWN page/item structures (MenuLayout) with the title's own
// constructors and allocator, registered with the title's page table the way
// the title's own option subpages are -- at OptionsPage::Construct (NTSC
// 0x1646c0), where the title constructs Graphics/Sound/Game and Add()s their
// links. The extension's body runs the title's constructor to completion,
// verifies the page it built (its vtable word), then Add()s one LinkItem per
// added page in front of the title's back item; each added page is a
// MenuPage::ConstructTitled with parent = the Options page, its widgets
// ToggleItem/ChoiceItem/ValueItem items and a BackItem to the Options page.
// The title's MenuPage::Step navigates them, its items step the values, its
// MenuDrawer draws them with the menu font -- no host code runs for the
// menu's input or drawing. The labels are added to the title's text table
// (MenuTextTable) in every loaded language bank.
//
// The host side is a value SYNC at every frame boundary (OnBoundary, the EE
// thread): a widget's guest value (ValueItem::values[0]) that changed since
// the last boundary was changed by the user through the title's menu -> the
// widget's `apply` runs (the owner applies it live and persists it); a host
// value (`read`) that differs from the guest value -- the setting changed
// from the CLI/launcher -- is written into the item. Nothing else is hooked:
// no menu input, no draw, no page entry.
//
// Savestates: the pages live in the title's RAM and travel with it. After a
// restore the extension re-validates its Options page address (the vtable
// word and its own first link's id); a restored image without the extension's
// pages (an older anchor) leaves the extension DORMANT, reported in the census
// (run.meta menu_ext=... state=dormant), never faulting.
//
// One-line toggle: Config ui.menu_pages (the composition installs the
// extension in PLAY mode only -- a headless or tick-synced leg never builds
// the pages, so the deterministic lanes are unchanged by construction).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_guest.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_widget.h"

namespace ps2r::verify { class RunMeta; }

namespace title {

class MenuExtension {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t addr)>;
    using Installer = std::function<bool(uint32_t addr, Body body)>;

    enum class State : uint8_t { Unregistered, Registered, Dormant, Refused };

    struct Counters {
        uint64_t constructs = 0;     // OptionsPage::Construct entries seen
        uint64_t pages = 0;          // added pages built
        uint64_t items = 0;          // added widgets built (back items excluded)
        uint64_t texts = 0;          // strings added per bank
        uint64_t banks = 0;          // language banks extended
        uint64_t boundaries = 0;     // OnBoundary calls with a live registration
        uint64_t changes = 0;        // user changes applied from the menu
        uint64_t pushes = 0;         // host values written into the menu
        uint64_t revalidations = 0;  // registration re-checks after a restore
    };

    explicit MenuExtension(MenuGuest guest);

    // Declares a page (before Install). Pages appear on the Options page in
    // declaration order, before the title's back item.
    void AddPage(MenuPageSpec page);
    // Wraps the title's Options page construction. False when the address is
    // not a function start or nothing was declared.
    bool Install(const Capture& capture, const Installer& install);
    // Per frame boundary on the EE thread: the value sync.
    void OnBoundary();
    // After a savestate restore: forget nothing, re-validate at the next boundary.
    void NoteRestored();

    State GetState() const { return state_; }
    const std::string& Refusal() const { return refusal_; }
    const Counters& Counts() const { return counts_; }
    uint32_t OptionsPage() const { return options_page_; }
    // The guest address of widget `w` of page `p` (0 when not built).
    uint32_t ItemAddress(size_t p, size_t w) const;
    // The text index base the strings received (0 before registration).
    uint32_t TextBase() const { return text_base_; }
    // run.meta rows: menu_ext=state=<..>,pages=,items=,texts=,banks=,changes=,pushes=,...
    void Record(ps2r::verify::RunMeta& meta) const;
    std::string CensusLine() const;

private:
    struct BuiltWidget {
        uint32_t item = 0;
        int32_t last_guest = 0;
        bool awaiting_host = false;
        uint32_t awaiting_since = 0;
        const MenuWidget* spec = nullptr;
    };
    struct BuiltPage {
        uint32_t page = 0;
        uint32_t link = 0;
        std::vector<BuiltWidget> widgets;
    };

    void Construct(CpuContext& ctx, uint32_t options_page);
    uint32_t BuildPage(CpuContext& ctx, const MenuPageSpec& spec, uint32_t options_page, BuiltPage& built, uint32_t& next_id);
    uint32_t BuildWidget(CpuContext& ctx, const MenuWidget& spec, uint32_t id);
    void InsertLinks(CpuContext& ctx, uint32_t options_page);
    bool Validate();
    uint32_t Call(CpuContext& ctx, uint32_t fn, std::initializer_list<uint32_t> args);
    uint32_t Allocate(CpuContext& ctx, uint32_t size);

    MenuGuest guest_;
    std::vector<MenuPageSpec> specs_;
    std::vector<BuiltPage> built_;
    State state_ = State::Unregistered;
    std::string refusal_;
    Counters counts_;
    uint32_t options_page_ = 0;
    uint32_t text_base_ = 0;
    uint32_t first_link_id_ = 0;
    bool revalidate_ = false;
};

}  // namespace title
