// te_game.cpp -- see te_game.h.
#include "te_game.h"

#include <cstdio>

#include "te_run.h"

namespace te::game {

namespace {
void FiberMain(void*) {
    Fiber& f = Get();
    const int trap = te_run_entry(f.instance, f.test_trap ? 1 : 0);
    if (trap < 0) Yield(Reason::Returned, 0);
    else Yield(Reason::Trap, trap);
    // never resumed
    std::fprintf(stderr, "[twinsanityevolved] fatal: a finished game fiber was resumed\n");
    std::abort();
}
}  // namespace

Fiber& Get() {
    static Fiber fiber;
    return fiber;
}

bool Prepare(w2c_game* instance, bool test_trap, std::string* err) {
    Fiber& f = Get();
    if (!f.stack) {
        int at_hint = 0;
        f.stack = static_cast<uint8_t*>(te_fiber_map_stack(kStackHint, kStackSize, &at_hint));
        if (!f.stack) { if (err) *err = "cannot map the game stack"; return false; }
        f.stack_size = kStackSize;
        f.at_hint = at_hint != 0;
    }
    f.instance = instance;
    f.test_trap = test_trap;
    te_fiber_prepare(&f.game, f.stack, f.stack_size, FiberMain, nullptr);
    return true;
}

Reason Resume() {
    Fiber& f = Get();
    f.reason = Reason::Running;
    f.on_game = true;
    f.started = true;
    te_fiber_switch(&f.host, &f.game);
    f.on_game = false;
    return f.reason;
}

void Yield(Reason reason, int code, uint64_t boundary) {
    Fiber& f = Get();
    f.reason = reason;
    f.code = code;
    f.boundary = boundary;
    te_fiber_switch(&f.game, &f.host);
}

void ExitFromGame(int status) {
    Fiber& f = Get();
    if (!f.on_game) return;
    Yield(Reason::Exit, status);
}

}  // namespace te::game
