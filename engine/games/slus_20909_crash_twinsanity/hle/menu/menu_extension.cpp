#include "games/slus_20909_crash_twinsanity/hle/menu/menu_extension.h"

#include <cstdio>
#include <utility>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_layout.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_text_table.h"

namespace title {
namespace {
// Boundaries a user change waits for the host value to follow before the sync
// would write the (older) host value back into the menu.
constexpr uint32_t kAwaitHostBoundaries = 180;
}  // namespace

MenuExtension::MenuExtension(MenuGuest guest) : guest_(std::move(guest)) {}

void MenuExtension::AddPage(MenuPageSpec page) { specs_.push_back(std::move(page)); }

bool MenuExtension::Install(const Capture& capture, const Installer& install) {
    if (specs_.empty() || !capture || !install) return false;
    Body original = capture(MenuLayout::kOptionsPageConstruct);
    if (!original) return false;
    return install(MenuLayout::kOptionsPageConstruct, [this, original](CpuContext& ctx, uint32_t addr) {
        const bool entry = addr == 0 || addr == MenuLayout::kOptionsPageConstruct;
        original(ctx, addr);
        if (!entry) return;
        ++counts_.constructs;
        // The title's constructor returned the page in v0; the extension's work
        // runs as further calls into the title, then every register the title's
        // caller sees is exactly the constructor's.
        const CPURegs saved_cpu = ctx.cpuRegs;
        const FPUregs saved_fpu = ctx.fpuRegs;
        Construct(ctx, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs = saved_cpu;
        ctx.fpuRegs = saved_fpu;
    });
}

uint32_t MenuExtension::Call(CpuContext& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t a[8] = {};
    size_t n = 0;
    for (uint32_t v : args) if (n < 8) a[n++] = v;
    return guest_.call(ctx, fn, a, n);
}

uint32_t MenuExtension::Allocate(CpuContext& ctx, uint32_t size) {
    return Call(ctx, MenuLayout::kMemoryAllocate, {size});
}

void MenuExtension::Construct(CpuContext& ctx, uint32_t options_page) {
    if (state_ == State::Registered) return;   // a second construction (none known) keeps the first
    built_.clear();
    const auto refuse = [this](const char* why) {
        state_ = State::Refused;
        refusal_ = why;
        built_.clear();
    };
    if (!MenuLayout::IsRam(options_page)) return refuse("options page outside RAM");
    if (guest_.read32(options_page + MenuLayout::kPageVTable) != MenuLayout::kOptionsPageVTable)
        return refuse("options page vtable mismatch");
    const MenuTextTable::Result texts = MenuTextTable::Extend(guest_, ctx);
    if (!texts.ok) return refuse(texts.refusal.empty() ? "text table" : texts.refusal.c_str());
    text_base_ = texts.base;
    counts_.texts = MenuStrings::kCount;
    counts_.banks = texts.banks;
    uint32_t next_id = MenuLayout::kFirstExtensionItemId;
    first_link_id_ = next_id;
    for (const MenuPageSpec& spec : specs_) {
        BuiltPage built;
        if (BuildPage(ctx, spec, options_page, built, next_id) == 0) return refuse("page construction failed");
        built_.push_back(std::move(built));
    }
    InsertLinks(ctx, options_page);
    options_page_ = options_page;
    state_ = State::Registered;
    refusal_.clear();
}

uint32_t MenuExtension::BuildPage(CpuContext& ctx, const MenuPageSpec& spec, uint32_t options_page, BuiltPage& built,
                                  uint32_t& next_id) {
    const uint32_t page = Allocate(ctx, MenuLayout::kPageSize);
    if (!MenuLayout::IsRam(page)) return 0;
    const uint32_t title = MenuTextTable::Index(text_base_, spec.title);
    if (title > MenuLayout::kMaxTitleText) return 0;
    Call(ctx, MenuLayout::kMenuPageConstructTitled, {page, title, MenuLayout::kMenuPlayers});
    guest_.write32(page + MenuLayout::kPageParent, options_page);
    built.page = page;
    for (const MenuWidget& w : spec.widgets) {
        const uint32_t item = BuildWidget(ctx, w, next_id++);
        if (!MenuLayout::IsRam(item)) return 0;
        Call(ctx, MenuLayout::kMenuPageAdd, {page, item});
        BuiltWidget bw;
        bw.item = item;
        bw.spec = &w;
        bw.last_guest = w.read ? w.read() : 0;
        guest_.write32(item + MenuLayout::kValueItemValues, static_cast<uint32_t>(bw.last_guest));
        built.widgets.push_back(bw);
        ++counts_.items;
    }
    const uint32_t back = Allocate(ctx, MenuLayout::kLinkItemSize);
    if (!MenuLayout::IsRam(back)) return 0;
    Call(ctx, MenuLayout::kBackItemConstruct,
         {back, MenuLayout::kBackText, MenuLayout::kSubPageBackItem, options_page, MenuLayout::kMenuPlayers});
    Call(ctx, MenuLayout::kMenuPageAdd, {page, back});
    // The Options link to this page (added to the Options page by InsertLinks).
    const uint32_t link = Allocate(ctx, MenuLayout::kLinkItemSize);
    if (!MenuLayout::IsRam(link)) return 0;
    Call(ctx, MenuLayout::kLinkItemConstruct, {link, title, next_id++, page, MenuLayout::kMenuPlayers});
    built.link = link;
    ++counts_.pages;
    return page;
}

uint32_t MenuExtension::BuildWidget(CpuContext& ctx, const MenuWidget& spec, uint32_t id) {
    const uint32_t label = MenuTextTable::Index(text_base_, spec.label);
    if (label > MenuLayout::kMaxItemText) return 0;
    const uint32_t wraps = spec.wraps ? 1u : 0u;
    const uint32_t value = static_cast<uint32_t>(spec.read ? spec.read() : 0);
    switch (spec.kind) {
        case MenuWidget::Kind::Toggle: {
            const uint32_t item = Allocate(ctx, MenuLayout::kChoiceItemSize);
            if (!MenuLayout::IsRam(item)) return 0;
            Call(ctx, MenuLayout::kToggleItemConstruct,
                 {item, label, id, MenuLayout::kOffText, MenuLayout::kOnText, wraps, value & 1u, MenuLayout::kMenuPlayers});
            return item;
        }
        case MenuWidget::Kind::Enum:
        case MenuWidget::Kind::KeyBinding: {
            if (spec.choices.empty()) return 0;
            const uint32_t item = Allocate(ctx, MenuLayout::kChoiceItemSize);
            if (!MenuLayout::IsRam(item)) return 0;
            const uint32_t choices = static_cast<uint32_t>(spec.choices.size());
            Call(ctx, MenuLayout::kChoiceItemConstruct,
                 {item, label, id, choices, wraps, value < choices ? value : 0u, MenuLayout::kMenuPlayers});
            for (uint32_t c = 0; c < choices; ++c)
                Call(ctx, MenuLayout::kChoiceItemSetText, {item, c, MenuTextTable::Index(text_base_, spec.choices[c])});
            return item;
        }
        case MenuWidget::Kind::Slider: {
            const uint32_t item = Allocate(ctx, MenuLayout::kValueItemSize);
            if (!MenuLayout::IsRam(item)) return 0;
            Call(ctx, MenuLayout::kValueItemConstruct,
                 {item, label, id, static_cast<uint32_t>(spec.minimum), static_cast<uint32_t>(spec.maximum), wraps, value,
                  MenuLayout::kMenuPlayers});
            return item;
        }
    }
    return 0;
}

// The links go in FRONT of the title's back item: Add() appends, so the back
// item (the title's last) is moved to the end afterwards.
void MenuExtension::InsertLinks(CpuContext& ctx, uint32_t options_page) {
    const uint32_t before = guest_.read32(options_page + MenuLayout::kPageItemsCount);
    for (const BuiltPage& b : built_) Call(ctx, MenuLayout::kMenuPageAdd, {options_page, b.link});
    const uint32_t data = guest_.read32(options_page + MenuLayout::kPageItemsData);
    const uint32_t after = guest_.read32(options_page + MenuLayout::kPageItemsCount);
    if (before == 0 || after != before + built_.size() || !MenuLayout::IsRam(data)) return;
    const uint32_t back = guest_.read32(data + (before - 1) * 4);
    for (uint32_t i = before - 1; i + 1 < after; ++i) guest_.write32(data + i * 4, guest_.read32(data + (i + 1) * 4));
    guest_.write32(data + (after - 1) * 4, back);
}

bool MenuExtension::Validate() {
    ++counts_.revalidations;
    if (!MenuLayout::IsRam(options_page_) ||
        guest_.read32(options_page_ + MenuLayout::kPageVTable) != MenuLayout::kOptionsPageVTable)
        return false;
    const uint32_t count = guest_.read32(options_page_ + MenuLayout::kPageItemsCount);
    const uint32_t data = guest_.read32(options_page_ + MenuLayout::kPageItemsData);
    if (!MenuLayout::IsRam(data) || count > 64) return false;
    // Our pages' link items, with their ids, must still be on the Options page.
    for (const BuiltPage& b : built_) {
        bool found = false;
        for (uint32_t i = 0; i < count && !found; ++i) found = guest_.read32(data + i * 4) == b.link;
        if (!found || !MenuLayout::IsRam(b.page)) return false;
        for (const BuiltWidget& w : b.widgets)
            if (!MenuLayout::IsRam(w.item) || MenuLayout::ItemId(guest_.read32(w.item + MenuLayout::kItemBits)) < MenuLayout::kFirstExtensionItemId)
                return false;
    }
    return true;
}

void MenuExtension::NoteRestored() { revalidate_ = true; }

void MenuExtension::OnBoundary() {
    if (state_ != State::Registered && state_ != State::Dormant) return;
    if (revalidate_) {
        revalidate_ = false;
        state_ = Validate() ? State::Registered : State::Dormant;
        if (state_ == State::Dormant) {
            refusal_ = "restored image lacks the extension's pages";
        } else {
            // The settings are the host's (the settings file), never the image's:
            // the restored items take the live values without an apply.
            refusal_.clear();
            for (BuiltPage& b : built_)
                for (BuiltWidget& w : b.widgets) {
                    const int32_t host = w.spec->read ? w.spec->read() : w.last_guest;
                    guest_.write32(w.item + MenuLayout::kValueItemValues, static_cast<uint32_t>(host));
                    w.last_guest = host;
                    w.awaiting_host = false;
                }
        }
    }
    if (state_ != State::Registered) return;
    ++counts_.boundaries;
    for (BuiltPage& b : built_) {
        for (BuiltWidget& w : b.widgets) {
            const int32_t guest = static_cast<int32_t>(guest_.read32(w.item + MenuLayout::kValueItemValues));
            if (guest != w.last_guest) {
                // The user stepped it in the title's menu.
                w.last_guest = guest;
                w.awaiting_host = true;
                w.awaiting_since = static_cast<uint32_t>(counts_.boundaries);
                ++counts_.changes;
                if (w.spec->apply) w.spec->apply(guest);
                continue;
            }
            if (!w.spec->read) continue;
            const int32_t host = w.spec->read();
            if (w.awaiting_host) {
                if (host == guest || counts_.boundaries - w.awaiting_since > kAwaitHostBoundaries) w.awaiting_host = false;
                continue;
            }
            if (host != guest) {
                // Changed outside the menu (the CLI, the launcher, F1): the item follows.
                guest_.write32(w.item + MenuLayout::kValueItemValues, static_cast<uint32_t>(host));
                w.last_guest = host;
                ++counts_.pushes;
            }
        }
    }
}

uint32_t MenuExtension::ItemAddress(size_t p, size_t w) const {
    if (p >= built_.size() || w >= built_[p].widgets.size()) return 0;
    return built_[p].widgets[w].item;
}

std::string MenuExtension::CensusLine() const {
    const char* state = state_ == State::Registered ? "registered" : state_ == State::Dormant ? "dormant"
                        : state_ == State::Refused ? "refused" : "unregistered";
    std::string line = std::string("state=") + state + ",constructs=" + std::to_string(counts_.constructs) +
                       ",pages=" + std::to_string(counts_.pages) + ",items=" + std::to_string(counts_.items) +
                       ",texts=" + std::to_string(counts_.texts) + ",banks=" + std::to_string(counts_.banks) +
                       ",text_base=" + std::to_string(text_base_) + ",boundaries=" + std::to_string(counts_.boundaries) +
                       ",changes=" + std::to_string(counts_.changes) + ",pushes=" + std::to_string(counts_.pushes) +
                       ",revalidations=" + std::to_string(counts_.revalidations);
    if (!refusal_.empty()) line += ",refusal=" + refusal_;
    return line;
}

void MenuExtension::Record(ps2r::verify::RunMeta& meta) const {
    meta.Set("menu_ext", CensusLine());
    std::string pages;
    for (size_t p = 0; p < built_.size(); ++p) {
        if (p) pages += ";";
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%x", built_[p].page);
        pages += "page" + std::to_string(p) + "=0x" + hex;
        for (size_t w = 0; w < built_[p].widgets.size(); ++w)
            pages += "," + built_[p].widgets[w].spec->key + "=" + std::to_string(built_[p].widgets[w].last_guest);
    }
    meta.Set("menu_ext_pages", pages);
}

}  // namespace title
