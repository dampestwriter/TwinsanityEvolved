#pragma once
// The title's VU0 macro-program table (and the one VU1 dispatcher entry +
// subroutine the census names): entry pc, disposition and, where captured,
// the 16-byte fingerprint -- title data handed to the engine's Vu0Registry.
// The libsin/cos row's fingerprint is the SDK bundle pair LibVu0SinCos keys
// on (the substitution itself is the engine's, not this table's).
#include "engine/vu/vu0/vu0_registry.h"

class Vu0Programs {
public:
    // Add every entry / subroutine row to `registry` (table order).
    static void Register(Vu0Registry& registry);

    static const Vu0Registry::Descriptor* Entries(size_t& count);
    static const Vu0Registry::Descriptor* Subroutines(size_t& count);
};
