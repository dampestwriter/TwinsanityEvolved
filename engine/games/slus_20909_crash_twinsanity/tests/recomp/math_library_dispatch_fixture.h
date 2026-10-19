#pragma once

#include "engine/ee/cpu/dispatch_table.h"

// Narrow generated-table storage for tests of the real DispatchTable wrapper.
// No game bodies, model arithmetic or expected oracle results live here.
class MathLibraryDispatchFixture {
public:
    static void Reset();
    static void Bind(uint32_t address, DispatchTable::Body body);
};
