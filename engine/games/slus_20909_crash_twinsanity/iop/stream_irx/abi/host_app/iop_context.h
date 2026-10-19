#pragma once
// Generated-code ABI forwarder: the `IopContext` the recompiled STREAM.IRX TU
// runs on is the engine's R3000 register file; `iop_fix_zero` is its $0 rule.
#include "engine/iop/iop_context.h"

using IopContext = ps2r::iop::IopContext;

inline void iop_fix_zero(IopContext& ctx) { ctx.FixZero(); }
