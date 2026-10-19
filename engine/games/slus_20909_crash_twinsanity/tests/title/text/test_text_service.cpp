#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "games/slus_20909_crash_twinsanity/tests/title/text/text_fixture.h"

namespace {
using namespace title;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)
void CaptureAndBounds() {
    TextFixture f;
    TextLayout::Item item;
    item.text = "A~bc~~de"; item.alignment = 0x11; item.x = .2f; item.y = .3f;
    f.SetItem(0, item);
    const auto ram = f.textures.ram;
    MaterialState::Environment environment;
    environment.colour_clamp = true;
    TextIntent::Output output;
    CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, output) == TextIntent::Result::Ok);
    CHECK(f.textures.ram == ram && output.pages.size() == 3 && output.source_items == 1);
    for (size_t n = 0; n < output.pages.size(); ++n) {
        const auto& page = output.pages[n];
        CHECK(page.page == n && page.bucket == 26 && page.material == f.material + n * 0x100);
        CHECK(page.sampling.asset.lease && page.sampling.asset.asset_id == 19 + n);
        for (const auto& draw : page.draws) {
            CHECK(draw.raster.frame.fbmsk == 0xff000000 && draw.use_fst && draw.tex.fst);
            CHECK(draw.tex.mmag && draw.tex.mmin == 4 && draw.tex.k == -69);
        }
    }
    const auto generation = output.pages[0].sampling.asset.token.generation;
    const auto reject = [&](TextIntent::Result expected) {
        const auto before = f.textures.ram;
        CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, output) == expected);
        CHECK(f.textures.ram == before && output.pages[0].sampling.asset.token.generation == generation);
    };
    f.textures.Put(f.font + 0x18, 4); reject(TextIntent::Result::UnsupportedFont);
    f.textures.Put(f.font + 0x18, 3);
    f.textures.Put(f.queue + 8, 257); reject(TextIntent::Result::LimitExceeded);
    f.textures.Put(f.queue + 8, 1);
    f.textures.Put(f.material + 0x60, 0); reject(TextIntent::Result::UnsupportedMaterial);
    f.textures.Put(f.material + 0x60, 2);
    f.textures.Put(f.material + 0x18, 0); reject(TextIntent::Result::UnsupportedMaterial);
    f.textures.Put(f.material + 0x18, 0x10000000);
    f.textures.Put(0x2f67f0, 4, 2); reject(TextIntent::Result::UnsupportedFont);
    f.textures.Put(0x2f67f0, 0, 2);
    f.textures.Put(f.font + 0x28, f.textures.object | 0xe0000000); reject(TextIntent::Result::InvalidMemory);
    f.textures.Put(f.font + 0x28, f.textures.object);
    f.textures.Put(f.textures.shader + 0xc, 2, 1); reject(TextIntent::Result::UnsupportedFont);
    f.textures.Put(f.textures.shader + 0xc, 0, 1);
    f.textures.Put(f.font + 8, 0x1fffff0); reject(TextIntent::Result::InvalidMemory);
    f.textures.Put(f.font + 8, f.glyphs);
    f.textures.Put(f.queue + 4, 0xe0142000); reject(TextIntent::Result::InvalidMemory);
    f.textures.Put(f.queue + 4, f.pointers);
    // The title's main thread runs on the scratchpad (TR entry.cpp:83): a
    // queued item whose string lives on that stack captures the same text
    // through the window; past the window, unaligned, or without a window
    // it is InvalidMemory (the same rule as SpriteIntent/TextureBindings).
    {
        const uint32_t stack_text = 0x70003d30;
        std::memcpy(f.textures.scratchpad.data() + (stack_text & 0x3fff), "A~bc~~de", 9);
        f.textures.Put(f.item + 0x18, stack_text);
        const auto ram_before = f.textures.ram, stack_before = f.textures.scratchpad;
        TextIntent::Output stack;
        CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, stack) == TextIntent::Result::Ok);
        CHECK(stack.pages.size() == output.pages.size() && stack.source_items == 1);
        for (size_t n = 0; n < stack.pages.size() && n < output.pages.size(); ++n)
            CHECK(stack.pages[n].draws.size() == output.pages[n].draws.size());
        CHECK(f.textures.ram == ram_before && f.textures.scratchpad == stack_before);
        CHECK(stack.pages[0].sampling.asset.token.generation == generation); // the same owned pages, not a new generation
        CHECK(TextIntent::Capture(f.Reader(false), f.textures.bindings, f.queue, environment, stack) == TextIntent::Result::InvalidMemory);
        std::memcpy(f.textures.scratchpad.data() + 0x3ffc, "xxxx", 4); // no terminator inside the window
        f.textures.Put(f.item + 0x18, 0x70003ffc); // string runs past the window
        CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, stack) == TextIntent::Result::InvalidMemory);
        f.textures.Put(f.item + 0x18, 0x70004000); // beyond the window entirely
        CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, stack) == TextIntent::Result::InvalidMemory);
        f.textures.Put(f.item + 0x18, f.strings);
        // The item object itself on the stack: the pointer table names it there.
        const uint32_t stack_item = 0x70003c00;
        std::memcpy(f.textures.scratchpad.data() + (stack_item & 0x3fff), f.textures.ram.data() + f.item, 0x1c);
        f.textures.Put(f.pointers, stack_item);
        CHECK(TextIntent::Capture(f.Reader(), f.textures.bindings, f.queue, environment, stack) == TextIntent::Result::Ok);
        CHECK(stack.source_items == 1);
        f.textures.Put(f.pointers, f.item);
    }
    item.scale_x = std::numeric_limits<float>::quiet_NaN(); f.SetItem(0, item);
    reject(TextIntent::Result::InvalidItem);
    item.scale_x = 1; item.text.assign(609, 'a'); f.SetItem(0, item);
    reject(TextIntent::Result::LimitExceeded);
    item.text.clear();
    for (int n = 0; n < 65; ++n) item.text += "a~";
    f.SetItem(0, item); reject(TextIntent::Result::LimitExceeded);
    CHECK(f.textures.assets.Release(output.pages[0].sampling.asset.token) == TextureAssets::Result::Ok);
    TextureAssets::View retained;
    CHECK(f.textures.assets.Find(output.pages[0].sampling.asset.lease, retained) == TextureAssets::Result::Ok);
    CHECK(retained.token.generation == generation);
}
void InstallationAndContinuation() {
    TextFixture f;
    std::map<uint32_t, TextService::Body> bodies;
    unsigned captured = 0, installed = 0, originals = 0;
    TextService::Hooks hooks;
    const auto capture = [&](uint32_t) -> TextService::Body {
        ++captured;
        return [&](CpuContext&, uint32_t entry) { CHECK(entry == 0x19d150); ++originals; };
    };
    const auto install = [&](uint32_t entry, TextService::Body body) {
        CHECK(captured == 4);
        ++installed; bodies[entry] = std::move(body); return true;
    };
    TextService disabled(f.textures.bindings, hooks);
    CHECK(disabled.Install(capture, install) && installed == 4);
    CpuContext context{};
    bodies.at(TextIntent::kDrawFontTexts)(context, 0x19d150);
    CHECK(originals == 1 && disabled.Counts().model_calls == 1);
    CHECK(!disabled.Install(capture, install));
    hooks.enabled = true; hooks.read = f.Reader();
    hooks.environment = [](MaterialState::Environment&) { return true; };
    unsigned selections = 0, records = 0;
    hooks.selected = [&](uint32_t) { ++selections; return true; };
    hooks.record = [&](const TextIntent::Output&) { ++records; return true; };
    TextService enabled(f.textures.bindings, hooks);
    captured = installed = 0;
    CHECK(enabled.Install(capture, install));
    // An interior entry refuses to the original continuation (ordering):
    // counted, no selection probe, nothing recorded, no fault.
    bodies.at(TextIntent::kDrawFontTexts)(context, 0x19d150);
    CHECK(selections == 0 && records == 0 && originals == 2 && enabled.Counts().failed == 0);
    CHECK(enabled.Counts().refused == 1 && enabled.Counts().model_calls == 1);
    bool threw = false;
    TextService partial(f.textures.bindings, hooks);
    captured = installed = 0;
    CHECK(!partial.Install(capture, [&](uint32_t entry, TextService::Body body) {
        install(entry, std::move(body)); return installed != 2;
    }));
    CHECK(captured == 4 && installed == 2);
    threw = false;
    try { bodies.at(TextIntent::kDrawFontTexts)(context, 0); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && records == 0 && originals == 2); // faulted (partial install): no replay, no refusal
}
void Binding() {
    TextFixture f;
    TextLayout::Item item; item.text = "abcdef";
    f.SetItem(0, item);
    TextService::Hooks hooks;
    CHECK(!hooks.enabled);
    hooks.enabled = true; hooks.read = f.Reader();
    hooks.environment = [](MaterialState::Environment&) { return true; };
    bool selected = false, accept = true, record_throws = false;
    unsigned originals = 0, records = 0, faults = 0;
    hooks.selected = [&](uint32_t object) { CHECK(object == f.font); return selected; };
    hooks.record = [&](const TextIntent::Output& output) {
        CHECK(output.pages.size() == 3);
        if (record_throws) throw std::runtime_error("record");
        if (!accept) return false;
        ++records; return true;
    };
    struct Refusal { GraphicsRefusals::Reason reason; uint32_t producer, detail; };
    std::vector<Refusal> refusals;
    hooks.refuse = [&](auto reason, uint32_t producer, uint32_t detail) { refusals.push_back({reason, producer, detail}); };
    hooks.fault = [&](auto, uint32_t, uint32_t) { ++faults; };
    TextService service(f.textures.bindings, hooks);
    std::map<uint32_t, TextService::Body> bodies;
    CHECK(service.Install([&](uint32_t) -> TextService::Body {
        return [&](CpuContext&, uint32_t) { ++originals; };
    }, [&](uint32_t address, TextService::Body body) {
        bodies[address] = std::move(body); return true;
    }));
    CpuContext context{};
    context.cpuRegs.GPR.r[5].UL[0] = f.queue;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(originals == 1 && records == 0);
    selected = true;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(service.Counts().replaced == 1 && service.Counts().model_calls == 1 && records == 1);
    f.textures.Put(f.queue + 8, 0); f.textures.Put(f.queue, 0);
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(service.Counts().empty_queues == 1 && records == 1);
    f.textures.Put(f.queue + 8, 1); f.textures.Put(f.queue, f.font);
    // Root refused the material batch before writing any list word: a counted
    // refusal, the original draws, the producer stays live.
    accept = false;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(records == 1 && service.Counts().failed == 0 && service.Counts().refused == 1 && originals == 2);
    CHECK(refusals.size() == 1 && refusals[0].reason == GraphicsRefusals::Reason::Ordering &&
          refusals[0].producer == TextIntent::kDrawFontTexts && refusals[0].detail == TextService::kRecordRefused);
    accept = true;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(records == 2 && originals == 2 && service.Counts().replaced == 2);
    // An unreadable queue pointer and a capture result refuse with their detail.
    context.cpuRegs.GPR.r[5].UL[0] = f.queue | 0x40000000;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    context.cpuRegs.GPR.r[5].UL[0] = f.queue;
    CHECK(originals == 3 && refusals.size() == 2 && refusals[1].detail == TextService::kInvalidQueue &&
          refusals[1].reason == GraphicsRefusals::Reason::UnsupportedState);
    // A throwing record is a fault after acceptance: attributed, thrown, never replayed.
    record_throws = true;
    bool threw = false;
    try { bodies.at(TextIntent::kDrawFontTexts)(context, 0); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && records == 2 && service.Counts().failed == 1 && faults == 1 && originals == 3);
    record_throws = false; threw = false;
    try { bodies.at(TextIntent::kDrawFontTexts)(context, 0); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && records == 2 && originals == 3);
    // A selected font reaching a leaf directly: ordering refusal, the original
    // leaf runs and is counted; the owned arm never batches packets.
    for (uint32_t leaf : {TextIntent::kBegin, TextIntent::kDraw, TextIntent::kEnd}) {
        TextService direct(f.textures.bindings, hooks);
        CHECK(direct.Install([&](uint32_t) -> TextService::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, TextService::Body body) { bodies[address] = std::move(body); return true; }));
        context.cpuRegs.GPR.r[4].UL[0] = f.font;
        const auto before = originals;
        bodies.at(leaf)(context, leaf);
        CHECK(originals == before + 1 && direct.Counts().failed == 0 && direct.Counts().refused == 1 &&
              direct.Counts().replaced == 0 && refusals.back().detail == TextService::kLeafWithSelectedFont &&
              refusals.back().producer == leaf);
    }
    // No frame environment: ordering refusal with kEnvironment.
    TextService::Hooks waiting = hooks;
    waiting.environment = [](MaterialState::Environment&) { return false; };
    TextService unready(f.textures.bindings, waiting);
    CHECK(unready.Install([&](uint32_t) -> TextService::Body {
        return [&](CpuContext&, uint32_t) { ++originals; };
    }, [&](uint32_t address, TextService::Body body) { bodies[address] = std::move(body); return true; }));
    const auto before = originals;
    bodies.at(TextIntent::kDrawFontTexts)(context, 0);
    CHECK(originals == before + 1 && unready.Counts().refused == 1 && refusals.back().detail == TextService::kEnvironment);
}
}
int main() {
    CaptureAndBounds(); Binding(); InstallationAndContinuation();
    std::printf("TextService: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
