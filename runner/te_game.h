// te_game.h -- the game fiber (runner side): the wasm2c module runs on its own fixed-address stack (te_fiber.h) and
// yields to the host at three points: a save-state checkpoint it was asked to stop at, Platform::System::Exit, and the
// end of entry (return or trap). The host loop in main() services the reason and switches back (or stops).
#pragma once
#include <cstdint>
#include <string>

#include "te_fiber.h"

struct w2c_game;

namespace te::game {

enum class Reason : int { Running = 0, Checkpoint, Exit, Trap, Returned, LoadState };   // LoadState: restore a state file in place at the checkpoint

struct Fiber {
    te_fiber_ctx host{};
    te_fiber_ctx game{};
    uint8_t* stack = nullptr;
    size_t stack_size = 0;
    bool at_hint = false;
    bool started = false;      // the fiber ran at least once (or was loaded)
    bool on_game = false;      // the game fiber is the running one
    Reason reason = Reason::Running;
    int code = 0;              // Exit status / trap code
    uint64_t boundary = 0;     // the checkpoint's boundary
    w2c_game* instance = nullptr;
    bool test_trap = false;
};

// The stack's fixed address and size. 0x3100_0000_0000 is outside every mapping macOS/Linux
// make on their own; 32 MB covers wasm2c's 500-deep call budget with room (committed lazily by the kernel).
constexpr uint64_t kStackHint = 0x310000000000ull;
constexpr size_t kStackSize = 32u << 20;

Fiber& Get();
// Maps the stack and prepares the entry (te_run_entry on the fiber). False with *err when the stack cannot be mapped.
bool Prepare(w2c_game* instance, bool test_trap, std::string* err);
// Host side: run the game until it yields; returns the reason.
Reason Resume();
// Game side: yield with a reason (never returns for Exit).
void Yield(Reason reason, int code = 0, uint64_t boundary = 0);
// Game side: Platform::System::Exit's path (te::RequestExit). Returns only when not on the game fiber.
void ExitFromGame(int status);

}  // namespace te::game
