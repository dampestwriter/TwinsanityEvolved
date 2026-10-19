#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"

#include <array>
#include <map>
#include <utility>

#include "engine/ee/cpu/shadow_call_stack.h"
#include "games/slus_20909_crash_twinsanity/recomp/continuations.h"

std::map<uint32_t, DispatchTable::Body> recompiled_functions;
extern const uint32_t g_recomp_fn_count = 0;
extern const uint32_t g_recomp_fn_addrs[] = {0};
bool g_recomp_fn_hooked[] = {false};
extern "C" { bool crashring_enabled = false; }
void initialize_recompiled_functions() {}
uint32_t recomp_fn_id_for(uint32_t) { return UINT32_MAX; }
bool update_function_impl(uint32_t address, const DispatchTable::Body& body) {
    const auto found = recompiled_functions.find(address);
    if (found == recompiled_functions.end()) return false;
    found->second = DispatchTable::ObserveInstall(address, body);
    return true;
}

namespace {
thread_local std::array<ShadowCallStack::Frame, 128> frames;
thread_local uint32_t depth = 0;
}
extern "C" uint32_t ee_get_call_stack_depth() { return depth; }
extern "C" bool ee_get_call_stack_top(uint32_t n, uint32_t* address, uint32_t* ra) {
    if (n >= depth) return false;
    *address = frames[depth - n - 1].func_addr;
    *ra = frames[depth - n - 1].ra;
    return true;
}
extern "C" bool ee_call_stack_push(uint32_t address, uint32_t ra) {
    if (depth == frames.size()) {
        ShadowCallStack::RejectedPush();
        return false;
    }
    frames[depth++] = {address, ra};
    return true;
}
extern "C" void ee_call_stack_pop() {
    if (depth) --depth;
    if (!depth) ShadowCallStack::StackEmptied();
}

void MathLibraryDispatchFixture::Reset() {
    recompiled_functions.clear();
    depth = 0;
    ShadowCallStack::StackEmptied();
}
void MathLibraryDispatchFixture::Bind(uint32_t address, DispatchTable::Body body) {
    recompiled_functions[address] =
        DispatchTable::GeneratedBody(address, std::move(body), recomp_continuations(address));
}
